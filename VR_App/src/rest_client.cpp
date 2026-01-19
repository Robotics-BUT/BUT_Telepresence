//
// Created by standa on 27.08.24.
//
#include <nlohmann/json.hpp>
#include "rest_client.h"
#include "log.h"

using json = nlohmann::json;

RestClient::RestClient(StreamingConfig &config) : config_(config) {

    httpClient_ = std::make_unique<httplib::Client>(IpToString(config.jetson_ip).c_str(),
                                                    IP_CONFIG_REST_API_PORT);
    httpClient_->set_connection_timeout(2, 0);
}

int RestClient::StartStream() {
    std::string codec = "JPEG";
    switch (config_.codec) {
        case Codec::JPEG:
            codec = "JPEG";
            break;
        case Codec::VP8:
            codec = "VP8";
            break;
        case Codec::VP9:
            codec = "VP9";
            break;
        case Codec::H264:
            codec = "H264";
            break;
        case Codec::H265:
            codec = "H265";
            break;
        default:
            break;
    }
    std::string req = json{{"bitrate",          config_.bitrate},
                           {"codec",            codec},
                           {"encoding_quality", config_.encodingQuality},
                           {"fps",              config_.fps},
                           {"ip_address",       IpToString(config_.headset_ip)},
                           {"port_left",        config_.portLeft},
                           {"port_right",       config_.portRight},
                           {"resolution",       {{"height", config_.resolution.getHeight()}, {"width", config_.resolution.getWidth()}}},
                           {"video_mode",       config_.videoMode == VideoMode::STEREO ? "stereo": "mono"}}.dump();

    auto res = httpClient_->Post("/api/v1/stream/start", req, "application/json");
    if (!res) {
        LOG_ERROR("RestClient: Failed to send start stream request - connection error");
        return -1;
    }
    if (res->status != 200) {
        LOG_ERROR("RestClient: Start stream request failed with status %d: %s", res->status, res->body.c_str());
        return -1;
    }
    LOG_INFO("RestClient: Stream started successfully");
    return 0;
}

int RestClient::StopStream() {
    auto res = httpClient_->Post("/api/v1/stream/stop");
    if (!res) {
        LOG_ERROR("RestClient: Failed to send stop stream request - connection error");
        return -1;
    }
    if (res->status != 200) {
        LOG_ERROR("RestClient: Stop stream request failed with status %d: %s", res->status, res->body.c_str());
        return -1;
    }
    LOG_INFO("RestClient: Stream stopped successfully");
    return 0;
}

StreamingConfig RestClient::GetStreamingConfig() {
    return config_;
//    auto conf = StreamingConfig();
//    if (auto result = httpClient_->Get("/api/v1/stream/state")) {
//        std::string body = result->body;
//        auto parsedBody = json::parse(body);
//
//        parsedBody["ip_address"].get_to(conf.ip);
//        parsedBody["port_left"].get_to(conf.portLeft);
//        parsedBody["port_right"].get_to(conf.portRight);
//        conf.codec = Codec::JPEG;
//        parsedBody["encoding_quality"].get_to(conf.encodingQuality);
//        conf.bitrate = 400;
//        parsedBody["resolution"]["width"].get_to(conf.horizontalResolution);
//        parsedBody["resolution"]["height"].get_to(conf.verticalResolution);
//        conf.videoMode =
//                parsedBody["video_mode"].template get<std::string>() == "stereo" ? VideoMode::STEREO
//                                                                                 : VideoMode::MONO;
//        parsedBody["fps"].get_to(conf.fps);
//    }
//
//    return conf;
}

int RestClient::UpdateStreamingConfig(const StreamingConfig &config) {
    std::string req = json{{"bitrate",          config.bitrate},
                           {"codec",            CodecToString(config.codec)},
                           {"encoding_quality", config.encodingQuality},
                           {"fps",              config.fps},
                           {"ip_address",       IpToString(config_.headset_ip)},
                           {"port_left",        config.portLeft},
                           {"port_right",       config.portRight},
                           {"resolution",       {{"height", config.resolution.getHeight()}, {"width", config.resolution.getWidth()}}},
                           {"video_mode",       config.videoMode == VideoMode::STEREO ? "stereo"
                                                                                      : "mono"}}.dump();
    auto res = httpClient_->Put("/api/v1/stream/update", req, "application/json");
    if (!res) {
        LOG_ERROR("RestClient: Failed to send update config request - connection error");
        return -1;
    }
    if (res->status != 200) {
        LOG_ERROR("RestClient: Update config request failed with status %d: %s", res->status, res->body.c_str());
        return -1;
    }
    LOG_INFO("RestClient: Config updated successfully");
    config_ = config;
    return 0;
}