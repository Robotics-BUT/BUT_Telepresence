/**
 * app_state.h - Central application state container
 *
 * Defines the shared AppState struct that is passed (via shared_ptr) across all
 * modules in the application. Contains streaming configuration, system info,
 * GUI navigation state, connection monitoring, and runtime flags.
 */
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cmath>

#include <openxr/openxr.h>
#include "config.h"
#include "types/enums.h"
#include "types/camera_types.h"
#include "types/gui_setting.h"

// =============================================================================
// Streaming Configuration
// =============================================================================

/**
 * Video streaming configuration for the camera pipeline.
 * Controls codec, quality, resolution, and network settings for the
 * GStreamer-based video stream between the Jetson server and the VR headset.
 */
struct StreamingConfig {
    std::vector<uint8_t> headset_ip;
    std::vector<uint8_t> jetson_ip;

    int portLeft{Config::LEFT_CAMERA_PORT};
    int portRight{Config::RIGHT_CAMERA_PORT};

    Codec codec{Codec::JPEG};
    int encodingQuality{60};
    int bitrate{4000000};

    CameraResolution resolution{CameraResolution::fromLabel("FHD")};
    VideoMode videoMode{VideoMode::Stereo};
    int fps{60};

    StreamingConfig() {
        headset_ip = {Config::DEFAULT_HEADSET_IP[0], Config::DEFAULT_HEADSET_IP[1],
                      Config::DEFAULT_HEADSET_IP[2], Config::DEFAULT_HEADSET_IP[3]};
        jetson_ip = {Config::DEFAULT_JETSON_IP[0], Config::DEFAULT_JETSON_IP[1],
                     Config::DEFAULT_JETSON_IP[2], Config::DEFAULT_JETSON_IP[3]};
    }
};

/**
 * Classification of a streaming-config change.
 * - None:       nothing relevant changed; no action needed.
 * - LiveOnly:   only bitrate / encoding quality changed. The robot updates the
 *               encoder in place and the headset keeps decoding the same stream,
 *               so neither side tears anything down (fast, no glitch).
 * - Structural: resolution, codec, video mode, ip, or ports changed. Both
 *               ends must rebuild: the robot re-launches its GStreamer pipeline
 *               (fresh SPS/keyframe at the new format) and the headset rebuilds
 *               its decode pipeline + GL render targets.
 *
 * This MUST stay in lockstep with the robot driver's CanUpdateDynamically()
 * (video_driver/main.cpp). If one end rebuilds while the other live-updates,
 * the headset decoder jumps into a mid-GOP stream and the OES->2D blit FBO is
 * left sized for the old resolution -- the black-screen /
 * GL_INVALID_FRAMEBUFFER_OPERATION (0x506) failure mode.
 */
enum class StreamConfigChange { None, LiveOnly, Structural };

// A frame-rate change is live: the robot retimes in place (rate_capsfilter) and the headset keeps its
// decoder (the decoder caps keep the old frame rate, which the decoder does not need to match).
inline StreamConfigChange classifyStreamConfigChange(const StreamingConfig& a,
                                                     const StreamingConfig& b) {
    const bool structural =
        a.headset_ip != b.headset_ip ||
        a.jetson_ip  != b.jetson_ip ||
        a.portLeft   != b.portLeft ||
        a.portRight  != b.portRight ||
        a.codec      != b.codec ||
        a.resolution.getWidth()  != b.resolution.getWidth() ||
        a.resolution.getHeight() != b.resolution.getHeight() ||
        a.videoMode  != b.videoMode;
    if (structural) return StreamConfigChange::Structural;

    const bool live =
        a.bitrate != b.bitrate ||
        a.encodingQuality != b.encodingQuality ||
        a.fps != b.fps;
    return live ? StreamConfigChange::LiveOnly : StreamConfigChange::None;
}

// =============================================================================
// System Information
// =============================================================================

/**
 * Runtime system information collected at startup.
 * Reports the OpenXR runtime, GPU, and OpenGL version running on the headset.
 */
