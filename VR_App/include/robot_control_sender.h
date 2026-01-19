//
// RobotControlSender - Sends robot control commands over UDP
//
#pragma once

#include "pch.h"
#include "log.h"
#include "common.h"
#include "BS_thread_pool.hpp"
#include "ntp_timer.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <atomic>
#include <cstring>

/**
 * RobotControlSender - Sends head pose and robot control data over UDP
 *
 * Protocol formats (little-endian):
 *
 * Message Type 0x01 - Head Pose (21 bytes):
 *   [0x01] [azimuth (float)] [elevation (float)] [speed (float)] [timestamp (uint64)]
 *
 * Message Type 0x02 - Robot Control (21 bytes):
 *   [0x02] [linear_x (float)] [linear_y (float)] [angular (float)] [timestamp (uint64)]
 *
 * Message Type 0x03 - Debug Info (78 bytes):
 *   [0x03] [timestamp (uint64)] [frame_id (uint64)] [fps (float)]
 *   [vidConv_us (uint64)] [enc_us (uint64)] [rtpPay_us (uint64)] [udpStream_us (uint64)]
 *   [rtpDepay_us (uint64)] [dec_us (uint64)] [presentation_us (uint64)]
 *   [ntp_offset_us (int64)] [ntp_synced (uint8)] [time_since_ntp_sync_us (uint64)]
 *
 * This simple protocol allows the receiving server to implement its own
 * robot-specific control logic without coupling the VR headset to specific hardware.
 */
class RobotControlSender {
public:
    explicit RobotControlSender(StreamingConfig &config, NtpTimer *ntpTimer);
    ~RobotControlSender();

    [[nodiscard]] bool isInitialized() const { return isInitialized_; }

    // Send head pose (quaternion is converted to azimuth/elevation internally)
    void sendHeadPose(XrQuaternionf quatPose, float speed, BS::thread_pool<BS::tp::none> &threadPool);

    // Send robot control commands (for mobile base control)
    void sendRobotControl(float linearX, float linearY, float angular, BS::thread_pool<BS::tp::none> &threadPool);

    // Send debug/validation information
    void sendDebugInfo(const CameraStatsSnapshot &stats, BS::thread_pool<BS::tp::none> &threadPool);

private:
    struct AzimuthElevation {
        float azimuth;    // radians, -π to π
        float elevation;  // radians, -π/2 to π/2
    };

    static AzimuthElevation quaternionToAzimuthElevation(XrQuaternionf quat);

    template<typename T>
    static void serializeLittleEndian(std::vector<uint8_t> &buffer, const T &value) {
        const uint8_t *bytes = reinterpret_cast<const uint8_t*>(&value);
        for (size_t i = 0; i < sizeof(T); ++i) {
            buffer.push_back(bytes[i]);
        }
    }

    void sendHeadPosePacket(float azimuth, float elevation, float speed, uint64_t timestamp);
    void sendRobotControlPacket(float linearX, float linearY, float angular, uint64_t timestamp);
    void sendDebugInfoPacket(const CameraStatsSnapshot &stats, uint64_t timestamp);

    int socket_{-1};
    struct sockaddr_in destAddr_{};
    std::atomic<bool> isInitialized_{false};
    NtpTimer *ntpTimer_;

    // Message types
    static constexpr uint8_t MSG_HEAD_POSE = 0x01;
    static constexpr uint8_t MSG_ROBOT_CONTROL = 0x02;
    static constexpr uint8_t MSG_DEBUG_INFO = 0x03;
};
