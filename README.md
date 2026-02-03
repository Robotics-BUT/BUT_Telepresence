# BUT Telepresence

A low-latency VR telepresence system for remote robot control. This system enables an operator to control a robot (Boston Dynamics Spot) through a Meta Quest VR headset with live stereo camera streaming and head/hand tracking.

**Paper:** [TODO: Add IEEE Access link]

## Architecture Overview

```
┌─────────────────┐         UDP          ┌─────────────────────────────────────┐
│   VR Headset    │◄────────────────────►│           Robot Platform            │
│  (Meta Quest)   │   Head pose/Control  │                                     │
│                 │                      │  ┌─────────────┐  ┌──────────────┐  │
│  ┌───────────┐  │    RTP/UDP (stereo)  │  │   Robot     │  │   Servo      │  │
│  │  VR_App   │◄─┼──────────────────────┼──┤  Controller │  │   Driver     │  │
│  └───────────┘  │                      │  └─────────────┘  └──────────────┘  │
└─────────────────┘                      │         │                │          │
                                         │         ▼                ▼          │
                                         │  ┌─────────────┐  ┌──────────────┐  │
                                         │  │ Spot Robot  │  │ Pan-Tilt     │  │
                                         │  │ (movement)  │  │ (camera)     │  │
                                         │  └─────────────┘  └──────────────┘  │
                                         │                                     │
                                         │  ┌─────────────────────────────┐    │
                                         │  │ Streaming Driver (GStreamer)│    │
                                         │  │ Stereo camera → RTP stream  │    │
                                         │  └─────────────────────────────┘    │
                                         └─────────────────────────────────────┘
```

## Repository Structure

```
BUT_Telepresence/
├── VR_App/              # Android VR application (C++/OpenXR)
├── robot_controller/    # Head pose & robot control relay (Python)
├── streaming_driver/    # Camera streaming pipeline (C++/GStreamer)
├── server/              # REST API for stream control (Python/Flask)
├── services/            # systemd unit files
└── scripts/             # Telemetry visualization utilities
```

---

# VR Headset Side

## Prerequisites

