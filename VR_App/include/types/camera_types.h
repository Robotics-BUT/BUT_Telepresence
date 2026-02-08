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
    uint64_t rtpDepay{0};
    uint64_t dec{0};
    uint64_t queue{0};
    uint64_t presentation{0};
    uint64_t totalLatency{0};

    // Timing timestamps
    uint64_t rtpPayTimestamp{0};
    uint64_t udpSrcTimestamp{0};
    uint64_t rtpDepayTimestamp{0};
    uint64_t decTimestamp{0};
    uint64_t queueTimestamp{0};
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
    std::atomic<uint64_t> rtpDepay{0};
    std::atomic<uint64_t> dec{0};
    std::atomic<uint64_t> queue{0};
    std::atomic<uint64_t> presentation{0};
    std::atomic<uint64_t> totalLatency{0};

    // Timestamps
    std::atomic<uint64_t> rtpPayTimestamp{0};
    std::atomic<uint64_t> udpSrcTimestamp{0};
    std::atomic<uint64_t> rtpDepayTimestamp{0};
    std::atomic<uint64_t> decTimestamp{0};
    std::atomic<uint64_t> queueTimestamp{0};
    std::atomic<uint64_t> frameReadyTimestamp{0};

    // Frame info
    std::atomic<uint64_t> frameId{0};
    std::atomic<uint16_t> packetsPerFrame{0};

    // Running average configuration
    static constexpr size_t HISTORY_SIZE = 50;

    /**
     * Create a copyable snapshot of current values
     */
    CameraStatsSnapshot snapshot() const;

    /**
     * Update history with current snapshot (call after each frame)
     */
    void updateHistory();

    /**
     * Get averaged snapshot over the last N frames
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

    /* GL texture info (for hardware-decoded frames via glsinkbin) */
    bool hasGlTexture{false};
    unsigned int glTexture{0};
    unsigned int glTarget{0};     /* GL_TEXTURE_2D or GL_TEXTURE_EXTERNAL_OES */

    /* CPU buffer info (for software-decoded frames via appsink) */
    unsigned long memorySize{static_cast<unsigned long>(frameWidth * frameHeight * 3)};
    void* dataHandle{nullptr};    /* pointer to raw RGB pixel data */
};

/**
 * Stereo camera pair (left and right)
 */
using CamPair = std::pair<CameraFrame, CameraFrame>;
