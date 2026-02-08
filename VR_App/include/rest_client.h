/**
 * rest_client.h - HTTP client for the camera streaming server REST API
 *
 * Communicates with the Jetson-side streaming server to start/stop
 * the camera pipeline and update streaming parameters (codec, resolution,
 * bitrate, etc.). Uses cpp-httplib for HTTP requests.
 *
 * REST endpoints:
 *   POST /api/v1/stream/start  - start streaming with given config
 *   POST /api/v1/stream/stop   - stop streaming
 *   PUT  /api/v1/stream/update - update streaming parameters on the fly
 */
#pragma once

#include "pch.h"
#include "types/app_state.h"
#include "httplib.h"

class RestClient {
public:
    /** Create client connected to the Jetson IP from config on Config::REST_API_PORT. */
    explicit RestClient(StreamingConfig& config);

    /** POST /api/v1/stream/start - returns 0 on success, -1 on failure. */
    int StartStream();

    /** POST /api/v1/stream/stop - returns 0 on success, -1 on failure. */
    int StopStream();

    /** Return the current local copy of the streaming configuration. */
    StreamingConfig GetStreamingConfig();

    /** PUT /api/v1/stream/update - push new config to server. Returns 0 on success. */
    int UpdateStreamingConfig(const StreamingConfig& config);

private:

    StreamingConfig& config_;
    std::unique_ptr<httplib::Client> httpClient_;
};
