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
        rtpDepay.load(),
        dec.load(),
        queue.load(),
        presentation.load(),
        totalLatency.load(),
        rtpPayTimestamp.load(),
        udpSrcTimestamp.load(),
        rtpDepayTimestamp.load(),
        decTimestamp.load(),
        queueTimestamp.load(),
        frameReadyTimestamp.load(),
        frameId.load(),
        packetsPerFrame.load()
    };
}

void CameraStats::updateHistory() {
    auto snap = snapshot();
    std::lock_guard<std::mutex> lock(historyMutex_);
    history_.push_back(snap);
    if (history_.size() > HISTORY_SIZE) {
        history_.pop_front();
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
        avg.fps += snap.fps;
        avg.camera += snap.camera;
        avg.vidConv += snap.vidConv;
        avg.enc += snap.enc;
        avg.rtpPay += snap.rtpPay;
        avg.udpStream += snap.udpStream;
        avg.rtpDepay += snap.rtpDepay;
        avg.dec += snap.dec;
        avg.queue += snap.queue;
        avg.totalLatency += snap.totalLatency;
        avg.presentation += snap.presentation;
    }

    size_t count = history_.size();
    avg.prevTimestamp /= static_cast<double>(count);
    avg.currTimestamp /= static_cast<double>(count);
    avg.fps /= static_cast<double>(count);
    avg.camera /= count;
    avg.vidConv /= count;
    avg.enc /= count;
    avg.rtpPay /= count;
    avg.udpStream /= count;
    avg.rtpDepay /= count;
    avg.dec /= count;
    avg.queue /= count;
    avg.totalLatency /= count;
    avg.presentation /= count;

    // Use most recent values for non-averaged fields
    const auto& latest = history_.back();
    avg.frameId = latest.frameId;
    avg.packetsPerFrame = latest.packetsPerFrame;
    avg.rtpPayTimestamp = latest.rtpPayTimestamp;
    avg.udpSrcTimestamp = latest.udpSrcTimestamp;
    avg.rtpDepayTimestamp = latest.rtpDepayTimestamp;
    avg.decTimestamp = latest.decTimestamp;
    avg.queueTimestamp = latest.queueTimestamp;
    avg.frameReadyTimestamp = latest.frameReadyTimestamp;

    return avg;
}
