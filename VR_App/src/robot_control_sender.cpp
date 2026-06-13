/**
 * robot_control_sender.cpp - UDP packet construction and sending
 *
 * Implements the binary protocol described in robot_control_sender.h.
 * All send methods serialize data in little-endian format and dispatch
 * the actual sendto() call to a thread pool.
 */
#include "robot_control_sender.h"
#include <unistd.h>

RobotControlSender::RobotControlSender(StreamingConfig &config, NtpTimer *ntpTimer)
        : ntpTimer_(ntpTimer), socket_(socket(AF_INET, SOCK_DGRAM, 0)),
          destIpString_(IpToString(config.jetson_ip)) {

    if (socket_ < 0) {
        LOG_ERROR("RobotControlSender: Socket creation failed (errno=%d: %s). "
                  "Head tracking and robot control will not work.",
                  errno, strerror(errno));
        isInitialized_ = false;
        return;
    }

    // Configure destination address
    memset(&destAddr_, 0, sizeof(destAddr_));
    destAddr_.sin_family = AF_INET;
    destAddr_.sin_addr.s_addr = inet_addr(destIpString_.c_str());
    destAddr_.sin_port = htons(Config::SERVO_PORT);

    isInitialized_ = true;
    LOG_INFO("RobotControlSender: Initialized, sending to %s:%d",
             destIpString_.c_str(), Config::SERVO_PORT);
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

void RobotControlSender::sendDebugInfo(const CameraStatsSnapshot &left,
                                       const CameraStatsSnapshot &right,
                                       const StreamingConfig &config,
                                       BS::thread_pool<BS::tp::none> &threadPool) {
    if (!isInitialized_) {
        return;
    }

    threadPool.detach_task([this, left, right, config]() {
        // Get current timestamp
        uint64_t timestamp = ntpTimer_->GetCurrentTimeUs();

        // Send the packet
        sendDebugInfoPacket(left, right, config, timestamp);
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
        int failures = ++consecutiveFailures_;
        if (failures == 1) {
            LOG_ERROR("RobotControlSender: Head pose send failed (errno=%d: %s). "
                      "Robot may not be receiving head tracking data.",
                      errno, strerror(errno));
        } else if (failures == FAILURE_THRESHOLD) {
            LOG_ERROR("RobotControlSender: %d consecutive send failures to %s:%d. "
                      "Check network connection and robot controller status.",
                      failures, destIpString_.c_str(), Config::SERVO_PORT);
        }
    } else {
        if (consecutiveFailures_ > 0) {
            LOG_INFO("RobotControlSender: Connection recovered after %d failures",
                     consecutiveFailures_.load());
        }
        consecutiveFailures_ = 0;
        ++successfulSends_;
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
        int failures = ++consecutiveFailures_;
        if (failures == 1) {
            LOG_ERROR("RobotControlSender: Robot control send failed (errno=%d: %s). "
                      "Robot movement commands may not be received.",
                      errno, strerror(errno));
        }
    } else {
        consecutiveFailures_ = 0;
    }
}

void RobotControlSender::sendDebugInfoPacket(const CameraStatsSnapshot &left,
                                             const CameraStatsSnapshot &right,
                                             const StreamingConfig &config, uint64_t timestamp) {
    std::vector<uint8_t> packet;
    packet.reserve(166);

    // Message type
    packet.push_back(MSG_DEBUG_INFO);

    // Latency stages: left stream (per-eye-symmetric, representative).
    serializeLittleEndian(packet, timestamp);
    serializeLittleEndian(packet, left.frameId);
    serializeLittleEndian(packet, left.fps);

    serializeLittleEndian(packet, left.camera);
    serializeLittleEndian(packet, left.vidConv);
    serializeLittleEndian(packet, left.enc);
    serializeLittleEndian(packet, left.rtpPay);
    serializeLittleEndian(packet, left.udpStream);
    serializeLittleEndian(packet, left.jbHold);
    serializeLittleEndian(packet, left.rtpDepay);
    serializeLittleEndian(packet, left.dec);
    serializeLittleEndian(packet, left.appsink);
    serializeLittleEndian(packet, left.presentation);

    serializeLittleEndian(packet, ntpTimer_->GetSmoothedOffsetUs());
    packet.push_back(ntpTimer_->HasInitialOffset() ? 1 : 0);
    serializeLittleEndian(packet, ntpTimer_->GetTimeSinceLastSyncUs());

    // Streaming config (shared by both eyes).
    packet.push_back(static_cast<uint8_t>(config.codec));
    packet.push_back(static_cast<uint8_t>(config.videoMode));
    serializeLittleEndian(packet, static_cast<uint16_t>(config.resolution.getWidth()));
    serializeLittleEndian(packet, static_cast<uint16_t>(config.resolution.getHeight()));
    serializeLittleEndian(packet, static_cast<uint16_t>(config.fps));
    serializeLittleEndian(packet, static_cast<uint32_t>(config.bitrate));

    // Per-eye network health (right is default-zero in mono).
    serializeLittleEndian(packet, left.jbNumLost);
    serializeLittleEndian(packet, left.rtxCount);
    serializeLittleEndian(packet, left.jitterUs);
    serializeLittleEndian(packet, left.actualBitrateBps);
    serializeLittleEndian(packet, right.jbNumLost);
    serializeLittleEndian(packet, right.rtxCount);
    serializeLittleEndian(packet, right.jitterUs);
    serializeLittleEndian(packet, right.actualBitrateBps);

    ssize_t sent = sendto(socket_, packet.data(), packet.size(), 0,
                          (sockaddr *) &destAddr_, sizeof(destAddr_));

    if (sent < 0) {
        // Debug info failures are less critical, just increment counter
        consecutiveFailures_++;
    } else {
        consecutiveFailures_ = 0;
    }
}

/**
 * Convert an OpenXR quaternion to azimuth/elevation angles.
 *
 * OpenXR coordinate system: right-handed, +X right, +Y up, +Z backward.
 * Azimuth = yaw (rotation around Y axis), range [-pi, pi].
 * Elevation = pitch (rotation around X axis), range [-pi/2, pi/2].
 * Handles gimbal lock when pitch is at +/-90 degrees.
 */
RobotControlSender::AzimuthElevation
RobotControlSender::quaternionToAzimuthElevation(XrQuaternionf q) {

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
