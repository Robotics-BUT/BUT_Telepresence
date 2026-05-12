/**
 * camera_types.h - Camera frame data, statistics, and resolution definitions
 *
 * Defines the types used throughout the video pipeline:
 * - CameraResolution: predefined resolution presets (nHD through UHD)
 * - CameraStats / CameraStatsSnapshot: thread-safe per-frame pipeline latency tracking
 * - CameraFrame: a single decoded video frame (GL texture or CPU buffer)
 * - CamPair: stereo pair alias (left + right camera frames)
 */
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <deque>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <stdexcept>
#include <utility>

// =============================================================================
// Camera Resolution
// =============================================================================

/**
 * Predefined camera resolutions with human-readable labels.
 * Resolutions are stored in a sorted list (nHD to UHD) and can be
 * looked up by label string or by index for sequential navigation.
 */
struct CameraResolution {
    int width;
    int height;
    std::string label;

    int getWidth() const { return width; }
    int getHeight() const { return height; }
    float getAspectRatio() const { return static_cast<float>(width) / static_cast<float>(height); }
    const std::string& getLabel() const { return label; }

    static const CameraResolution& fromLabel(const std::string& label) {
        const auto& map = getMap();
        auto it = map.find(label);
        if (it != map.end()) {
            return it->second;
        }
        throw std::invalid_argument("Invalid resolution label: " + label);
    }

    static const CameraResolution& fromIndex(std::size_t index) {
        const auto& list = getList();
        if (index >= list.size()) {
            throw std::out_of_range("Resolution index out of range");
        }
        return list[index];
    }

    std::size_t getIndex() const {
        const auto& list = getList();
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (width == list[i].width && height == list[i].height) {
                return i;
            }
        }
        throw std::runtime_error("Resolution not found in predefined list");
    }

    static std::size_t count() {
        return getList().size();
    }

private:
    static const std::unordered_map<std::string, CameraResolution>& getMap() {
        static const std::unordered_map<std::string, CameraResolution> map = {
            {"nHD",    {640,  360,  "nHD"}},
            {"qHD",    {960,  540,  "qHD"}},
            {"WSVGA",  {1024, 576,  "WSVGA"}},
            {"HD",     {1280, 720,  "HD"}},
            {"HD+",    {1600, 900,  "HD+"}},
            {"FHD",    {1920, 1080, "FHD"}},
            {"QWXGA",  {2048, 1152, "QWXGA"}},
            {"QHD",    {2560, 1440, "QHD"}},
            {"WQXGA+", {3200, 1800, "WQXGA+"}},
            {"UHD",    {3840, 2160, "UHD"}},
        };
        return map;
    }

    static const std::vector<CameraResolution>& getList() {
        static const std::vector<CameraResolution> list = {
            {640,  360,  "nHD"},
            {960,  540,  "qHD"},
            {1024, 576,  "WSVGA"},
            {1280, 720,  "HD"},
            {1600, 900,  "HD+"},
            {1920, 1080, "FHD"},
            {2048, 1152, "QWXGA"},
            {2560, 1440, "QHD"},
            {3200, 1800, "WQXGA+"},
            {3840, 2160, "UHD"},
        };
        return list;
    }
};

// =============================================================================
// Per-frame PTS-keyed timestamp map
// =============================================================================

/**
 * Bounded thread-safe map from GstBuffer PTS → wall-clock timestamp (µs).
 *
 * Used to correctly attribute per-stage latency in pipelined async stages
 * (HW decoder, queues). Without this, the GStreamer probe that fires when
 * frame N exits a stage reads the GLOBAL "latest enter timestamp" — which
 * has been overwritten by frame N+depth's enter event, making the probe
 * blind to the pipeline depth.
 *
 * Each stage stores `now` keyed by buffer.pts on its enter callback, and
 * the downstream stage consumes that pts to get the *correct* per-frame
 * enter time for its delta computation.
 *
 * Capacity is fixed; if exceeded (e.g., frames dropped between probes),
 * the oldest half is evicted on next insert. This keeps memory bounded
 * even under sustained loss without coordinating with frame_id.
 */
