#pragma once

#include "pch.h"
#include "types/app_state.h"
#include "httplib.h"

class RestClient {
public:
    explicit RestClient(StreamingConfig& config);

    int StartStream();

    int StopStream();

    StreamingConfig GetStreamingConfig();

    int UpdateStreamingConfig(const StreamingConfig& config);

private:

    StreamingConfig& config_;
    std::unique_ptr<httplib::Client> httpClient_;
};
