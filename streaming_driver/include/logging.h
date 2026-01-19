//
// Created by standa on 24.1.24.
//
#pragma once
#include <fstream>
#include <map>
#include <vector>
#include <exception>
#include <gst/rtp/gstrtpbuffer.h>
#ifdef JETSON
#include <experimental/filesystem>
#else
#include <filesystem>
#endif

constexpr bool BENCHMARK = false;
constexpr unsigned int SAMPLES = 1000;

inline std::map<std::string, std::vector<long> > timestampsCamera;
inline std::map<std::string, std::vector<long> > timestampsStreaming;
inline std::map<std::string, std::vector<long> > timestampsStreamingFiltered;
static inline uint16_t cameraLeftFrameId = 0, cameraRightFrameId = 0;
inline bool cameraLeftFrameIdIncremented = false, cameraRightFrameIdIncremented = false;
inline std::map<std::string, std::vector<long> > timestampsReceiving;
inline std::map<std::string, std::vector<long> > timestampsReceivingFiltered;

inline uint16_t latestNvvidconv = 0, latestJpegenc = 0, latestRtpjpegpay = 0;
inline uint64_t latestRtpJpegpayTimestamp = 0;

inline bool finishing = false;

inline uint64_t GetCurrentUs() {
    using namespace std::chrono;

    //auto currentTime = high_resolution_clock::now();
    //auto duration = duration_cast<microseconds>(currentTime.time_since_epoch());
    //auto timeMicro = duration.count();
    //return timeMicro;

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

inline void SaveLogFilesStreaming() {
    std::ofstream cameraPipeline0File, cameraPipeline1File, streamingPipeline0File, streamingPipeline1File;
    cameraPipeline0File.open("cameraPipeline0Log.txt", std::ios::trunc);
    cameraPipeline1File.open("cameraPipeline1Log.txt", std::ios::trunc);
    streamingPipeline0File.open("streamingPipeline0Log.txt", std::ios::trunc);
    streamingPipeline1File.open("streamingPipeline1Log.txt", std::ios::trunc);

    std::cout << "Will be writing log containing " << timestampsStreaming["pipeline_left"].size() << " records\n";
    for (int i = 0; i < timestampsStreaming["pipeline_left"].size(); i = i + 3) {
        streamingPipeline0File <<
                timestampsStreamingFiltered["pipeline_left"][i] << "," <<
                timestampsStreamingFiltered["pipeline_left"][i + 1] << "," <<
                timestampsStreamingFiltered["pipeline_left"][i + 2] << "\n";
    }

    for (int i = 0; i < timestampsStreaming["pipeline_8556"].size(); i = i + 3) {
        streamingPipeline1File <<
                timestampsStreamingFiltered["pipeline_8556"][i] << "," <<
                timestampsStreamingFiltered["pipeline_8556"][i + 1] << "," <<
                timestampsStreamingFiltered["pipeline_8556"][i + 2] << "\n";
    }

    for (int i = 0; i < timestampsCamera["pipeline_left"].size(); i = i + 2) {
        cameraPipeline0File <<
                timestampsCamera["pipeline_left"][i] << "," <<
                timestampsCamera["pipeline_left"][i + 1] << "\n";
    }

    for (int i = 0; i < timestampsCamera["pipeline_right"].size(); i = i + 2) {
        cameraPipeline1File <<
                timestampsCamera["pipeline_right"][i] << "," <<
                timestampsCamera["pipeline_right"][i + 1] << "\n";
    }

    cameraPipeline0File.close();
    cameraPipeline1File.close();
    streamingPipeline0File.close();
    streamingPipeline1File.close();
    std::cout << "Log files written! \n";
    std::this_thread::sleep_for(std::chrono::seconds(1));
    throw std::exception();
}

inline void SaveLogFilesReceiving() {
    std::ofstream receivingPipeline0File, receivingPipeline1File;
    receivingPipeline0File.open("receivingPipeline0Log.txt", std::ios::trunc);
    receivingPipeline1File.open("receivingPipeline0Log.txt", std::ios::trunc);

    std::cout << "Will be writing log containing " << timestampsStreaming["pipeline_left"].size() << " records\n";
    for (int i = 0; i < timestampsStreaming["pipeline_left"].size(); i = i + 6) {
        receivingPipeline0File <<
                timestampsReceiving["pipeline_left"][i] << "," <<
                timestampsReceiving["pipeline_left"][i + 1] << "," <<
                timestampsReceiving["pipeline_left"][i + 2] << "," <<
                timestampsReceiving["pipeline_left"][i + 3] << "," <<
                timestampsReceiving["pipeline_left"][i + 5] << "," <<
                timestampsReceiving["pipeline_left"][i + 5] << "," <<
                timestampsReceiving["pipeline_left"][i + 6] << "\n";
    }

    for (int i = 0; i < timestampsStreaming["pipeline_right"].size(); i = i + 6) {
        receivingPipeline1File <<
                timestampsReceiving["pipeline_right"][i] << "," <<
                timestampsReceiving["pipeline_right"][i + 1] << "," <<
                timestampsReceiving["pipeline_right"][i + 2] << "," <<
                timestampsReceiving["pipeline_right"][i + 3] << "," <<
                timestampsReceiving["pipeline_right"][i + 5] << "," <<
                timestampsReceiving["pipeline_right"][i + 5] << "," <<
                timestampsReceiving["pipeline_right"][i + 6] << "\n";
    }

    receivingPipeline0File.close();
    receivingPipeline1File.close();
    std::cout << "Log files written! \n";
    std::this_thread::sleep_for(std::chrono::seconds(1));
    throw std::exception();
}

inline void OnIdentityHandoffCameraStreaming(const GstElement *identity, GstBuffer *buffer, gpointer data) {
    if (finishing) { return; }
    const auto timeMicro = GetCurrentUs();

    const std::string pipelineName = identity->object.parent->name;


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

        // std::cout << pipelineName << ": frame - " << GetFrameId(pipelineName) <<
        //         ", nvvidconv: " << nvvidconv <<
        //         ", jpegenc: " << jpegenc <<
        //         ", rtpjpegpay: " << rtpjpegpay <<
        //         "\n";

        GstRTPBuffer rtp_buf = GST_RTP_BUFFER_INIT;
        if (gst_rtp_buffer_map(buffer, GST_MAP_READWRITE, &rtp_buf)) {
            // Add FrameId
            uint64_t frameId = IncrementFrameId(pipelineName);
            uint64_t rtpjpegpayTimestamp = timestampsStreamingFiltered[pipelineName][d - 1];
            if (
                !gst_rtp_buffer_add_extension_twobytes_header(&rtp_buf, 1, 1, &frameId, sizeof(frameId)) ||
                !gst_rtp_buffer_add_extension_twobytes_header(&rtp_buf, 1, 1, &nvvidconv, sizeof(nvvidconv)) ||
                !gst_rtp_buffer_add_extension_twobytes_header(&rtp_buf, 1, 1, &jpegenc, sizeof(jpegenc)) ||
                !gst_rtp_buffer_add_extension_twobytes_header(&rtp_buf, 1, 1, &rtpjpegpay, sizeof(rtpjpegpay)) ||
                !gst_rtp_buffer_add_extension_twobytes_header(&rtp_buf, 1, 1, &rtpjpegpayTimestamp, sizeof(rtpjpegpayTimestamp))
            ) {
                std::cerr << "Couldn't add the RTP header with metadata! \n";
            }

            gst_rtp_buffer_unmap(&rtp_buf);
        }
    }

    if (timestampsStreamingFiltered[pipelineName].size() > SAMPLES && BENCHMARK) {
        finishing = true;
        SaveLogFilesStreaming();
    }
}

