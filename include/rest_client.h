//
// Created by standa on 27.08.24.
//
#include "pch.h"
#include "common.h"
#include "httplib.h"

#pragma once

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