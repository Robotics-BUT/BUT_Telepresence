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

#include <openxr/openxr.h>
#include "config.h"
#include "types/enums.h"
#include "types/camera_types.h"

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
 * - Structural: resolution, codec, video mode, fps, ip, or ports changed. Both
 *               ends must rebuild: the robot re-launches its GStreamer pipeline
 *               (fresh SPS/keyframe at the new format) and the headset rebuilds
 *               its decode pipeline + GL render targets.
 *
 * This MUST stay in lockstep with the robot driver's CanUpdateDynamically()
 * (streaming_driver/main.cpp). If one end rebuilds while the other live-updates,
 * the headset decoder jumps into a mid-GOP stream and the OES->2D blit FBO is
 * left sized for the old resolution -- the black-screen /
 * GL_INVALID_FRAMEBUFFER_OPERATION (0x506) failure mode.
 */
enum class StreamConfigChange { None, LiveOnly, Structural };

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
        a.videoMode  != b.videoMode ||
        a.fps        != b.fps;
    if (structural) return StreamConfigChange::Structural;

    const bool live =
        a.bitrate != b.bitrate ||
        a.encodingQuality != b.encodingQuality;
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

/**
 * State of the floating GUI panel in VR space.
 * Stores the panel's world-space transform, controller ray hit-test results,
 * and grab-to-move state. Written by HandleControllers(), read by render.
 */
struct GuiPanelState {
    static constexpr int PIXEL_WIDTH = 300;
    static constexpr int PIXEL_HEIGHT = 540;

    XrVector3f position{1.0f, -0.5f, 0.2f};
    float height{1.0f};

    float getWorldWidth() const { return height * (float)PIXEL_WIDTH / (float)PIXEL_HEIGHT; }

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
    float grabPlaneZ{0.0f};
    float grabHitX{0.0f};
    float grabHitY{0.0f};
    XrVector3f grabPanelPos{};
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

    int focusedElement{0};   /* index into the settings vector */
    int focusedSegment{0};   /* sub-segment index (e.g. IP address octets) */

    bool changesEnqueued{false};  /* true when a GUI input event needs processing */
    int cooldown{0};              /* frames to wait before accepting the next input */

    int pendingActivation{-1};   /* button index clicked via controller ray, deferred to HandleControllers */
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

    /* Performance metrics */
    float appFrameRate{0.0f};       /* measured render FPS */
    long long appFrameTime{0};      /* last frame duration in microseconds */

    /* System info */
    SystemInfo systemInfo{};

    /* GUI state */
    GUIControl guiControl{};
    GuiPanelState guiPanel{};

    /* Head tracking settings - sent to the robot servo controller */
    uint32_t headMovementMaxSpeed{990000};        /* servo speed limit (device units) */
    uint32_t headMovementPredictionMs{50};         /* prediction horizon in milliseconds */
    float headMovementSpeedMultiplier{1.5f};       /* angular velocity scaling factor */

    /* Connection monitoring */
    ConnectionState connectionState{};
    std::string cameraServerStatus{"Unknown"};
    std::string robotControlStatus{"Unknown"};
    std::string ntpSyncStatus{"Unknown"};

    /* Runtime state */
    bool robotControlEnabled{true};
    bool headsetMounted{false};
};