struct SystemInfo {
    std::string openXrRuntime;
    std::string openXrSystem;
    const unsigned char* openGlVersion{nullptr};
    const unsigned char* openGlVendor{nullptr};
    const unsigned char* openGlRenderer{nullptr};
};

// =============================================================================
// GUI Panel State (3D positioning + controller ray interaction)
// =============================================================================

/** Where the settings panel opens: upright in front of the head, or wide and tilted up under the video. */
enum class GuiPanelLayout { Front = 0, Below = 1 };

/** The panel's drawing surface for a layout. */
struct GuiPanelGeometry {
    int width;            // panel units: the ImGui layout and the controller ray's coordinates
    int height;
    float renderScale;    // swapchain pixels per panel unit (ImGui framebuffer scale)
};

/**
 * State of the floating GUI panel in VR space.
 * Stores the panel's world-space transform, controller ray hit-test results,
 * and grab-to-move state. Written by HandleControllers(), read by render.
 */
struct GuiPanelState {
    // Front: 1080 x 1620 px, a little above the ~1160 display pixels the panel spans at its default distance, so the
    // compositor samples it slightly minified; Below: 1440 x 660 px, likewise above the ~1000 it spans
    static constexpr GuiPanelGeometry FRONT{720, 1080, 1.5f};
    static constexpr GuiPanelGeometry BELOW{1440, 660, 1.0f};
    static constexpr GuiPanelGeometry geometryOf(GuiPanelLayout l) { return l == GuiPanelLayout::Below ? BELOW : FRONT; }

    GuiPanelLayout layout{GuiPanelLayout::Front};   // saved; the panel is placed for it whenever it opens
    XrVector3f position{0.0f, -0.1f, -0.7f};
    float yaw{0.0f};                       // rotation about +Y; at 0 (and pitch 0) the panel faces +Z
    float pitch{0.0f};                     // then about the panel's own X; negative tilts its top away
    float height{0.6f};

    GuiPanelGeometry geometry() const { return geometryOf(layout); }
    float getWorldWidth() const { return height * (float) geometry().width / (float) geometry().height; }
    XrQuaternionf orientation() const {    // yaw, then pitch
        const float sy = std::sin(yaw * 0.5f), cy = std::cos(yaw * 0.5f);
        const float sp = std::sin(pitch * 0.5f), cp = std::cos(pitch * 0.5f);
        return {cy * sp, sy * cp, -sy * sp, cy * cp};
    }

    // Controller ray (written by HandleControllers, read by render)
    bool rayActive{false};
    bool rayHitting{false};
    XrVector3f rayOrigin{};
    XrVector3f rayEnd{};
    float imguiMouseX{-1.0f};
    float imguiMouseY{-1.0f};
    bool triggerDown{false};
    float scrollDelta{0.0f};

    // Grab-to-move / resize (right grip while pointing at panel)
    bool grabbing{false};
    float grabHitX{0.0f};                  // where the grab started, in the panel's own frame
    float grabHitY{0.0f};
    float grabDistance{0.0f};              // along the ray: the grabbed point stays this far from the controller
    float grabInitialHeight{1.0f};
};

// =============================================================================
// GUI Control State
// =============================================================================

/**
 * VR GUI navigation state.
 * The left thumbstick moves focus between settings, and face buttons change values.
 * Controller ray pointing provides an alternative mouse-like interaction.
 */
struct GUIControl {
    bool focusMoveUp{false};
    bool focusMoveDown{false};
    bool focusMoveLeft{false};
    bool focusMoveRight{false};

    int tab{0};              /* open tab (GuiTab) */
    int focusedElement{0};   /* highlighted row: index into the settings vector, always a visible row of the open tab */
    int focusedSegment{0};   /* sub-segment index (e.g. IP address octets) */

    bool changesEnqueued{false};  /* true when a GUI input event needs processing */
    int cooldown{0};              /* frames to wait before accepting the next input */

    /* A row control pressed with the controller ray: run by HandleControllers on the next frame, outside drawing */
    int pendingRow{-1};
    GuiAction pendingAction{GuiAction::None};
    int pendingValue{0};
};