class PtsTimestampMap {
public:
    static constexpr size_t MAX_SIZE = 64;

    void store(uint64_t pts, uint64_t time_us) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (map_.size() >= MAX_SIZE) {
            map_.clear();  // coarse eviction; fine for telemetry
        }
        map_[pts] = time_us;
    }

    // Returns 0 if pts not found (caller should treat 0 as "skip this delta").
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

// =============================================================================
// Camera Statistics
// =============================================================================

/**
 * Copyable snapshot of camera stats for passing values between threads.
 * All latency values are in microseconds. The pipeline stages correspond
 * to GStreamer identity probe points inserted along the decoding pipeline.
 */
struct CameraStatsSnapshot {
    double prevTimestamp{0.0};
    double currTimestamp{0.0};
    double fps{0.0};

    /* Pipeline stage latencies (microseconds) */
    uint64_t camera{0};
    uint64_t vidConv{0};
    uint64_t enc{0};
    uint64_t rtpPay{0};
    uint64_t udpStream{0};
    uint64_t jbHold{0};        // rtpjitterbuffer hold time (udpsrc_ident -> postjb_ident, keyed by RTP timestamp)
    uint64_t rtpDepay{0};
    uint64_t dec{0};
    uint64_t queue{0};
    uint64_t appsink{0};       // queue_ident -> new-sample callback (glsinkbin GL upload + appsink hand-off; near-zero on JPEG)
    uint64_t presentation{0};  // new-sample callback -> predicted photon emission
    uint64_t totalLatency{0};

    // Timing timestamps
    uint64_t rtpPayTimestamp{0};
    uint64_t frameReadyTimestamp{0};

    // Frame info
    uint64_t frameId{0};
    uint16_t packetsPerFrame{0};
};

/**
 * Thread-safe camera statistics with running average support.
 * Uses std::atomic for lock-free reads from the render thread while
 * GStreamer callbacks write from pipeline threads. The running average
 * is computed over the last HISTORY_SIZE (50) frames.
 */
struct CameraStats {
    /* Timing */
    std::atomic<double> prevTimestamp{0.0};
    std::atomic<double> currTimestamp{0.0};
    std::atomic<double> fps{0.0};

    // Pipeline stage latencies
    std::atomic<uint64_t> camera{0};
    std::atomic<uint64_t> vidConv{0};
    std::atomic<uint64_t> enc{0};
    std::atomic<uint64_t> rtpPay{0};
    std::atomic<uint64_t> udpStream{0};
    std::atomic<uint64_t> jbHold{0};
    std::atomic<uint64_t> rtpDepay{0};
    std::atomic<uint64_t> dec{0};
    std::atomic<uint64_t> queue{0};
    std::atomic<uint64_t> appsink{0};  // queue_ident -> appsink new-sample callback
    std::atomic<uint64_t> presentation{0};  // appsink -> predicted photon emission
    std::atomic<uint64_t> totalLatency{0};

    // Timestamps
    std::atomic<uint64_t> rtpPayTimestamp{0};
    std::atomic<uint64_t> frameReadyTimestamp{0};

    // Frame info
    std::atomic<uint64_t> frameId{0};
    std::atomic<uint16_t> packetsPerFrame{0};

    // Only measure presentation on the first render after a new frame arrives
    std::atomic<uint64_t> lastMeasuredFrameReady{0};

