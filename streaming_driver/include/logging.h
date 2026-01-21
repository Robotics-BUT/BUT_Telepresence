//
// Created by standa on 24.1.24.
//
#pragma once
#include <fstream>
#include <map>
#include <vector>
#include <exception>
#include <gst/rtp/gstrtpbuffer.h>
#include <string_view>

// ============================================================================
// Constants
// ============================================================================

namespace PipelineNames {
    constexpr std::string_view LEFT = "pipeline_left";
    constexpr std::string_view RIGHT = "pipeline_right";
}

namespace IdentityNames {
    constexpr std::string_view CAMERA_SRC = "camsrc_ident";
    constexpr std::string_view VIDEO_CONVERT = "vidconv_ident";
    constexpr std::string_view ENCODER = "enc_ident";
    constexpr std::string_view RTP_PAYLOADER = "rtppay_ident";
}

// Pipeline stages in order (indices for timestamp arrays)
enum class Stage : size_t {
    CAMERA_SRC = 0,
    VIDEO_CONVERT = 1,
    ENCODER = 2,
    RTP_PAYLOADER = 3
};

// ============================================================================
// Per-Pipeline State
// ============================================================================

struct PipelineState {
    uint16_t frameId = 0;
    bool frameIdIncremented = false;
    uint64_t lastCameraFrameTime = 0;
    uint64_t cameraFrameDuration = 0;

    uint16_t getAndIncrementFrameId() {
        frameIdIncremented = true;
        return frameId++;
    }

    void markFrameSent() {
        frameIdIncremented = false;
    }

    void updateCameraFrameDuration(uint64_t currentTime) {
        if (lastCameraFrameTime != 0) {
            cameraFrameDuration = currentTime - lastCameraFrameTime;
        }
        lastCameraFrameTime = currentTime;
    }
};

// ============================================================================
// Global State
// ============================================================================

// Timestamps collected during current frame processing for each pipeline
inline std::map<std::string, std::vector<long>> timestampsStreaming;

// Per-pipeline state for left and right cameras
inline std::map<std::string, PipelineState> pipelineStates;

// ============================================================================
// Helper Functions
// ============================================================================

inline uint64_t GetCurrentUs() {
    using namespace std::chrono;

    struct timespec res{};
    clock_gettime(CLOCK_REALTIME, &res);
    return static_cast<uint64_t>(res.tv_sec) * 1'000'000 + res.tv_nsec / 1000;
}

inline PipelineState& GetState(const std::string& pipelineName) {
    return pipelineStates[pipelineName];
}

// ============================================================================
// Event Handlers
// ============================================================================

inline void HandleCameraSourceEvent(const std::string& pipelineName, uint64_t currentTime) {
    auto& state = GetState(pipelineName);

    // Calculate frame duration (time between consecutive frames)
    state.updateCameraFrameDuration(currentTime);

    // If we have timestamps from previous frame, mark it as sent
    if (!timestampsStreaming[pipelineName].empty()) {
        timestampsStreaming[pipelineName].clear();
        state.markFrameSent();
    }
}

inline void AddRtpHeaderMetadata(GstBuffer* buffer, const std::string& pipelineName) {
    auto& state = GetState(pipelineName);

    // Only process first RTP packet per frame
    if (state.frameIdIncremented) {
        return;
    }

    auto& currentTimestamps = timestampsStreaming[pipelineName];

    // Validate we have all required timestamps
    if (currentTimestamps.size() < 4) {
        //std::cerr << "Not enough timestamps: " << currentTimestamps.size() << "\n";
        return;
    }

    // Calculate stage durations in microseconds
    uint64_t videoConvertDuration = currentTimestamps[static_cast<size_t>(Stage::VIDEO_CONVERT)] - currentTimestamps[static_cast<size_t>(Stage::CAMERA_SRC)];
    uint64_t encoderDuration = currentTimestamps[static_cast<size_t>(Stage::ENCODER)] - currentTimestamps[static_cast<size_t>(Stage::VIDEO_CONVERT)];
    uint64_t rtpPayloaderDuration = currentTimestamps[static_cast<size_t>(Stage::RTP_PAYLOADER)] - currentTimestamps[static_cast<size_t>(Stage::ENCODER)];

    // Pack metadata into RTP header extension
    GstRTPBuffer rtpBuf = GST_RTP_BUFFER_INIT;
    if (!gst_rtp_buffer_map(buffer, GST_MAP_READWRITE, &rtpBuf)) {
        return;
    }

    uint64_t frameId = state.getAndIncrementFrameId();
    uint64_t rtpPayloaderTimestamp = currentTimestamps[static_cast<size_t>(Stage::RTP_PAYLOADER)];
    uint64_t cameraFrameDuration = state.cameraFrameDuration;

    bool success =
        gst_rtp_buffer_add_extension_onebyte_header(&rtpBuf, 1, &frameId, sizeof(frameId)) &&
        gst_rtp_buffer_add_extension_onebyte_header(&rtpBuf, 1, &cameraFrameDuration, sizeof(cameraFrameDuration)) &&
        gst_rtp_buffer_add_extension_onebyte_header(&rtpBuf, 1, &videoConvertDuration, sizeof(videoConvertDuration)) &&
        gst_rtp_buffer_add_extension_onebyte_header(&rtpBuf, 1, &encoderDuration, sizeof(encoderDuration)) &&
        gst_rtp_buffer_add_extension_onebyte_header(&rtpBuf, 1, &rtpPayloaderDuration, sizeof(rtpPayloaderDuration)) &&
        gst_rtp_buffer_add_extension_onebyte_header(&rtpBuf, 1, &rtpPayloaderTimestamp, sizeof(rtpPayloaderTimestamp));

    if (!success) {
        std::cerr << "Failed to add RTP header metadata\n";
    }

    gst_rtp_buffer_unmap(&rtpBuf);
}

// ============================================================================
// Main GStreamer Callback
// ============================================================================

inline void OnIdentityHandoffCameraStreaming(const GstElement* identity, GstBuffer* buffer, gpointer data) {
    const uint64_t currentTime = GetCurrentUs();
    const std::string pipelineName = identity->object.parent->name;
    const std::string identityName = identity->object.name;

    // Handle camera source: update frame duration and reset state for new frame
    if (identityName == IdentityNames::CAMERA_SRC) {
        HandleCameraSourceEvent(pipelineName, currentTime);
    }

    // Handle RTP payloader: add timing metadata to RTP header
    // For H264/H265, only process first packet per frame to avoid late packets from previous frame
    if (identityName == IdentityNames::RTP_PAYLOADER) {
        auto& state = GetState(pipelineName);
        if (!state.frameIdIncremented) {
            // First RTP packet for this frame - record timestamp and add metadata
            timestampsStreaming[pipelineName].emplace_back(currentTime);
            AddRtpHeaderMetadata(buffer, pipelineName);
        }
        // Skip subsequent RTP packets (they're fragments of the same frame)
        return;
    }

    // Record timestamp for non-RTP pipeline stages
    timestampsStreaming[pipelineName].emplace_back(currentTime);
}
