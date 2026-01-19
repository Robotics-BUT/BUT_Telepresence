//
// Created by standa on 10/6/25.
//

#include "ros_network_gateway_client.h"

constexpr int BUFFER_SIZE = 65535;  // Max UDP packet size
constexpr int RESPONSE_TIMEOUT_US = 50000;

RosNetworkGatewayClient::RosNetworkGatewayClient()
        : socket_(socket(AF_INET, SOCK_DGRAM, 0)), serverAddrLen_(sizeof(serverAddr_)) {

    if (socket_ < 0) {
        LOG_ERROR("socket creation failed");
        return;
    }

    memset(&myAddr_, 0, sizeof(myAddr_));
    myAddr_.sin_family = AF_INET;
    myAddr_.sin_addr.s_addr = INADDR_ANY;
    myAddr_.sin_port = htons(IP_CONFIG_ROS_GATEWAY_PORT);

    if (bind(socket_, (sockaddr *) &myAddr_, sizeof(myAddr_)) < 0) {
        LOG_ERROR("bind socket failed");
    }

    listenerThread_ = std::thread(&RosNetworkGatewayClient::listenForMessages, this);
}

RosNetworkGatewayClient::~RosNetworkGatewayClient() {
    if (listenerThread_.joinable()) listenerThread_.join();
    if (socket_ >= 0) close(socket_);
}

void RosNetworkGatewayClient::listenForMessages() {
    while (true) {
        std::vector<uint8_t> buffer(BUFFER_SIZE);

        ssize_t received = recvfrom(socket_, buffer.data(), buffer.size(), 0,
                                    (sockaddr *) &serverAddr_, &serverAddrLen_);

        if (received <= 0)
            continue;

        buffer.resize(received);

        double timestamp = 0.0;
        std::string topic, type, payload;

        if (!parseMessage(buffer, timestamp, topic, type, payload)) {
            LOG_ERROR("ROS Topic: Failed to parse ROS message header");
            continue;
        }

//        double now = std::chrono::duration<double>(
//                std::chrono::system_clock::now().time_since_epoch())
//                .count();
//        double delay_ms = (now - timestamp) * 1000.0;
//
//        LOG_INFO("ROS Topic: %s (%s)\n  Timestamp: %.3f  Delay: %.1f ms\n  Payload: %s",
//                 topic.c_str(), type.c_str(), timestamp, delay_ms, payload.c_str());

        try {
            if (schemaRegistry_.registerIfSchema(type, payload)) { continue; };
            if (!schemaRegistry_.hasSchema(type)) { continue; }
            auto parsed = schemaRegistry_.buildParsedMessage(type, topic, payload);
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

bool RosNetworkGatewayClient::parseMessage(const std::vector<uint8_t> &buffer,
                                           double &timestamp,
                                           std::string &topic,
                                           std::string &type,
                                           std::string &payload) {
    if (buffer.size() < sizeof(double) + 3)
        return false;

    std::memcpy(&timestamp, buffer.data(), sizeof(double));
    size_t pos = sizeof(double);

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

    // Rest is payload (JSON string)
    payload.assign(reinterpret_cast<const char *>(&buffer[pos]), buffer.size() - pos);
    return true;
}