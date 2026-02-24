# BUT Telepresence

A low-latency standalone VR telepresence system for remote robot control. This system enables researchers to control a mobile robot through a Meta Quest VR headset with live camera streaming and head tracking. Supports stereo, mono, and panoramic (multi-camera) video modes.

**Paper:** [[Preprint]](https://www.researchsquare.com/article/rs-8855703/v1)

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
                                         │  │ Camera(s) → RTP stream      │    │
                                         │  │ Stereo / Mono / Panoramic   │    │
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

- Android Studio with NDK r25c (25.2.9519653)
- [GStreamer Android SDK](https://gstreamer.freedesktop.org/download/#android) (arm64, tested with 1.28.x built against NDK r25c)
- [Boost for Android](https://github.com/moritz-wundke/Boost-for-Android) — Boost 1.85.0 built for Android NDK r25c (see build instructions below)
- Meta Quest 2/Pro/3 or different compatible headset with a developer mode enabled and connected via USB

The OpenXR loader is fetched automatically from Maven Central as a Gradle dependency ([Khronos OpenXR Android Loader](https://central.sonatype.com/artifact/org.khronos.openxr/openxr_loader_for_android) 1.1.53). No manual SDK download needed.

## Setup

> **Note:** In the GStreamer Android SDK, rename these folders: `arm64` → `arm64-v8a` and `armv7` → `armeabi-v7a`

1. Clone with submodules:
   ```bash
   git clone --recursive https://github.com/Robotics-BUT/BUT_Telepresence
   ```

2. Build Boost for Android (if you don't have it already):
   ```bash
   git clone https://github.com/moritz-wundke/Boost-for-Android
   cd Boost-for-Android
   ./build-android.sh /path/to/Android/Sdk/ndk/25.2.9519653 --boost=1.85.0
   ```
   The build output will be in `build/out/`.

3. Create `VR_App/local.properties`:
   ```properties
   sdk.dir=/path/to/Android/Sdk
   gstreamer_sdk.dir=/path/to/gstreamer-1.0-android-universal
   boost_build.dir=/path/to/Boost-for-Android/build/out
   ```

4. Build and deploy using Android Studio GUI

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
    translator: asgard     # Asgard ecosystem (Spot, Husky, etc.), you will have to provide your own translator for other platforms

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

GStreamer-based camera streaming pipeline supporting three video modes:

| Mode | Cameras | Description |
|------|---------|-------------|
| **Stereo** | 2 (left/right) | Stereoscopic pair, one RTP stream per eye |
| **Mono** | 1 | Single camera, same frame to both eyes |
| **Panoramic** | 6 at 60° intervals | Single active camera selected by head yaw (see below) |

Make sure you have GStreamer installed with the HW accelerated codecs included.

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

# Panoramic Mode (Experimental)

> **Warning:** Panoramic mode is experimental. Expect rough edges.

Panoramic mode enables 360° coverage using 6 mono cameras mounted in a ring at 60° intervals. Only one camera streams at a time — the robot controller computes which camera faces the operator's gaze direction and switches the active source in real-time.

## How It Works

The streaming driver uses a **sliding window** of 3 cameras. The active camera and its two closest neighbors are kept open. When the operator looks toward a camera outside the window, the furthest non-active slot is dynamically swapped without rebuilding the pipeline.

```
VR Headset                          Jetson
  head pose ──UDP──► robot_controller
                       │ azimuth → camera_index (0-5)
                       │ 1-byte UDP to localhost:9100
                       ▼
                     streaming_driver (sliding window of 3)
                       │ If camera in window → instant input-selector switch
                       │ If not → pad probe swap on furthest non-active slot
                       ▼
                     slot 0 ─┐
                     slot 1 ─┤ input-selector ─► encoder ─► RTP ─► VR headset
                     slot 2 ─┘
```

- On startup, the pipeline opens cameras {5, 0, 1} — camera 0 (forward-facing) is active with its two neighbors preloaded.
- When a camera already in the window is requested, the `input-selector` switches pads instantly (zero-copy, sub-frame latency).
- When a camera outside the window is requested, a GStreamer blocking pad probe swaps the `nvarguscamerasrc` element on the furthest non-active slot: stop old source, remove from bin, create new source with the target sensor-id, add and link, sync state to PLAYING, then switch the selector and unblock.
- For H.264/H.265 codecs, an I-frame is forced on each camera switch to avoid decode artifacts.
- The VR app renders the single stream to both eyes (mono rendering).

## Configuration

Select `Panoramic` as the video mode in the VR app settings GUI or via the REST API (`"video_mode": "panoramic"`).

The camera selection port (9100) is a static constant shared between the streaming driver and the robot controller — it is not exposed through the REST API. The robot controller side is configured in `robot_controller/config.yaml`:

```yaml
network:
  camera:
    control_port: 9100
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
