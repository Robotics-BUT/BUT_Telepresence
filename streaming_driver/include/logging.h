//
// Created by standa on 24.1.24.
//
#pragma once
#include <fstream>
#include <map>
#include <vector>
#include <exception>
#include <gst/rtp/gstrtpbuffer.h>

inline std::map<std::string, std::vector<long> > timestampsStreaming;
inline std::map<std::string, std::vector<long> > timestampsStreamingFiltered;
static inline uint16_t cameraLeftFrameId = 0, cameraRightFrameId = 0;
inline bool cameraLeftFrameIdIncremented = false, cameraRightFrameIdIncremented = false;

inline uint64_t lastCameraFrameTimeLeft = 0, lastCameraFrameTimeRight = 0;

inline bool finishing = false;

inline uint64_t GetCurrentUs() {
    using namespace std::chrono;

    struct timespec res{};
    clock_gettime(CLOCK_REALTIME, &res);
    return static_cast<uint64_t>(res.tv_sec) * 1'000'000 + res.tv_nsec / 1000;
}

inline uint16_t GetFrameId(const std::string &pipelineName) {
    return pipelineName == "pipeline_left" ? cameraLeftFrameId : cameraRightFrameId;
}

inline uint16_t IncrementFrameId(const std::string &pipelineName) {
    if (pipelineName == "pipeline_left") {
        cameraLeftFrameIdIncremented = true;
        return cameraLeftFrameId++;
    } else {
        cameraRightFrameIdIncremented = true;
        return cameraRightFrameId++;
    }
}

inline bool IsFrameIncremented(const std::string &pipelineName) {
    return pipelineName == "pipeline_left" ? cameraLeftFrameIdIncremented : cameraRightFrameIdIncremented;
}

inline void FrameSent(const std::string &pipelineName) {
    pipelineName == "pipeline_left" ? cameraLeftFrameIdIncremented = false : cameraRightFrameIdIncremented = false;
}

inline void OnIdentityHandoffCameraStreaming(const GstElement *identity, GstBuffer *buffer, gpointer data) {
    if (finishing) { return; }
    const auto timeMicro = GetCurrentUs();

    const std::string pipelineName = identity->object.parent->name;

    // Calculate camera frame duration (inverse of framerate)
    static uint64_t cameraFrameDurationLeft = 0, cameraFrameDurationRight = 0;
    if (std::string(identity->object.name) == "camsrc_ident") {
        if (pipelineName == "pipeline_left") {
            if (lastCameraFrameTimeLeft != 0) {
                cameraFrameDurationLeft = timeMicro - lastCameraFrameTimeLeft;
            }
            lastCameraFrameTimeLeft = timeMicro;
        } else if (pipelineName == "pipeline_right") {
            if (lastCameraFrameTimeRight != 0) {
                cameraFrameDurationRight = timeMicro - lastCameraFrameTimeRight;
            }
            lastCameraFrameTimeRight = timeMicro;
        }
    }

    if (std::string(identity->object.name) == "camsrc_ident" && !timestampsStreaming[pipelineName].empty()) {
        // Frame successfully sent, new one just got into the pipeline
        timestampsStreaming[pipelineName].clear();
        FrameSent(pipelineName);
    }

    timestampsStreaming[pipelineName].emplace_back(timeMicro);

    // Add metadata to the RTP header on the first call of rtpjpegpay
    if (std::string(identity->object.name) == "rtppay_ident" && !IsFrameIncremented(pipelineName)) {
        timestampsStreamingFiltered[pipelineName].emplace_back(timestampsStreaming[pipelineName][0]);
        timestampsStreamingFiltered[pipelineName].emplace_back(timestampsStreaming[pipelineName][1]);
        timestampsStreamingFiltered[pipelineName].emplace_back(timestampsStreaming[pipelineName][2]);
        timestampsStreamingFiltered[pipelineName].emplace_back(timestampsStreaming[pipelineName][3]);

        const unsigned long d = timestampsStreamingFiltered[pipelineName].size();

        uint64_t nvvidconv = timestampsStreamingFiltered[pipelineName][d - 3] - timestampsStreamingFiltered[pipelineName][d - 4];
        uint64_t jpegenc = timestampsStreamingFiltered[pipelineName][d - 2] - timestampsStreamingFiltered[pipelineName][d - 3];
        uint64_t rtpjpegpay = timestampsStreamingFiltered[pipelineName][d - 1] - timestampsStreamingFiltered[pipelineName][d - 2];

        GstRTPBuffer rtp_buf = GST_RTP_BUFFER_INIT;
        if (gst_rtp_buffer_map(buffer, GST_MAP_READWRITE, &rtp_buf)) {
            // Add FrameId
            uint64_t frameId = IncrementFrameId(pipelineName);
            uint64_t rtpjpegpayTimestamp = timestampsStreamingFiltered[pipelineName][d - 1];
            uint64_t cameraFrameDuration = (pipelineName == "pipeline_left") ? cameraFrameDurationLeft : cameraFrameDurationRight;
            if (
                !gst_rtp_buffer_add_extension_onebyte_header(&rtp_buf, 1, &frameId, sizeof(frameId)) ||
                !gst_rtp_buffer_add_extension_onebyte_header(&rtp_buf, 1, &cameraFrameDuration, sizeof(cameraFrameDuration)) ||
                !gst_rtp_buffer_add_extension_onebyte_header(&rtp_buf, 1, &nvvidconv, sizeof(nvvidconv)) ||
                !gst_rtp_buffer_add_extension_onebyte_header(&rtp_buf, 1, &jpegenc, sizeof(jpegenc)) ||
                !gst_rtp_buffer_add_extension_onebyte_header(&rtp_buf, 1, &rtpjpegpay, sizeof(rtpjpegpay)) ||
                !gst_rtp_buffer_add_extension_onebyte_header(&rtp_buf, 1, &rtpjpegpayTimestamp, sizeof(rtpjpegpayTimestamp))
            ) {
                std::cerr << "Couldn't add the RTP header with metadata! \n";
            }

            gst_rtp_buffer_unmap(&rtp_buf);
        }
    }
}
