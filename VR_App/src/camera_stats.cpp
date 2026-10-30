/**
 * camera_stats.cpp - Camera statistics snapshot and averaging
 *
 * Implements thread-safe snapshot capture from atomic CameraStats fields,
 * history ring buffer management (HISTORY_SIZE entries), and running
 * average computation across the history for smooth latency display.
 * Timing fields (camera, enc, dec, etc.) are averaged; metadata fields
 * (frameId, timestamps) use the most recent value.
 */
#include "types/camera_types.h"

#include <algorithm>
#include <cmath>
#include <vector>

CameraStatsSnapshot CameraStats::snapshot() const {
    return CameraStatsSnapshot{
        prevTimestamp.load(),
        currTimestamp.load(),
        fps.load(),
        camera.load(),
        vidConv.load(),
        enc.load(),
        rtpPay.load(),
        udpStream.load(),
        jbHold.load(),
        rtpDepay.load(),
        dec.load(),
        queue.load(),
        appsink.load(),
        presentation.load(),
        totalLatency.load(),
        rtpPayTimestamp.load(),
        frameReadyTimestamp.load(),
        frameId.load(),
        packetsPerFrame.load(),
        jbNumLost.load(),
        rtxCount.load(),
        jitterUs.load(),
        actualBitrateBps.load()
    };
}

void CameraStats::updateHistory(size_t windowFrames) {
    if (windowFrames < 2) windowFrames = 2;  // need at least 2 samples for fps
    auto snap = snapshot();
    std::lock_guard<std::mutex> lock(historyMutex_);
    history_.push_back(snap);
    while (history_.size() > windowFrames) {
        history_.pop_front();
    }

    // Recompute fps as the windowed rate over the deque so the value seen via
    // snapshot() (which goes to InfluxDB) is stable across HW-decoder bursts,
    // not the per-frame 1e6/Δt that swings into the kHz range on bursts.
    if (history_.size() >= 2) {
        double first = history_.front().currTimestamp;
        double last = history_.back().currTimestamp;
        double dt_us = last - first;
        if (dt_us > 0.0) {
            double windowed_fps = (history_.size() - 1) * 1e6 / dt_us;
            fps.store(windowed_fps);
        }
    }
}

CameraStatsSnapshot CameraStats::averagedSnapshot() const {
    std::lock_guard<std::mutex> lock(historyMutex_);

    if (history_.empty()) {
        return snapshot();
    }

    CameraStatsSnapshot avg{};
    for (const auto& snap : history_) {
        avg.prevTimestamp += snap.prevTimestamp;
        avg.currTimestamp += snap.currTimestamp;
        avg.camera += snap.camera;
        avg.vidConv += snap.vidConv;
        avg.enc += snap.enc;
        avg.rtpPay += snap.rtpPay;
        avg.udpStream += snap.udpStream;
        avg.jbHold += snap.jbHold;
        avg.rtpDepay += snap.rtpDepay;
        avg.dec += snap.dec;
        avg.queue += snap.queue;
        avg.appsink += snap.appsink;
        avg.presentation += snap.presentation;
        avg.totalLatency += snap.totalLatency;
    }

    size_t count = history_.size();
    avg.prevTimestamp /= static_cast<double>(count);
    avg.currTimestamp /= static_cast<double>(count);
    avg.camera /= count;
    avg.vidConv /= count;
    avg.enc /= count;
    avg.rtpPay /= count;
    avg.udpStream /= count;
    avg.jbHold /= count;
    avg.rtpDepay /= count;
    avg.dec /= count;
    avg.queue /= count;
    avg.appsink /= count;
    avg.presentation /= count;
    avg.totalLatency /= count;

    // fps as the true windowed rate, not arithmetic mean of per-frame ratios.
    if (count >= 2) {
        double dt_us = history_.back().currTimestamp - history_.front().currTimestamp;
        avg.fps = (dt_us > 0.0) ? (count - 1) * 1e6 / dt_us : 0.0;
    } else {
        avg.fps = 0.0;
    }

    // Use most recent values for non-averaged fields
    const auto& latest = history_.back();
    avg.frameId = latest.frameId;
    avg.packetsPerFrame = latest.packetsPerFrame;
    avg.rtpPayTimestamp = latest.rtpPayTimestamp;
    avg.frameReadyTimestamp = latest.frameReadyTimestamp;

    return avg;
}

namespace {
double p95(std::vector<double> &v) {
    if (v.empty()) return 0.0;
    const size_t k = static_cast<size_t>(std::ceil(0.95 * v.size())) - 1;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}
}  // namespace

CameraWindowSummary CameraStats::windowSummary() const {
    std::lock_guard<std::mutex> lock(historyMutex_);
    CameraWindowSummary w;
    w.frames = history_.size();
    if (w.frames == 0) return w;
    std::vector<double> tot, net, enc, dec;
    tot.reserve(w.frames); net.reserve(w.frames); enc.reserve(w.frames); dec.reserve(w.frames);
    for (size_t i = 0; i < history_.size(); ++i) {
        const auto &s = history_[i];
        tot.push_back(static_cast<double>(s.totalLatency + s.presentation));   // as the telemetry total
        net.push_back(static_cast<double>(s.udpStream + s.jbHold));
        enc.push_back(static_cast<double>(s.enc));
        dec.push_back(static_cast<double>(s.dec));
        if (i > 0) w.maxGapUs = std::max(w.maxGapUs, s.currTimestamp - history_[i - 1].currTimestamp);
    }
    w.newestUs = history_.back().currTimestamp;
    const double span = history_.back().currTimestamp - history_.front().currTimestamp;
    if (w.frames >= 2 && span > 0.0) w.fps = (w.frames - 1) * 1e6 / span;
    w.p95TotalUs = p95(tot); w.p95NetworkUs = p95(net); w.p95EncUs = p95(enc); w.p95DecUs = p95(dec);
    return w;
}
