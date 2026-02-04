# BUT Telepresence

A low-latency standalone VR telepresence system for remote robot control. This system enables researchers to control a mobile robot through a Meta Quest VR headset with live stereo camera streaming and head tracking.

**Paper:** [TODO: Add IEEE Access link]

## Architecture Overview

```
┌─────────────────┐         UDP          ┌─────────────────────────────────────┐
│   VR Headset    │─────────────────────►│           Robot Platform            │
│  (Meta Quest)   │   Head pose/Control  │                                     │
│                 │                      │  ┌───────────────────────────────┐  │
│  ┌───────────┐  │Stereo Video (RTP/UDP)│  │             Robot             │  │
│  │  VR_App   │  │◄─────────────────────│  │           Controller          │  │
│  └───────────┘  │                      │  └───────────────────────────────┘  │
└─────────────────┘                      │         │                │          │
                                         │         ▼                ▼          │
                                         │  ┌─────────────┐  ┌──────────────┐  │
                                         │  │  Robot      │  │  Camera      │  │
                                         │  │  movement   │  │  Pan-Tilt    │  │
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
├── server/              # REST API for video stream control (Python/Flask)
├── services/            # systemd unit files
└── scripts/             # Telemetry visualization utilities
```

---

# VR Headset Side

## Prerequisites

To setup your environment to build & side-load the VR app you'll need the following:

- Android Studio with NDK 21.4.7075529
- [GStreamer Android SDK](https://gstreamer.freedesktop.org/download/#android) (arm64 1.18.x - 1.22.x)
- [Oculus OpenXR Mobile SDK](https://developer.oculus.com/downloads/package/oculus-openxr-mobile-sdk/) (tested with version 49.0)
- Boost 1.72.0 built for Android NDK 21 (available in this repository)
- Meta Quest 2/Pro/3 or different compatible headset with a developer mode enabled and connected via USB

## Setup

> **Note:** In the GStreamer Android SDK, rename these folders: `arm64` → `arm64-v8a` and `armv7` → `armeabi-v7a`

1. Clone with submodules:
   ```bash
   git clone --recursive https://github.com/Robotics-BUT/BUT_Telepresence
   ```

2. Create `VR_App/local.properties`:
   ```properties
   sdk.dir=/path/to/Android/Sdk
   gstreamer_sdk.dir=/path/to/gstreamer-1.0-android-universal
   ovr_openxr_mobile_sdk.dir=/path/to/ovr_openxr_mobile_sdk
   boost.dir=/path/to/ndk_21_boost_1.72.0
   ```

3. Build and deploy using Android Studio GUI

## Configuration

Network addresses are configured in `VR_App/src/common.h`:

```cpp
#define IP_CONFIG_JETSON_ADDR "10.0.31.42"      // Robot platform IP (can be configured from GUI once the app starts)
#define IP_CONFIG_HEADSET_ADDR "10.0.31.220"    // Headset IP (This is also auto-detected once the app starts)
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

# Or just use the in-built logcat interface in Android Studio
```

**Performance profiling:**

The app reports telemetry to InfluxDB (when enabled in *robot_controller*) including FPS, pipeline latency at each stage, and NTP sync status. See `scripts/visualize_telemetry.py` for analysis.

---

# Robot Side

The robot platform runs two services: the robot controller (command relay), and optionally the REST API server that runs the streaming driver internally (camera pipeline).

## Robot Controller

Relays head pose commands to the pan-tilt servo driver and movement commands to the robot.

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
    translator: tg_drives  # Head movement translator used for the reference measurements, you will need to create your own
  robot:
    ip: "10.0.31.11"       # Your robot IP
    port: 5555
    translator: spot       # Spot has been used for the reference measurements, you will have to provide your own translator

# Configuration specific to the reference servo translator
tg_drives:
  elevation_min: -2000000000
  elevation_max: 200000000
  azimuth_min: -600000000
  azimuth_max: 1100000000
  filter_alpha: 0.15       # Low-pass filter (0-1, higher = smoother)

# This enables the built-in telemetry logging and Grafana display.
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
Or set up permanently through the provided service in *services/*

## Streaming Driver

GStreamer-based stereo camera streaming pipeline. Make sure you have GStreamer installed with the HW accelerated codecs included.

Once you're certain you have all necessary elements (or you've made the needed changes in `pipelines.h`), build the project:

### Build

```bash
cd streaming_driver
mkdir build && cd build
cmake ..
make
```

## REST API Server

Provides HTTP endpoints for stream control and runs the streaming driver internally.

### Running

```bash
cd server
python -m venv venv
source venv/bin/activate
pip install -r requirements.txt
python -m swagger_server
# Swagger UI: http://localhost:8080/ui/
```

Or set up permanently through the provided service in *services/*

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

MIT License
