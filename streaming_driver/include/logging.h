//
// Created by standa on 24.1.24.
//
#pragma once
#include <fstream>
#include <mutex>
#include <unordered_map>
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

// IMX415 sensor + Argus capture-to-output latency. The sensor does not expose
// per-frame capture timestamps via Argus on this L4T version, so this static
// value is reported alongside the dynamic per-stage measurements to keep the
// latency budget complete.
//
// 2026-05-18: model revised to a fixed wall-clock latency of 37.5 ms,
// corresponding to 3 sensor frames at the fixed 80 Hz nvarguscamerasrc capture
// rate, consistent with NVIDIA's documented Argus capture-to-output pipeline
// depth of ~3-4 sensor frames (see paper §VI.B). The prior 1 x output-frame
// model under-counted at 60 FPS because the sensor's capture cadence is
// independent of the output FPS, so the real camera contribution does not
// shrink at higher output rates. SetSensorStaticLatencyForFps() is retained
// as a no-op to preserve the main.cpp call site.
inline uint64_t SENSOR_STATIC_LATENCY_US = 37500;  // 3 sensor frames at 80 Hz

inline void SetSensorStaticLatencyForFps(uint32_t /*fps*/) {
    // Intentionally a no-op: the camera/ISP latency is wall-clock-fixed and
    // does not depend on the output frame rate. See the comment above.
}

// ============================================================================
// Per-frame PTS-keyed timestamp map
// ============================================================================

/**
 * Bounded thread-safe map from GstBuffer PTS -> wall-clock timestamp (us).
 *
 * Used to correctly attribute per-stage latency in pipelined async stages
 * (NVENC encoder). Without this, a probe that fires when frame N exits a
 * stage reads the GLOBAL "latest enter timestamp" — which has been
 * overwritten by frame N+depth's enter event, masking pipeline depth.
 *
 * Each stage stores `now` keyed by buffer.pts on its enter callback, and
 * the downstream stage consumes that pts to get the *correct* per-frame
 * enter time for its delta computation.
 *
 * Capacity is fixed; if exceeded (e.g., frames dropped between probes),
 * the map is cleared on next insert. Memory stays bounded even under
 * sustained loss.
 */
class PtsTimestampMap {
public:
    static constexpr size_t MAX_SIZE = 64;

    void store(uint64_t pts, uint64_t time_us) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (map_.size() >= MAX_SIZE) {
            map_.clear();
        }
        map_[pts] = time_us;
    }

    // Returns 0 if pts not found.
    uint64_t consume(uint64_t pts) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = map_.find(pts);
        if (it == map_.end()) return 0;
        uint64_t t = it->second;
        map_.erase(it);
        return t;
    }

private:
    mutable std::mutex mtx_;
    std::unordered_map<uint64_t, uint64_t> map_;
};

// ============================================================================
// Per-Pipeline State
// ============================================================================

struct PipelineState {
    uint16_t frameId = 0;
    uint64_t lastEmbeddedPts = 0;  // dedup: skip subsequent RTP packets of the same frame

    // Static sensor + Argus latency contribution per frame
    uint64_t cameraFrameDuration = 0;

    // Per-frame PTS -> stage emit time
    PtsTimestampMap camsrcPtsMap;
    PtsTimestampMap vidconvPtsMap;
    PtsTimestampMap encPtsMap;

    uint16_t getAndIncrementFrameId() {
        return frameId++;
    }
};

// ============================================================================
// Global State
// ============================================================================

inline std::map<std::string, PipelineState> pipelineStates;

// ============================================================================
// Helper Functions
// ============================================================================

inline uint64_t GetCurrentUs() {
    struct timespec res{};
    clock_gettime(CLOCK_REALTIME, &res);
    return static_cast<uint64_t>(res.tv_sec) * 1'000'000 + res.tv_nsec / 1000;
}

inline PipelineState& GetState(const std::string& pipelineName) {
    return pipelineStates[pipelineName];
}

// ============================================================================
// RTP Header Metadata
// ============================================================================