// =============================================================================
// Connection State
// =============================================================================

/**
 * Connection status for all external system components.
 * Tracked per-component so the GUI can display individual health indicators.
 */
struct ConnectionState {
    ConnectionStatus cameraServer{ConnectionStatus::Unknown};
    ConnectionStatus robotControl{ConnectionStatus::Unknown};
    ConnectionStatus ntpSync{ConnectionStatus::Unknown};
    std::string lastError;
};

// =============================================================================
// Application State
// =============================================================================

/**
 * What the status bar above the video shows, refreshed a few times a second (TelepresenceProgram::UpdateStatusBar).
 * The bar is a head-locked quad layer centred ELEVATION_DEG above the eyes, just over the top edge of the Full FOV
 * video (26.6 deg at 16:9), at the video's distance so both are seen at the same vergence.
 */
struct StatusBarState {
    static constexpr GuiPanelGeometry GEOMETRY{1600, 90, 1.25f};   // the pill is drawn centred, sized to its items
    static constexpr float DISTANCE_M = 2.0f, ELEVATION_DEG = 29.0f, HEIGHT_DEG = 3.0f;

    bool video{false};          /* a frame arrived within the last second */
    double fps{0.0};            /* frames received per second (left stream) */
    double latencyMs{0.0};      /* the per-stage latency sum (the panel header's figure) */
    double lossPerS{0.0};       /* RTP packets lost per second, both streams */
};

/**
 * Main application state container.
 * Shared across the application via std::shared_ptr for configuration and
 * runtime state. All modules (rendering, networking, GUI) read from and
 * write to this struct.
 */
struct AppState {
    /* Camera streaming */
    CamPair cameraStreamingStates{};
    StreamingConfig streamingConfig{};

    /* Display settings */
    AspectRatioMode aspectRatioMode{AspectRatioMode::FullFOV};
    /* Stereo convergence / horizontal image translation (HIT): per-eye horizontal
     * shift of the camera image plane for stereo comfort. Headset-render only, NOT
     * sent to the robot. World metres at the image plane; 0 = no shift (default). */
    float stereoConvergence{0.0f};

    /* Audio passthrough (optional, headset-only control; all default OFF so the
     * feature is purely additive and never engages unless the operator turns it on). */
    bool audioRobotEnable{false};   /* play the robot's mic audio on the headset speakers (RX) */
    bool audioMicEnable{false};     /* capture the headset mic and stream it to the robot (TX) */
    bool micPushToTalk{false};      /* true = push-to-talk (transmit while button held); false = open mic */
    bool micMuted{false};           /* open-mic mute toggle */
    int audioVolume{80};            /* 0-100 playback volume for the robot audio */

    /* Performance metrics */
    float appFrameRate{0.0f};       /* measured render FPS */
    long long appFrameTime{0};      /* last frame duration in microseconds */

    /* System info */
    SystemInfo systemInfo{};

    /* GUI state */
    GUIControl guiControl{};
    GuiPanelState guiPanel{};
    StatusBarState statusBar{};

    /* Head tracking settings - sent to the robot servo controller */
    uint32_t headMovementMaxSpeed{3000000};       /* camera-head speed, TGZ profile-generator inc/s (1e6 = 0.95 motor rev/s) */
    uint32_t headMovementPredictionMs{40};         /* lead beyond the predicted display time, ms (matches the drive at 3e6) */
    float headMovementSensitivity{1.0f};           /* camera-head angle per head angle (1.0 = 1:1; the relay maps angles 1:1) */

    /* Connection monitoring */
    ConnectionState connectionState{};
    std::string cameraServerStatus{"Unknown"};
    std::string robotControlStatus{"Unknown"};
    std::string ntpSyncStatus{"Unknown"};

    /* Runtime state */
    /* Proximity sensor overridden: the display and the app stay on with the headset off the head (not saved) */
    bool keepRunningOffHead{false};
    bool robotControlEnabled{true};
    bool headsetMounted{false};
};
