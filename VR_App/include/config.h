/**
 * config.h - Network configuration defaults
 *
 * Default IP addresses and ports for all telepresence system components.
 * IP addresses can be overridden at runtime via the in-VR settings GUI.
 */
#pragma once

#include <cstdint>

namespace Config {

/* Default IP addresses (overridable via GUI) */
constexpr uint8_t DEFAULT_JETSON_IP[4] = {10, 0, 31, 42};     /* Jetson camera/control server */
constexpr uint8_t DEFAULT_HEADSET_IP[4] = {10, 0, 31, 220};   /* VR headset (auto-detected) */

/* Network ports */
constexpr int REST_API_PORT = 32281;       /* Jetson REST API for stream control */
constexpr int SERVO_PORT = 32115;          /* UDP port for head pose / robot control */
constexpr int LEFT_CAMERA_PORT = 8554;     /* RTP video stream (left eye) */
constexpr int RIGHT_CAMERA_PORT = 8556;    /* RTP video stream (right eye) */
constexpr int ROS_GATEWAY_PORT = 8502;     /* UDP port for ROS gateway messages */

}  // namespace Config