inline void OnIdentityHandoffReceiving(const GstElement *identity, GstBuffer *buffer, gpointer data) {
    if (finishing) { return; }
    using namespace std::chrono;

    const auto tp = std::chrono::time_point_cast<microseconds>(steady_clock::now());
    const auto tmp = std::chrono::duration_cast<microseconds>(tp.time_since_epoch());
    const auto timeMicro = tmp.count();

    const std::string pipelineName = identity->object.parent->name;
    timestampsReceiving[pipelineName].emplace_back(timeMicro);

    if (std::string(identity->object.name) == "udpsrc_ident") {
        GstRTPBuffer rtp_buf = GST_RTP_BUFFER_INIT;
        gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp_buf);
        gpointer myInfoBuf = nullptr;
        guint size_64 = 8;
        guint8 appbits = 1;
        if (gst_rtp_buffer_get_extension_twobytes_header(&rtp_buf, &appbits, 1, 0, &myInfoBuf, &size_64)) {
            if (pipelineName == "pipeline_left") {
                cameraLeftFrameId = *(static_cast<uint16_t *>(myInfoBuf));
            } else if (pipelineName == "pipeline_right") {
                cameraRightFrameId = *(static_cast<uint16_t *>(myInfoBuf));
            }
        }
        if (gst_rtp_buffer_get_extension_twobytes_header(&rtp_buf, &appbits, 1, 1, &myInfoBuf, &size_64)) {
            latestNvvidconv = *(static_cast<uint16_t *>(myInfoBuf));
        }
        if (gst_rtp_buffer_get_extension_twobytes_header(&rtp_buf, &appbits, 1, 2, &myInfoBuf, &size_64)) {
            latestJpegenc = *(static_cast<uint16_t *>(myInfoBuf));
        }
        if (gst_rtp_buffer_get_extension_twobytes_header(&rtp_buf, &appbits, 1, 3, &myInfoBuf, &size_64)) {
            latestRtpjpegpay = *(static_cast<uint16_t *>(myInfoBuf));
        }
        if (gst_rtp_buffer_get_extension_twobytes_header(&rtp_buf, &appbits, 1, 4, &myInfoBuf, &size_64)) {
            latestRtpJpegpayTimestamp = *(static_cast<uint64_t *>(myInfoBuf));
        }
        gst_rtp_buffer_unmap(&rtp_buf);
    }

    if (std::string(identity->object.name) == "vidflip_ident") {
        if (!timestampsReceiving[pipelineName].empty()) {
            const unsigned long s = timestampsReceiving[pipelineName].size();

            timestampsReceivingFiltered[pipelineName].emplace_back(timestampsReceiving[pipelineName][s - 6]);
            timestampsReceivingFiltered[pipelineName].emplace_back(timestampsReceiving[pipelineName][s - 5]);
            timestampsReceivingFiltered[pipelineName].emplace_back(timestampsReceiving[pipelineName][s - 4]);
            timestampsReceivingFiltered[pipelineName].emplace_back(timestampsReceiving[pipelineName][s - 3]);
            timestampsReceivingFiltered[pipelineName].emplace_back(timestampsReceiving[pipelineName][s - 2]);
            timestampsReceivingFiltered[pipelineName].emplace_back(timestampsReceiving[pipelineName][s - 1]);

            const unsigned long d = timestampsReceivingFiltered[pipelineName].size();

            uint16_t udpstream = timestampsReceivingFiltered[pipelineName][d - 6] - latestRtpJpegpayTimestamp;
            uint16_t rtpjpegdepay = timestampsReceivingFiltered[pipelineName][d - 5] - timestampsReceivingFiltered[pipelineName][d - 6];
            uint16_t jpegdec = timestampsReceivingFiltered[pipelineName][d - 4] - timestampsReceivingFiltered[pipelineName][d - 5];
            uint16_t queue = timestampsReceivingFiltered[pipelineName][d - 3] - timestampsReceivingFiltered[pipelineName][d - 4];
            uint16_t videoconvert = timestampsReceivingFiltered[pipelineName][d - 2] - timestampsReceivingFiltered[pipelineName][d - 3];
            uint16_t videoflip = timestampsReceivingFiltered[pipelineName][d - 1] - timestampsReceivingFiltered[pipelineName][d - 2];

            std::cout << pipelineName <<
                    ": frame - " << GetFrameId(pipelineName) <<
                    ", nvvidconv: " << latestNvvidconv <<
                    ", jpegenc: " << latestJpegenc <<
                    ", rtpjpegpay: " << latestRtpjpegpay <<
                    ", udpstream: " << udpstream <<
                    ": rtpjpegdepay: " << rtpjpegdepay <<
                    ", jpegdec: " << jpegdec <<
                    ", queue: " << queue <<
                    ", videoconvert: " << videoconvert <<
                    ", videoflip: " << videoflip <<
                        ", TOTAL: " << (latestNvvidconv + latestJpegenc + latestRtpjpegpay + udpstream + rtpjpegdepay + jpegdec + queue + videoconvert + videoflip) / 1000.0 << "ms \n";
        }
        timestampsReceiving[pipelineName].clear();
    }

    if (timestampsReceivingFiltered[pipelineName].size() > SAMPLES && BENCHMARK) {
        finishing = true;
        SaveLogFilesReceiving();
    }
}
