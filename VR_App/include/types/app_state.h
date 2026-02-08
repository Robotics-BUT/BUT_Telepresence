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
// GUI Control State
// =============================================================================

/**
 * VR GUI navigation state.
 * Since VR has no mouse cursor, the GUI uses a focus-based navigation model.
 * The left thumbstick moves focus between settings, and face buttons change values.
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
