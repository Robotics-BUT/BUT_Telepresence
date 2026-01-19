import subprocess
import threading
import connexion
import json
import os

from swagger_server.models import StreamConfiguration, Apiv1streamupdateResolution
from swagger_server.models.inline_response200 import InlineResponse200  # noqa: E501
from swagger_server.models.inline_response2001 import InlineResponse2001  # noqa: E501
from swagger_server.models.inline_response2002 import InlineResponse2002  # noqa: E501
from swagger_server.models.inline_response500 import InlineResponse500  # noqa: E501
from swagger_server.models.inline_response5001 import InlineResponse5001  # noqa: E501
from swagger_server.models.inline_response5002 import InlineResponse5002  # noqa: E501
from swagger_server.models.inline_response5003 import InlineResponse5003  # noqa: E501
from swagger_server.models.required_stream_configuration import RequiredStreamConfiguration  # noqa: E501
from swagger_server.models.stream_state import StreamState  # noqa: E501
from swagger_server.models.stream_update_body import StreamUpdateBody  # noqa: E501
from swagger_server import util

# This object represents the current state and is mutated by the setter endpoints
stream_state = None
is_streaming = False
# Get absolute path relative to this script's location
_script_dir = os.path.dirname(os.path.abspath(__file__))
exec_path = os.path.abspath(os.path.join(_script_dir, "../../../streaming_driver/build/telepresence_streaming_driver"))
process = None
streaming_thread = None

# Lock to synchronize access to global state across threads
state_lock = threading.Lock()


def cfg_dict_from_state(s: dict) -> dict:
    # Match the keys to what C++ ConfigFromJson expects
    return {
        "ip": s["ip_address"],
        "portLeft": int(s["port_left"]),
        "portRight": int(s["port_right"]),
        "codec": s["codec"],  # "JPEG"/"H264"/"H265"
        "encodingQuality": int(s["encoding_quality"]),
        "bitrate": int(s["bitrate"]),
        "horizontalResolution": int(s["resolution"]["width"]),
        "verticalResolution": int(s["resolution"]["height"]),
        "videoMode": s["video_mode"],  # "mono"/"stereo"
        "fps": int(s["fps"]),
    }


def stdout_reader_thread(process):
    """Background thread that continuously drains stdout to prevent pipe blocking."""
    try:
        for line in iter(process.stdout.readline, ""):
            print(line, end="")
    except Exception as e:
        print(f"Stdout reader error: {e}")
    finally:
        if process.stdout:
            process.stdout.close()


def run_streaming_process():
    global stream_state, is_streaming, process

    with state_lock:
        if is_streaming and process:
            print("Stream is already running, reconfiguring")
            configure_streaming_process()
            return

        print("Starting streaming process!")
        is_streaming = True

        process = subprocess.Popen(
            [exec_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,  # Ensures the output is in string format rather than bytes
            bufsize=1,  # Line-buffered output
        )

    # Start dedicated thread for reading stdout (prevents pipe blocking)
    stdout_thread = threading.Thread(target=stdout_reader_thread, args=(process,), daemon=True)
    stdout_thread.start()

    # Send initial config immediately after start
    configure_streaming_process()

    # Wait for process to exit (don't block on stdout reading)
    process.wait()

    with state_lock:
        is_streaming = False
    print("The streaming process has ended")


def configure_streaming_process():
    global stream_state, is_streaming, process

    with state_lock:
        if not is_streaming or process is None or process.stdin is None:
            print("Cannot configure streaming - streaming is not running")
            return False

        msg = {"cmd": "update", "config": cfg_dict_from_state(stream_state)}

        try:
            process.stdin.write(json.dumps(msg) + "\n")
            process.stdin.flush()
            print("Configuration sent to streaming process")
            return True
        except (BrokenPipeError, IOError, OSError) as e:
            print(f"Failed to send configuration - pipe broken: {e}")
            is_streaming = False
            return False


def api_v1_stream_start_post(body):
    global stream_state, is_streaming, streaming_thread

    if not connexion.request.is_json:
        return "Missing body!"

    with state_lock:
        if is_streaming:
            return {"error": "Stream is already running. Use /update to reconfigure or /stop first."}

        stream_state = connexion.request.get_json()
        RequiredStreamConfiguration.from_dict(stream_state)

        streaming_thread = threading.Thread(target=run_streaming_process, daemon=True)
        streaming_thread.start()
        return stream_state


def api_v1_stream_state_get():  # noqa: E501
    global stream_state, is_streaming, process

    with state_lock:
        # Start from last requested/known config; if none, return defaults that satisfy StreamState shape.
        if stream_state is None:
            return {
                "ip_address": "192.168.1.100",
                "port_left": 8554,
                "port_right": 8556,
                "codec": "JPEG",
                "encoding_quality": 85,
                "bitrate": 4000000,
                "resolution": {"width": 1920, "height": 1080},
                "video_mode": "stereo",
                "fps": 60,
                "is_streaming": False,
            }

        # If the subprocess died unexpectedly, reflect that in is_streaming
        alive = (process is not None and process.poll() is None)  # None means still running

        state = dict(stream_state)  # copy
        state["is_streaming"] = bool(is_streaming and alive)
        return state


def api_v1_stream_stop_post():
    global is_streaming, streaming_thread, process

    with state_lock:
        if not is_streaming or process is None:
            return "Stream already stopped!"

        try:
            if process.stdin:
                process.stdin.write(json.dumps({"cmd": "stop"}) + "\n")
                process.stdin.flush()
        except Exception as e:
            print(f"Failed to send stop command: {e}")

        process.terminate()

    # Join thread outside lock to avoid deadlock
    if streaming_thread:
        streaming_thread.join(timeout=2.0)
    return "Stopped"


def api_v1_stream_update_put(body):
    global stream_state, is_streaming, streaming_thread

    if not connexion.request.is_json:
        return "Missing body!"

    new_config = connexion.request.get_json()

    # Validate the new config before applying it
    try:
        StreamUpdateBody.from_dict(new_config)
    except Exception as e:
        return {"error": f"Invalid configuration: {str(e)}"}

    should_configure = False
    should_start = False

    with state_lock:
        # Merge new config with existing state (update only provided fields)
        if stream_state is None:
            stream_state = new_config
        else:
            stream_state.update(new_config)

        if is_streaming:
            should_configure = True
        else:
            should_start = True

    # Call configure_streaming_process OUTSIDE the lock to avoid deadlock
    if should_configure:
        configure_streaming_process()
        return stream_state

    if should_start:
        streaming_thread = threading.Thread(target=run_streaming_process, daemon=True)
        streaming_thread.start()

    return stream_state
