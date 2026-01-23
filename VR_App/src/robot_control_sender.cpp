
//
// RobotControlSender - Sends robot control commands over UDP
//
#include "robot_control_sender.h"
#include <unistd.h>

RobotControlSender::RobotControlSender(StreamingConfig &config, NtpTimer *ntpTimer)
        : ntpTimer_(ntpTimer), socket_(socket(AF_INET, SOCK_DGRAM, 0)) {

    if (socket_ < 0) {
        LOG_ERROR("RobotControlSender: socket creation failed - errno: %d", errno);
        isInitialized_ = false;
        return;
    }

    // Configure destination address
    memset(&destAddr_, 0, sizeof(destAddr_));
    destAddr_.sin_family = AF_INET;
    destAddr_.sin_addr.s_addr = inet_addr(IpToString(config.jetson_ip).c_str());
    destAddr_.sin_port = htons(IP_CONFIG_SERVO_PORT);

    isInitialized_ = true;
    LOG_INFO("RobotControlSender: Initialized successfully, sending to %s:%d",
             IpToString(config.jetson_ip).c_str(), IP_CONFIG_SERVO_PORT);
}

RobotControlSender::~RobotControlSender() {
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
}

void RobotControlSender::sendHeadPose(XrQuaternionf quatPose, float speed,
                                      BS::thread_pool<BS::tp::none> &threadPool) {
    if (!isInitialized_) {
        return;
    }

    threadPool.detach_task([this, quatPose, speed]() {
        // Convert quaternion to azimuth/elevation
        auto azElev = quaternionToAzimuthElevation(quatPose);

        // Get current timestamp
        uint64_t timestamp = ntpTimer_->GetCurrentTimeUs();

        // Send the packet
        sendHeadPosePacket(azElev.azimuth, azElev.elevation, speed, timestamp);
    });
}

void RobotControlSender::sendRobotControl(float linearX, float linearY, float angular,
                                          BS::thread_pool<BS::tp::none> &threadPool) {
    if (!isInitialized_) {
        return;
    }

    threadPool.detach_task([this, linearX, linearY, angular]() {
        // Get current timestamp
        uint64_t timestamp = ntpTimer_->GetCurrentTimeUs();

        // Send the packet
        sendRobotControlPacket(linearX, linearY, angular, timestamp);
        LOG_INFO(
                "RobotControlSender: Sending robot control message: linearX=%f, linearY=%f, angular=%f",
                linearX, linearY, angular);
    });
}

void RobotControlSender::sendDebugInfo(const CameraStatsSnapshot &stats,
                                       BS::thread_pool<BS::tp::none> &threadPool) {
    if (!isInitialized_) {
        return;
    }

    threadPool.detach_task([this, stats]() {
        // Get current timestamp
        uint64_t timestamp = ntpTimer_->GetCurrentTimeUs();

        // Send the packet
        sendDebugInfoPacket(stats, timestamp);
    });
}

void RobotControlSender::sendHeadPosePacket(float azimuth, float elevation, float speed,
                                            uint64_t timestamp) {
    std::vector<uint8_t> packet;
    packet.reserve(21);

    // Message type
    packet.push_back(MSG_HEAD_POSE);

    // Azimuth (float, 4 bytes, little-endian)
    serializeLittleEndian(packet, azimuth);

    // Elevation (float, 4 bytes, little-endian)
    serializeLittleEndian(packet, elevation);

    // Speed (float, 4 bytes, little-endian)
    serializeLittleEndian(packet, speed);

    // Timestamp (uint64, 8 bytes, little-endian)
    serializeLittleEndian(packet, timestamp);

    // Send UDP packet
    ssize_t sent = sendto(socket_, packet.data(), packet.size(), 0,
                          (sockaddr *) &destAddr_, sizeof(destAddr_));

    if (sent < 0) {
        LOG_ERROR("RobotControlSender: Failed to send head pose packet - errno: %d", errno);
    }
}

