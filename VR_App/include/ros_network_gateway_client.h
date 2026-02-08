#pragma once

#include "pch.h"
#include "log.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unordered_map>
#include <atomic>
#include <cstring>
#include "BS_thread_pool.hpp"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct MessageSchema {
    std::string type;
    json definition;
};

class ParsedMessage {
public:
    ParsedMessage(std::string type, std::string topic, json schema, json data)
            : type_(std::move(type)), topic_(std::move(topic)), schema_(std::move(schema)),
              data_(std::move(data)) {}

    template<typename T>
    T get(const std::string &field) const {
        // Split path by dots: e.g. "clock.sec"
        std::stringstream ss(field);
        std::string part;
        const json *cursor = &data_;

        // Walk the path
        while (std::getline(ss, part, '.')) {
            if (!cursor->contains(part)) {
                throw std::runtime_error(
                        "ROS: Field '" + part + "' not found in message path '" + field + "'");
            }

            // Move to the next nested level
            cursor = &cursor->at(part);

            // Automatically unwrap single-element arrays
            if (cursor->is_array() && !cursor->empty()) {
                cursor = &cursor->at(0);
            }
        }

        try {
            if (cursor->is_array()) {
                if (cursor->empty()) {
                    throw std::runtime_error("ROS: Field '" + field + "' is an empty array");
                }
                return cursor->at(0).get<T>();  // unwrap scalar array
            }
            return cursor->get<T>();
        }
        catch (const std::exception &e) {
            throw std::runtime_error("ROS: Type mismatch for field '" + field + "': " + e.what());
        }
    }

    void print() const {
        LOG_INFO("[ROS ParsedMessage] Type: %s", type_.c_str());
        LOG_INFO("[ROS ParsedMessage] Data: %s", data_.dump(2).c_str());
    }

    const json &data() const { return data_; }

    const json &schema() const { return schema_; }

    const std::string &type() const { return type_; }

    const std::string &topic() const { return topic_; }

private:
    std::string type_;
    std::string topic_;
    json schema_;
    json data_;
};

// <--------------------------------------------------------------------------------->

class SchemaRegistry {
public:
    bool registerIfSchema(const std::string &type, const std::string &payload) {
        bool isSchema = false;
        try {
            json j = json::parse(payload);

            // If it looks like a schema definition (proto)
            if (j.contains("fields") && j.contains("namespace") && j.contains("name")) {
                registry_[type] = {type, j};
                isSchema = true;
                LOG_INFO("[ROS SchemaRegistry] Registered schema for type %s", type.c_str());
            }
        } catch (const std::exception &e) {
            LOG_ERROR("[ROS SchemaRegistry] Failed to parse payload as JSON: %s", e.what());
        }

        return isSchema;
    }

    bool hasSchema(const std::string &type) const {
        return registry_.find(type) != registry_.end();
    }

    const MessageSchema *getSchema(const std::string &type) const {
        auto it = registry_.find(type);
        if (it != registry_.end())
            return &it->second;
        return nullptr;
    }

    ParsedMessage buildParsedMessage(
            const std::string &type,
            const std::string &topic,
            const std::string &payload) {
        if (!hasSchema(type)) {
            throw std::runtime_error("ROS: No schema found for type " + type + " during parsing");
        }

        auto schema = getSchema(type);
        json j = json::parse(payload);

        // Optional validation step
        for (auto &field: schema->definition["fields"]) {
            std::string name = field["name"];
            if (!j.contains(name)) {
                LOG_ERROR("[ROS Parse Warning] Missing field %s in payload of type %s",
                          name.c_str(), type.c_str());
            }
        }

        // Unwrap single element arrays
        for (auto &[key, value]: j.items()) {
            if (value.is_array() && value.size() == 1) {
                value = value.at(0);
            }
        }

        return ParsedMessage(type, topic, schema->definition, j);
    }

private:
    std::unordered_map<std::string, MessageSchema> registry_;
};

// <--------------------------------------------------------------------------------->

class RosNetworkGatewayClient {

public:
    explicit RosNetworkGatewayClient();
    ~RosNetworkGatewayClient();

    [[nodiscard]] bool isInitialized() const { return isInitialized_; }

private:

    void listenForMessages();

    std::atomic<bool> isInitialized_{false};
    std::atomic<bool> running_{true};

    static bool parseMessage(const std::vector<uint8_t> &buffer,
                             double &timestamp,
                             std::string &topic,
                             std::string &type,
                             std::string &payload);

    sockaddr_in myAddr_{}, serverAddr_{};
    socklen_t serverAddrLen_;
    int socket_ = -1;

    SchemaRegistry schemaRegistry_{};

    std::thread listenerThread_;
};

