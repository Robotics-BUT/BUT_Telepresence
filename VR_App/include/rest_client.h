/**
 * rest_client.h - HTTP client for the camera streaming server REST API
 *
 * Communicates with the Jetson-side streaming server to start/stop
 * the camera pipeline and update streaming parameters (codec, resolution,
 * bitrate, etc.). Uses cpp-httplib for HTTP requests.
 *
 * REST endpoints:
 *   POST /api/v1/video/start  - start streaming with given config
 *   POST /api/v1/video/stop   - stop streaming
 *   PUT  /api/v1/video/update - update streaming parameters on the fly
 */
#pragma once

#include "pch.h"
#include "types/app_state.h"
#include "httplib.h"

class RestClient {
public:
    /** Create client connected to the Jetson IP from config on Config::REST_API_PORT. */
    explicit RestClient(StreamingConfig& config);

    /** POST /api/v1/video/start - returns 0 on success, -1 on failure. */
    int StartStream();

    /** POST /api/v1/video/stop - returns 0 on success, -1 on failure. */
    int StopStream();

    /** POST /api/v1/audio/start - start/reconfigure the optional robot audio bridge.
     *  robotMicToHeadset: stream the robot mic to the headset (RX leg);
     *  operatorToSpeaker: play the operator's mic on the robot speaker (TX leg).
     *  Returns 0 on success, -1 on failure. */
    int StartAudio(bool robotMicToHeadset, bool operatorToSpeaker, bool aecEnabled);

    /** POST /api/v1/audio/stop - returns 0 on success, -1 on failure. */
    int StopAudio();

    /** Return the current local copy of the streaming configuration. */
    StreamingConfig GetStreamingConfig();

    /** PUT /api/v1/video/update - push new config to server. Returns 0 on success. */
    int UpdateStreamingConfig(const StreamingConfig& config);

private:

    /** Create a fresh httplib::Client using the current config IP, with timeouts. */
    std::unique_ptr<httplib::Client> makeClient();

    StreamingConfig& config_;
};
