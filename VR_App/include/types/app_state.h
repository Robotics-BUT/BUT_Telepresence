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
 * Video streaming configuration for camera pipeline
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
 * Runtime system information
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
 * VR GUI navigation state
 */
struct GUIControl {
    bool focusMoveUp{false};
    bool focusMoveDown{false};
    bool focusMoveLeft{false};
    bool focusMoveRight{false};

    int focusedElement{0};
    int focusedSegment{0};

    bool changesEnqueued{false};
    int cooldown{0};
};

// =============================================================================
// Connection State
// =============================================================================

/**
 * Connection status for all system components
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
 * Main application state container
 * Shared across the application for configuration and runtime state
 */
struct AppState {
    // Camera streaming
    CamPair cameraStreamingStates{};
    StreamingConfig streamingConfig{};

    // Display settings
    AspectRatioMode aspectRatioMode{AspectRatioMode::FullFOV};

    // Performance metrics
    float appFrameRate{0.0f};
    long long appFrameTime{0};

    // System info
    SystemInfo systemInfo{};

    // GUI state
    GUIControl guiControl{};

    // Head tracking settings
    uint32_t headMovementMaxSpeed{990000};
    uint32_t headMovementPredictionMs{50};
    float headMovementSpeedMultiplier{1.5f};

    // Connection monitoring
    ConnectionState connectionState{};
    std::string cameraServerStatus{"Unknown"};
    std::string robotControlStatus{"Unknown"};
    std::string ntpSyncStatus{"Unknown"};

    // Runtime state
    bool robotControlEnabled{true};
    bool headsetMounted{false};
};
