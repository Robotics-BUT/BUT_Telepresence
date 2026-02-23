/**
 * enums.h - Application-wide enumeration types with string conversion
 *
 * Defines enums for video codec selection, stereo/mono mode, aspect ratio,
 * robot platform type, and connection status. Each enum includes inline
 * string conversion functions for display and logging.
 *
 * Enums that support cycling (Codec, VideoMode, AspectRatioMode, RobotType)
 * include a Count sentinel for modular arithmetic in the GUI settings.
 */
#pragma once

#include <string>
#include <stdexcept>

// =============================================================================
// Video/Streaming Enums
// =============================================================================

/** Video codec for the camera streaming pipeline. */
enum class Codec {
    JPEG,
    VP8,
    VP9,
    H264,
    H265,
    Count
};

inline std::string CodecToString(Codec codec) {
    switch (codec) {
        case Codec::JPEG:  return "JPEG";
        case Codec::VP8:   return "VP8";
        case Codec::VP9:   return "VP9";
        case Codec::H264:  return "H264";
        case Codec::H265:  return "H265";
        default:           return "Unknown";
    }
}

/** Stereo (two independent eye streams) or mono (single stream for both eyes). */
enum class VideoMode {
    Stereo,
    Mono,
    Count
};

inline std::string VideoModeToString(VideoMode mode) {
    switch (mode) {
        case VideoMode::Stereo: return "STEREO";
        case VideoMode::Mono:   return "MONO";
        default:                return "Unknown";
    }
}

/** How the camera image fills the VR field of view. */
enum class AspectRatioMode {
    FullScreen,
    FullFOV,
    Count
};

inline std::string AspectRatioModeToString(AspectRatioMode mode) {
    switch (mode) {
        case AspectRatioMode::FullScreen: return "FULLSCREEN";
        case AspectRatioMode::FullFOV:    return "FULLFOV";
        default:                          return "Unknown";
    }
}

// =============================================================================
// Robot Enums
// =============================================================================

/** Supported robot platforms. Determines control protocol details. */
enum class RobotType {
    Asgard,
    Count
};

inline std::string RobotTypeToString(RobotType type) {
    switch (type) {
        case RobotType::Asgard: return "ASGARD";
        default:
            throw std::invalid_argument("Invalid robot type");
    }
}

inline RobotType StringToRobotType(const std::string& type) {
    if (type == "ASGARD") return RobotType::Asgard;
    throw std::invalid_argument("Invalid robot type: " + type);
}

// =============================================================================
// Connection Status
// =============================================================================

/** Health status for external connections (camera server, robot control, NTP). */
enum class ConnectionStatus {
    Unknown,
    Connecting,
    Connected,
    Failed
};