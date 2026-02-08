#pragma once

#include <cstdint>

// =============================================================================
// Network Configuration
// Default IP addresses and ports for telepresence system components
// =============================================================================

namespace Config {

// Default IP addresses (can be overridden via GUI)
constexpr uint8_t DEFAULT_JETSON_IP[4] = {10, 0, 31, 42};
constexpr uint8_t DEFAULT_HEADSET_IP[4] = {10, 0, 31, 220};

// Network ports
constexpr int REST_API_PORT = 32281;
constexpr int SERVO_PORT = 32115;
constexpr int LEFT_CAMERA_PORT = 8554;
constexpr int RIGHT_CAMERA_PORT = 8556;
constexpr int ROS_GATEWAY_PORT = 8502;

}  // namespace Config