    // Per-frame enter-time maps. Each stage stores its emit time so the
    // downstream stage can compute the *correct* per-frame delta (instead
    // of being fooled by a global "latest" timestamp when the stage is
    // async/pipelined). See comment on PtsTimestampMap.
    //
    // Key choice follows the invariant that survives the interval:
    //   - rtpTsArrivalMap is keyed by the RTP timestamp (uint32_t cast to
    //     uint64_t) because rtpjitterbuffer rewrites GstBuffer PTS across
    //     its interval; the RTP timestamp does not change.
    //   - All other maps are keyed by GstBuffer PTS, which is preserved
    //     across the post-jitterbuffer stages (depay, decoder, queue).
    PtsTimestampMap rtpTsArrivalMap;  // first-packet arrival per RTP ts (udpsrc -> postjb)
    PtsTimestampMap postjbPtsMap;     // post-jitterbuffer emit per pts  (postjb -> rtpdepay)
    PtsTimestampMap depayPtsMap;      // rtpdepay emit per pts            (rtpdepay -> dec)
    PtsTimestampMap decPtsMap;        // amcviddec emit per pts           (dec -> queue)
    PtsTimestampMap queuePtsMap;      // post-decoder queue emit per pts  (queue -> appsink)

    // Per-packet dedup: every RTP packet of one frame carries the same RTP
    // timestamp; rtpTsArrivalMap should record only the first packet's arrival.
    std::atomic<uint32_t> lastSeenRtpTs{0};

    /**
     * Create a copyable snapshot of current values
     */
    CameraStatsSnapshot snapshot() const;

    /**
     * Update history with current snapshot. windowFrames is the configured
     * stream FPS — caller passes streamingConfig.fps so the rolling window
     * always covers ≈1 s of data, regardless of the chosen rate.
     */
    void updateHistory(size_t windowFrames);

    /**
     * Get averaged snapshot. Per-stage latencies are arithmetic means over
     * the deque; fps is the windowed rate (n−1)/(last_ts−first_ts), which
     * is robust to decoder bursts that inflate per-frame 1/Δt readings.
     */
    CameraStatsSnapshot averagedSnapshot() const;

private:
    mutable std::mutex historyMutex_;
    mutable std::deque<CameraStatsSnapshot> history_;
};

// =============================================================================
// Camera Frame
// =============================================================================

/**
 * Single camera frame data and metadata.
 * Depending on the codec, a frame is either a GL texture (hardware-decoded
 * H264/H265 via Qualcomm AMC) or a CPU buffer (software-decoded JPEG).
 */
struct CameraFrame {
    CameraStats* stats{nullptr};

    int frameWidth{CameraResolution::fromLabel("FHD").getWidth()};
    int frameHeight{CameraResolution::fromLabel("FHD").getHeight()};

    /* For HW frames, glTexture points to an *app-owned* GL_TEXTURE_2D backed
     * by hwBackingTex below — populated by an OES->2D blit on the GstGL
     * worker thread. Render thread therefore never touches a SurfaceTexture-
     * backed external-OES handle whose underlying gralloc buffer can be
     * swapped asynchronously by MediaCodec.updateTexImage(). */
    bool hasGlTexture{false};
    unsigned int glTexture{0};
    unsigned int glTarget{0};     /* GL_TEXTURE_2D or GL_TEXTURE_EXTERNAL_OES */

    /* CPU buffer info (for software-decoded frames via appsink) */
    unsigned long memorySize{static_cast<unsigned long>(frameWidth * frameHeight * 3)};
    void* dataHandle{nullptr};    /* pointer to raw RGB pixel data */

    /* App-owned GL_TEXTURE_2D + FBO that the OES->2D blit writes into.
     * Allocated lazily on the GstGL worker thread; reused across frames at
     * the same resolution; reallocated on resolution change. */
    unsigned int hwBackingTex{0};
    unsigned int hwBackingFBO{0};
    int          hwBackingWidth{0};
    int          hwBackingHeight{0};

    /* Serializes GStreamer field-publish against render-thread reads. */
    mutable std::mutex frameMutex;
};

/**
 * Stereo camera pair (left and right)
 */
using CamPair = std::pair<CameraFrame, CameraFrame>;