inline void AddRtpHeaderMetadataPerFrame(GstBuffer* buffer, PipelineState& state,
                                         uint64_t vidConvDuration, uint64_t encDuration,
                                         uint64_t rtpPayDuration, uint64_t rtpPayTimestamp) {
    GstRTPBuffer rtpBuf = GST_RTP_BUFFER_INIT;
    if (!gst_rtp_buffer_map(buffer, GST_MAP_READWRITE, &rtpBuf)) {
        return;
    }

    uint64_t frameId = state.getAndIncrementFrameId();
    uint64_t cameraFrameDuration = state.cameraFrameDuration;

    bool success =
        gst_rtp_buffer_add_extension_onebyte_header(&rtpBuf, 1, &frameId, sizeof(frameId)) &&
        gst_rtp_buffer_add_extension_onebyte_header(&rtpBuf, 1, &cameraFrameDuration, sizeof(cameraFrameDuration)) &&
        gst_rtp_buffer_add_extension_onebyte_header(&rtpBuf, 1, &vidConvDuration, sizeof(vidConvDuration)) &&
        gst_rtp_buffer_add_extension_onebyte_header(&rtpBuf, 1, &encDuration, sizeof(encDuration)) &&
        gst_rtp_buffer_add_extension_onebyte_header(&rtpBuf, 1, &rtpPayDuration, sizeof(rtpPayDuration)) &&
        gst_rtp_buffer_add_extension_onebyte_header(&rtpBuf, 1, &rtpPayTimestamp, sizeof(rtpPayTimestamp));

    if (!success) {
        std::cerr << "Failed to add RTP header metadata\n";
    }

    gst_rtp_buffer_unmap(&rtpBuf);
}

// ============================================================================
// Main GStreamer Callback
// ============================================================================

inline void OnIdentityHandoffCameraStreaming(GstElement* identity, GstBuffer* buffer, gpointer /*data*/) {
    const uint64_t now = GetCurrentUs();
    const std::string pipelineName = identity->object.parent->name;
    const std::string identityName = identity->object.name;

    auto& state = GetState(pipelineName);

    GstClockTime pts = GST_BUFFER_PTS(buffer);
    if (pts == GST_CLOCK_TIME_NONE) return;
    uint64_t ptsKey = static_cast<uint64_t>(pts);

    if (identityName == IdentityNames::CAMERA_SRC) {
        // Static sensor + Argus latency contribution (unchanged from pre-patch).
        state.cameraFrameDuration = SENSOR_STATIC_LATENCY_US;
        state.camsrcPtsMap.store(ptsKey, now);
    }
    else if (identityName == IdentityNames::VIDEO_CONVERT) {
        state.vidconvPtsMap.store(ptsKey, now);
    }
    else if (identityName == IdentityNames::ENCODER) {
        state.encPtsMap.store(ptsKey, now);
    }
    else if (identityName == IdentityNames::RTP_PAYLOADER) {
        // Dedup: same pts means subsequent RTP packet of the same frame — skip.
        if (ptsKey == state.lastEmbeddedPts) return;

        // Per-frame durations: look up THIS pts at each upstream stage.
        // Critical for NVENC pipeline visibility — the previous global-vector
        // scheme paired enc's emit time with frame N+depth's camsrc/vidconv,
        // masking encoder pipeline depth as ~0 ms.
        uint64_t camsrcTime  = state.camsrcPtsMap.consume(ptsKey);
        uint64_t vidconvTime = state.vidconvPtsMap.consume(ptsKey);
        uint64_t encTime     = state.encPtsMap.consume(ptsKey);

        if (camsrcTime == 0 || vidconvTime == 0 || encTime == 0) {
            // Missing upstream — frame may have been dropped between stages.
            // Skip embedding to avoid embedding garbage durations.
            return;
        }

        uint64_t vidConvDuration = (vidconvTime > camsrcTime) ? (vidconvTime - camsrcTime) : 0;
        uint64_t encDuration     = (encTime > vidconvTime)    ? (encTime - vidconvTime)    : 0;
        uint64_t rtpPayDuration  = (now > encTime)            ? (now - encTime)            : 0;

        AddRtpHeaderMetadataPerFrame(buffer, state, vidConvDuration, encDuration, rtpPayDuration, now);
        state.lastEmbeddedPts = ptsKey;
    }
}
