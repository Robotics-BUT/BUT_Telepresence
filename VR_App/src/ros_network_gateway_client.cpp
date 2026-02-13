/**
 * ros_network_gateway_client.cpp - ROS UDP listener implementation
 *
 * Runs a background thread listening for ROS messages on Config::ROS_GATEWAY_PORT.
 * Each UDP packet contains:
 *   [timestamp (double)][compressed (uint8)][topic\0][type\0][payload]
 * Proto messages (schema definitions) are registered automatically; data messages
 * are parsed against their registered schema.
 */
#include "ros_network_gateway_client.h"
#include "config.h"
#include <unistd.h>

constexpr int BUFFER_SIZE = 65535;  // Max UDP packet size
constexpr int RESPONSE_TIMEOUT_US = 50000;

RosNetworkGatewayClient::RosNetworkGatewayClient()
        : socket_(socket(AF_INET, SOCK_DGRAM, 0)), serverAddrLen_(sizeof(serverAddr_)) {

    if (socket_ < 0) {
        LOG_ERROR("RosNetworkGatewayClient: Socket creation failed (errno=%d: %s). "
                  "ROS topic data will not be available.",
                  errno, strerror(errno));
        isInitialized_ = false;
        return;
    }

    memset(&myAddr_, 0, sizeof(myAddr_));
    myAddr_.sin_family = AF_INET;
    myAddr_.sin_addr.s_addr = INADDR_ANY;
    myAddr_.sin_port = htons(Config::ROS_GATEWAY_PORT);

    if (bind(socket_, (sockaddr *) &myAddr_, sizeof(myAddr_)) < 0) {
        LOG_ERROR("RosNetworkGatewayClient: Bind to port %d failed (errno=%d: %s). "
                  "Another process may be using this port. ROS topic data unavailable.",
                  Config::ROS_GATEWAY_PORT, errno, strerror(errno));
        close(socket_);
        socket_ = -1;
        isInitialized_ = false;
        return;
    }

    isInitialized_ = true;
    running_ = true;
    LOG_INFO("RosNetworkGatewayClient: Listening for ROS messages on port %d",
             Config::ROS_GATEWAY_PORT);
    listenerThread_ = std::thread(&RosNetworkGatewayClient::listenForMessages, this);
}

RosNetworkGatewayClient::~RosNetworkGatewayClient() {
    running_ = false;
    if (socket_ >= 0) {
        shutdown(socket_, SHUT_RDWR);  // Unblock recvfrom
        close(socket_);
        socket_ = -1;
    }
    if (listenerThread_.joinable()) {
        listenerThread_.join();
    }
}

/** Background listener loop. Receives UDP packets and dispatches to schema/parse logic. */
void RosNetworkGatewayClient::listenForMessages() {
    if (!isInitialized_) {
        LOG_ERROR("RosNetworkGatewayClient: Listener started without initialization, exiting");
        return;
    }

    LOG_INFO("RosNetworkGatewayClient: Listener thread started");

    while (running_) {
        std::vector<uint8_t> buffer(BUFFER_SIZE);

        ssize_t received = recvfrom(socket_, buffer.data(), buffer.size(), 0,
                                    (sockaddr *) &serverAddr_, &serverAddrLen_);

        if (received <= 0) {
            if (!running_) break;  // Normal shutdown
            continue;
        }

        buffer.resize(received);

        double timestamp = 0.0;
        bool compressed = false;
        std::string topic, type, payload;

        if (!parseMessage(buffer, timestamp, compressed, topic, type, payload)) {
            LOG_ERROR("ROS Topic: Failed to parse ROS message header");
            continue;
        }

        if (compressed) {
            LOG_ERROR("ROS Topic: Received compressed message but Zstd decompression "
                      "is not supported in this client. Set compression_level:=0 on the gateway.");
            continue;
        }

        LOG_DEBUG("ROS Topic: %s (%s), timestamp: %.3f, payload: %s",
                  topic.c_str(), type.c_str(), timestamp, payload.c_str());

        try {
            if (schemaRegistry_.registerIfSchema(type, payload)) { continue; };
            if (!schemaRegistry_.hasSchema(type)) { continue; }
            auto parsed = schemaRegistry_.buildParsedMessage(type, topic, payload);
            LOG_INFO("ROS Topic: %s, json: %s", topic.c_str(), parsed.data().dump().c_str());
            if (parsed.topic() == "/loki_1/chassis/battery_voltage") {
                LOG_INFO("ROS Topic: %s, data: %f", topic.c_str(), parsed.get<float>("data"));
            } else if (parsed.topic() == "/loki_1/chassis/clock") {
                LOG_INFO("ROS Topic: %s, clock sec: %lu", topic.c_str(),
                         parsed.get<long>("clock.sec"));
            }
        } catch (const std::exception &e) {
            LOG_ERROR("ROS Topic: Failed to parse ROS message payload: %s", e.what());
        }

        buffer.resize(BUFFER_SIZE); // reset for next packet
    }
};

/**
 * Parse the binary message header:
 *   [timestamp(double)][compressed(uint8)][topic\0][type\0][payload]
 * Returns false if the buffer is too small or missing null terminators.
 */
bool RosNetworkGatewayClient::parseMessage(const std::vector<uint8_t> &buffer,
                                           double &timestamp,
                                           bool &compressed,
                                           std::string &topic,
                                           std::string &type,
                                           std::string &payload) {
    // Minimum: 8 (timestamp) + 1 (compressed) + 1 (topic) + 1 (\0) + 1 (type) + 1 (\0)
    if (buffer.size() < sizeof(double) + 1 + 4)
        return false;

    std::memcpy(&timestamp, buffer.data(), sizeof(double));
    size_t pos = sizeof(double);

    // Compression flag byte
    compressed = buffer[pos] != 0;
    pos += 1;

    // Find first null (topic)
    auto topic_end = std::find(buffer.begin() + pos, buffer.end(), '\0');
    if (topic_end == buffer.end()) return false;
    topic.assign(reinterpret_cast<const char *>(&buffer[pos]), topic_end - (buffer.begin() + pos));
    pos = (topic_end - buffer.begin()) + 1;

    // Find second null (type)
    auto type_end = std::find(buffer.begin() + pos, buffer.end(), '\0');
    if (type_end == buffer.end()) return false;
    type.assign(reinterpret_cast<const char *>(&buffer[pos]), type_end - (buffer.begin() + pos));
    pos = (type_end - buffer.begin()) + 1;

    // Rest is payload (JSON string or compressed bytes)
    payload.assign(reinterpret_cast<const char *>(&buffer[pos]), buffer.size() - pos);
    return true;
}