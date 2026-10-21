/**
 * stream_feedback.h - headset -> video driver feedback channel
 *
 * Sends key-frame requests and NACKs for missing RTP packets to the robot's video
 * driver on Config::FEEDBACK_PORT, and times how long the video takes to come back
 * after a configuration change. Protocol (multi-byte fields in network byte order):
 *   0x10 KEYFRAME_REQ  [0x10][eye]
 *   0x11 NACK          [0x11][eye][n][seq u16] x n
 *   0x12 RTX_MODE      [0x12][enabled]            heartbeat, 1 Hz
 * eye: 0 = left (also mono), 1 = right.
 *
 * Every behaviour is off by default and switched at runtime (checked once a second):
 *   adb shell setprop debug.but.rtx 1        NACK packets the jitter buffer asks for
 *   adb shell setprop debug.but.kf_loss 1    request a key frame when a packet is declared lost
 *   adb shell setprop debug.but.kf_reconf 1  request key frames after a structural Apply, and
 *                                            re-send the config if no frame arrives in 1.5 s
 * The reconfiguration timing is logged in every mode ("Reconfigure: ...").
 */
#pragma once

#include "pch.h"
#include "log.h"
#include "types/app_state.h"
#include <arpa/inet.h>
#include <atomic>
#include <mutex>
#include <thread>

class StreamFeedback {
public:
    explicit StreamFeedback(const StreamingConfig &config);
    ~StreamFeedback();

    /* GStreamer streaming threads */
    void onRetransmissionRequest(int eye, uint16_t seq);
    void onPacketLost(int eye);
    void onFrame(int eye);

    /* Render thread: call markApply() when Apply is pressed and armReconfigure() once
     * the config has been sent to the robot. */
    void markApply();
    void armReconfigure(bool structural, bool stereo);
    /** True once when the watchdog wants the last config re-sent to the robot. */
    bool takeConfigResend() { return resendConfig_.exchange(false); }

private:
    static int64_t nowNs();
    void loop();
    void readSwitches();
    void send(const uint8_t *data, size_t len);
    void requestKeyframe(int eye);

    int socket_{-1};
    sockaddr_in dest_{};
    std::thread thread_;
    std::atomic<bool> stop_{false};

    std::atomic<bool> rtx_{false}, kfLoss_{false}, kfReconf_{false};

    std::atomic<int64_t> lastFrameNs_[2]{};
    std::atomic<bool> waitingFirst_[2]{};
    std::atomic<int64_t> firstFrameNs_[2]{};      // first frame after markApply()
    std::atomic<int64_t> lastKeyframeReqNs_[2]{};
    std::atomic<uint32_t> nacks_[2]{}, lost_[2]{}, keyframeReqs_[2]{};

    // Reconfiguration watchdog, guarded by armMutex_
    std::mutex armMutex_;
    bool armed_{false}, structural_{false}, stereo_{true}, resent_{false};
    int64_t applyNs_{0}, armNs_{0}, nextKeyframeNs_{0};
    int keyframesSent_{0};
    int64_t lastFrameAtApplyNs_[2]{};
    std::atomic<bool> resendConfig_{false};
};
