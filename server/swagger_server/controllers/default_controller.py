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
video_state = None
is_video_running = False
# Get absolute path relative to this script's location
_script_dir = os.path.dirname(os.path.abspath(__file__))
video_exec_path = os.path.abspath(os.path.join(_script_dir, "../../../video_driver/build/telepresence_video_driver"))
video_process = None
video_thread = None

# Lock to synchronize access to global state across threads
video_state_lock = threading.Lock()


def cfg_dict_from_video_state(s: dict) -> dict:
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
        "videoMode": s["video_mode"],  # "mono"/"stereo"/"panoramic"
        "fps": int(s["fps"]),
    }


def stdout_reader_thread(proc):
    """Background thread that continuously drains stdout to prevent pipe blocking.
    Shared by both the video and audio subprocesses."""
    try:
        for line in iter(proc.stdout.readline, ""):
            print(line, end="")
    except Exception as e:
        print(f"Stdout reader error: {e}")
    finally:
        if proc.stdout:
            proc.stdout.close()


def run_video_process():
    global video_state, is_video_running, video_process

    with video_state_lock:
        if is_video_running and video_process:
            print("Stream is already running, reconfiguring")
            configure_video_process()
            return

        print("Starting video stream!")
        is_video_running = True

        video_process = subprocess.Popen(
            [video_exec_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,  # Ensures the output is in string format rather than bytes
            bufsize=1,  # Line-buffered output
        )

    # Start dedicated thread for reading stdout (prevents pipe blocking)
    stdout_thread = threading.Thread(target=stdout_reader_thread, args=(video_process,), daemon=True)
    stdout_thread.start()

    # Send initial config immediately after start
    configure_video_process()

    # Wait for video_process to exit (don't block on stdout reading)
    video_process.wait()

    with video_state_lock:
        is_video_running = False
    print("The video stream has ended")


def configure_video_process():
    global video_state, is_video_running, video_process

    with video_state_lock:
        if not is_video_running or video_process is None or video_process.stdin is None:
            print("Cannot configure video - video is not running")
            return False

        msg = {"cmd": "update", "config": cfg_dict_from_video_state(video_state)}

        try:
            video_process.stdin.write(json.dumps(msg) + "\n")
            video_process.stdin.flush()
            print("Configuration sent to video process")
            return True
        except (BrokenPipeError, IOError, OSError) as e:
            print(f"Failed to send configuration - pipe broken: {e}")
            is_video_running = False
            return False


def api_v1_video_start_post(body):
    global video_state, is_video_running, video_thread

    if not connexion.request.is_json:
        return "Missing body!"

    with video_state_lock:
        if is_video_running:
            return {"error": "Stream is already running. Use /update to reconfigure or /stop first."}

        video_state = connexion.request.get_json()
        RequiredStreamConfiguration.from_dict(video_state)

        video_thread = threading.Thread(target=run_video_process, daemon=True)
        video_thread.start()
        return video_state


def api_v1_video_state_get():  # noqa: E501
    global video_state, is_video_running, video_process

    with video_state_lock:
        # Start from last requested/known config; if none, return defaults that satisfy StreamState shape.
        if video_state is None:
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

        # If the subprocess died unexpectedly, reflect that in is_video_running
        alive = (video_process is not None and video_process.poll() is None)  # None means still running

        state = dict(video_state)  # copy
        state["is_streaming"] = bool(is_video_running and alive)
        return state


def api_v1_video_stop_post():
    global is_video_running, video_thread, video_process

    with video_state_lock:
        if not is_video_running or video_process is None:
            return "Stream already stopped!"

        try:
            if video_process.stdin:
                video_process.stdin.write(json.dumps({"cmd": "stop"}) + "\n")
                video_process.stdin.flush()
        except Exception as e:
            print(f"Failed to send stop command: {e}")

        video_process.terminate()

    # Join thread outside lock to avoid deadlock
    if video_thread:
        video_thread.join(timeout=2.0)
    return "Stopped"


def api_v1_video_update_put(body):
    global video_state, is_video_running, video_thread

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

    with video_state_lock:
        # Merge new config with existing state (update only provided fields)
        if video_state is None:
            video_state = new_config
        else:
            video_state.update(new_config)

        if is_video_running:
            should_configure = True
        else:
            should_start = True

    # Call configure_video_process OUTSIDE the lock to avoid deadlock
    if should_configure:
        configure_video_process()
        return video_state

    if should_start:
        video_thread = threading.Thread(target=run_video_process, daemon=True)
        video_thread.start()

    return video_state


# ---------------------------------------------------------------------------
# Audio bridge — optional, fully independent of the video stream above. Manages
# the standalone audio_driver subprocess exactly like the video driver, but
# with its own state/process/lock so audio can never disturb video.
# ---------------------------------------------------------------------------
audio_state = None
audio_is_running = False
audio_process = None
audio_thread = None
audio_exec_path = os.path.abspath(
    os.path.join(_script_dir, "../../../audio_driver/build/telepresence_audio_driver"))
audio_lock = threading.Lock()


def _detect_usb_audio(kind):
    """Return the PulseAudio sink/source name containing 'usb' (the USB DAC / Rode mic),
    or '' if none. Lets the robot pin audio to the USB devices regardless of the
    flapping default sink (the headset doesn't know robot device names)."""
    try:
        env = dict(os.environ)
        env.setdefault("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")
        out = subprocess.run(["pactl", "list", "short", kind], capture_output=True,
                             text=True, env=env, timeout=3).stdout
        for line in out.splitlines():
            parts = line.split("\t")
            if len(parts) >= 2 and "usb" in parts[1].lower() and "monitor" not in parts[1].lower():
                return parts[1]
    except Exception as e:
        print(f"USB audio detect ({kind}) failed: {e}")
    return ""


def configure_audio_process():
    """Send the current audio_state to the audio_driver on stdin (keys pass through
    unchanged — the driver reads the same snake_case keys as the REST schema).
    Auto-fills the USB sink/source so playback/capture pin to the USB devices."""
    global audio_state, audio_is_running, audio_process

    with audio_lock:
        if not audio_is_running or audio_process is None or audio_process.stdin is None:
            print("Cannot configure audio - audio is not running")
            return False
        cfg = dict(audio_state)
        if not cfg.get("playback_device"):
            dev = _detect_usb_audio("sinks")
            if dev:
                cfg["playback_device"] = dev
                print(f"Audio: pinned playback to USB sink {dev}")
        if not cfg.get("capture_device"):
            src = _detect_usb_audio("sources")
            if src:
                cfg["capture_device"] = src
                print(f"Audio: pinned capture to USB source {src}")
        msg = {"cmd": "update", "config": cfg}
        try:
            audio_process.stdin.write(json.dumps(msg) + "\n")
            audio_process.stdin.flush()
            print("Configuration sent to audio process")
            return True
        except (BrokenPipeError, IOError, OSError) as e:
            print(f"Failed to send audio configuration - pipe broken: {e}")
            audio_is_running = False
            return False


def run_audio_process():
    global audio_state, audio_is_running, audio_process

    with audio_lock:
        if audio_is_running and audio_process:
            print("Audio is already running, reconfiguring")
            return  # reconfigure handled by caller via configure_audio_process()

        print("Starting audio process!")
        audio_is_running = True

        # The audio bridge talks to PipeWire/PulseAudio (pulsesrc/pulsesink). As a
        # systemd system service we lack the user session env, so point it at the
        # defuser PipeWire-Pulse socket explicitly.
        audio_env = dict(os.environ)
        audio_env.setdefault("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")

        audio_process = subprocess.Popen(
            [audio_exec_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            env=audio_env,
        )

    stdout_thread = threading.Thread(target=stdout_reader_thread, args=(audio_process,), daemon=True)
    stdout_thread.start()

    configure_audio_process()
    audio_process.wait()

    with audio_lock:
        audio_is_running = False
    print("The audio process has ended")


def api_v1_audio_start_post(body):
    global audio_state, audio_is_running, audio_thread

    if not connexion.request.is_json:
        return "Missing body!"

    new_cfg = connexion.request.get_json()

    with audio_lock:
        already = audio_is_running
        audio_state = new_cfg

    # Reconfigure if already running, else start.
    if already:
        configure_audio_process()
        return audio_state

    audio_thread = threading.Thread(target=run_audio_process, daemon=True)
    audio_thread.start()
    return audio_state


def api_v1_audio_stop_post():
    global audio_is_running, audio_thread, audio_process

    with audio_lock:
        if not audio_is_running or audio_process is None:
            return "Audio already stopped!"
        try:
            if audio_process.stdin:
                audio_process.stdin.write(json.dumps({"cmd": "stop"}) + "\n")
                audio_process.stdin.flush()
        except Exception as e:
            print(f"Failed to send audio stop command: {e}")
        audio_process.terminate()

    if audio_thread:
        audio_thread.join(timeout=2.0)
    return "Stopped"


def api_v1_audio_state_get():
    global audio_state, audio_is_running, audio_process

    with audio_lock:
        alive = (audio_process is not None and audio_process.poll() is None)
        state = dict(audio_state) if audio_state else {}
        state["is_running"] = bool(audio_is_running and alive)
        return state
