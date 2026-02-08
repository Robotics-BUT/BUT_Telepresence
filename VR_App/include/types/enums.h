#pragma once

#include <string>
#include <stdexcept>

// =============================================================================
// Video/Streaming Enums
// =============================================================================

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

enum class RobotType {
    Odin,
    Spot,
    Count
};

inline std::string RobotTypeToString(RobotType type) {
    switch (type) {
        case RobotType::Odin: return "ODIN";
        case RobotType::Spot: return "SPOT";
        default:
            throw std::invalid_argument("Invalid robot type");
    }
}

inline RobotType StringToRobotType(const std::string& type) {
    if (type == "ODIN") return RobotType::Odin;
    if (type == "SPOT") return RobotType::Spot;
    throw std::invalid_argument("Invalid robot type: " + type);
}

// =============================================================================
// Connection Status
// =============================================================================

enum class ConnectionStatus {
    Unknown,
    Connecting,
    Connected,
    Failed
};