- Android Studio with NDK 21.4.7075529
- [GStreamer Android SDK](https://gstreamer.freedesktop.org/download/) (arm64)
- [Oculus OpenXR Mobile SDK](https://developer.oculus.com/downloads/package/oculus-openxr-mobile-sdk/)
- Boost 1.72.0 built for Android NDK 21

## Setup

1. Clone with submodules:
   ```bash
   git clone --recursive <repo-url>
   ```

2. Create `VR_App/local.properties`:
   ```properties
   sdk.dir=/path/to/Android/Sdk
   gstreamer_sdk.dir=/path/to/gstreamer-1.0-android-universal
   ovr_openxr_mobile_sdk.dir=/path/to/ovr_openxr_mobile_sdk
   boost.dir=/path/to/ndk21_boost_1.72.0
   ```

3. Build:
   ```bash
   cd VR_App
   ./gradlew assembleDebug
   ```

4. Install on headset (developer mode required):
   ```bash
   adb install -r app/build/outputs/apk/openGLES/debug/app-openGLES-debug.apk
   ```

## Configuration

Network addresses are configured in `VR_App/src/common.h`:

```cpp
#define IP_CONFIG_JETSON_ADDR "10.0.31.42"      // Robot platform IP
#define IP_CONFIG_HEADSET_ADDR "10.0.31.220"    // Headset IP (for RTP sink)
#define REST_API_PORT 32281
#define SERVO_PORT 32115
#define LEFT_CAMERA_PORT 8554
#define RIGHT_CAMERA_PORT 8556
```

Rebuild after modifying.

## Debugging

**ADB logcat filtering:**
```bash
# GStreamer pipeline issues
adb logcat -s GStreamer

# OpenXR session/rendering
adb logcat -s OpenXR

# Application logic
adb logcat -s VR_App

# All app output
adb logcat | grep -E "(GStreamer|OpenXR|VR_App)"
```

**Common issues:**

| Symptom | Cause | Solution |
|---------|-------|----------|
| Black screen, no video | Stream not started or wrong IP | Check `IP_CONFIG_*` in common.h, verify streaming_driver is running |
| Laggy head tracking | Network congestion | Reduce video bitrate, check WiFi channel interference |
| App crashes on start | OpenXR runtime missing | Ensure headset firmware is up to date |
| "No XR runtime" error | Oculus services not running | Reboot headset, check developer mode |

**Performance profiling:**

The app reports telemetry to InfluxDB including FPS, pipeline latency at each stage, and NTP sync status. See `scripts/visualize_telemetry.py` for analysis.

---

# Robot Side

The robot platform runs three services: the robot controller (command relay), the streaming driver (camera pipeline), and optionally the REST API server.

## Robot Controller

Relays head pose commands to the pan-tilt servo driver and movement commands to the robot.

### Setup

```bash
cd robot_controller
cp config.yaml.example config.yaml  # if not present, use existing config.yaml
# Edit config.yaml with your network addresses
```

### Configuration

Edit `robot_controller/config.yaml`:

```yaml
network:
  ingest:
    host: "0.0.0.0"
    port: 32115
  servo:
    ip: "192.168.1.150"    # Pan-tilt servo driver
    port: 502
    translator: tg_drives
  robot:
    ip: "10.0.31.11"       # Spot robot
    port: 5555
    translator: spot

tg_drives:
  elevation_min: -2000000000
  elevation_max: 200000000
  azimuth_min: -600000000
  azimuth_max: 1100000000
  filter_alpha: 0.15       # Low-pass filter (0-1, lower = smoother)

telemetry:
  enabled: true
  influxdb_host: "http://localhost:8181"
  influxdb_database: "but_telepresence_telemetry"
```

### Running

```bash
cd robot_controller
./start.sh
```

Or manually:
```bash
python -m venv venv
source venv/bin/activate
pip install -r requirements.txt
python -m robot_controller
```

### Debugging

**Log levels** in `config.yaml`:
```yaml
logging:
  level: DEBUG  # DEBUG, INFO, WARNING, ERROR
  file: null    # null=stdout, or path like "/var/log/robot_controller.log"
```

**Common issues:**

| Symptom | Cause | Solution |
|---------|-------|----------|
| "Connection refused" to servo | Servo driver not reachable | Check IP/port, verify servo driver is powered on |
| Jerky servo movement | filter_alpha too high | Lower `filter_alpha` (e.g., 0.1) |
| Commands ignored | Out-of-order packets | Check network stability; controller drops stale packets automatically |
| No telemetry data | InfluxDB not running | See `robot_controller/TELEMETRY_SETUP.md` |

**Protocol debugging:**

Capture UDP traffic to inspect messages:
```bash
# Head pose packets (0x01 prefix, 17 bytes)
tcpdump -i any udp port 32115 -X

# Verify message format
python -c "
import struct
# Head pose: [0x01][azimuth:f32][elevation:f32][timestamp:u64]
data = bytes.fromhex('01 00 00 80 3f 00 00 00 40 ...')
msg_type, az, el, ts = struct.unpack('<Bffq', data[:17])
print(f'Type: {msg_type}, Az: {az}, El: {el}, TS: {ts}')
"
```

## Streaming Driver

GStreamer-based stereo camera streaming pipeline.

### Build

```bash
cd streaming_driver
mkdir build && cd build
cmake ..
make
```

### Running

Configure the pipeline parameters in the source or via command-line arguments. The driver streams RTP/UDP to the headset IP.

### Debugging

```bash
# Test pipeline manually
gst-launch-1.0 -v videotestsrc ! x264enc ! rtph264pay ! udpsink host=10.0.31.220 port=8554

# Check GStreamer plugin availability
gst-inspect-1.0 x264enc
gst-inspect-1.0 rtph264pay
```

## REST API Server (Optional)

Provides HTTP endpoints for stream control.

### Running with Docker

```bash
cd server
docker build -t swagger_server .
docker run -p 8080:8080 swagger_server
```

### Running locally

```bash
cd server
python -m venv venv
source venv/bin/activate
pip install -r requirements.txt
python -m swagger_server
# Swagger UI: http://localhost:8080/ui/
```

## systemd Services

Install services for automatic startup:

```bash
sudo cp services/*.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable camera-server robot-controller
sudo systemctl start camera-server robot-controller
```

Check status:
```bash
systemctl status robot-controller
journalctl -u robot-controller -f
```

---

# Telemetry & Monitoring

The system collects latency metrics at each pipeline stage. See `robot_controller/TELEMETRY_SETUP.md` for InfluxDB + Grafana setup.

Export and visualize data:
```bash
cd scripts
python export_telemetry.py --output data.csv
python visualize_telemetry.py data.csv
```

---

# License

MIT License - see individual component directories for details.