void RobotControlSender::sendRobotControlPacket(float linearX, float linearY, float angular,
                                                uint64_t timestamp) {
    std::vector<uint8_t> packet;
    packet.reserve(21);

    // Message type
    packet.push_back(MSG_ROBOT_CONTROL);

    // Linear velocity X (float, 4 bytes, little-endian)
    serializeLittleEndian(packet, linearX);

    // Linear velocity Y (float, 4 bytes, little-endian)
    serializeLittleEndian(packet, linearY);

    // Angular velocity (float, 4 bytes, little-endian)
    serializeLittleEndian(packet, angular);

    // Timestamp (uint64, 8 bytes, little-endian)
    serializeLittleEndian(packet, timestamp);

    // Send UDP packet
    ssize_t sent = sendto(socket_, packet.data(), packet.size(), 0,
                          (sockaddr *) &destAddr_, sizeof(destAddr_));

    if (sent < 0) {
        LOG_ERROR("RobotControlSender: Failed to send robot control packet - errno: %d", errno);
    }
}

void RobotControlSender::sendDebugInfoPacket(const CameraStatsSnapshot &stats, uint64_t timestamp) {
    std::vector<uint8_t> packet;
    packet.reserve(98);

    // Message type
    packet.push_back(MSG_DEBUG_INFO);

    serializeLittleEndian(packet, timestamp);
    serializeLittleEndian(packet, stats.frameId);
    serializeLittleEndian(packet, stats.fps);

    serializeLittleEndian(packet, stats.camera);
    serializeLittleEndian(packet, stats.vidConv);
    serializeLittleEndian(packet, stats.enc);
    serializeLittleEndian(packet, stats.rtpPay);
    serializeLittleEndian(packet, stats.udpStream);
    serializeLittleEndian(packet, stats.rtpDepay);
    serializeLittleEndian(packet, stats.dec);
    serializeLittleEndian(packet, stats.presentation);

    serializeLittleEndian(packet, ntpTimer_->GetSmoothedOffsetUs());
    packet.push_back(ntpTimer_->HasInitialOffset() ? 1 : 0);
    serializeLittleEndian(packet, ntpTimer_->GetTimeSinceLastSyncUs());

    ssize_t sent = sendto(socket_, packet.data(), packet.size(), 0,
                          (sockaddr *) &destAddr_, sizeof(destAddr_));

    if (sent < 0) {
        LOG_ERROR("RobotControlSender: Failed to send debug info packet - errno: %d", errno);
    }
}

RobotControlSender::AzimuthElevation
RobotControlSender::quaternionToAzimuthElevation(XrQuaternionf q) {
    // Convert quaternion to Euler angles (yaw/pitch) for OpenXR coordinate system
    // OpenXR uses right-handed: +X right, +Y up, +Z backward (forward is -Z)
    // Azimuth = yaw (rotation around Y axis)
    // Elevation = pitch (rotation around X axis)

    // Check for gimbal lock
    double sinp = 2.0 * (q.w * q.x - q.z * q.y);

    double azimuth, elevation;

    if (std::abs(sinp) >= 1.0) {
        // Gimbal lock: pitch is at ±90 degrees
        elevation = std::copysign(M_PI / 2.0, sinp);
        azimuth = std::atan2(-2.0 * q.x * q.z, 1.0 - 2.0 * (q.x * q.x + q.y * q.y));
    } else {
        // Normal case
        elevation = std::asin(sinp);
        azimuth = std::atan2(2.0 * (q.w * q.y + q.z * q.x),
                             1.0 - 2.0 * (q.x * q.x + q.y * q.y));
    }

    // Normalize angles to [-π, π] range
    // atan2 already returns values in [-π, π], but let's ensure consistency
    while (azimuth > M_PI) azimuth -= 2.0 * M_PI;
    while (azimuth < -M_PI) azimuth += 2.0 * M_PI;
    while (elevation > M_PI) elevation -= 2.0 * M_PI;
    while (elevation < -M_PI) elevation += 2.0 * M_PI;

    return AzimuthElevation{static_cast<float>(azimuth), static_cast<float>(elevation)};
}
