/**
 * stream_feedback.cpp - key-frame requests, NACKs and reconfiguration timing
 *
 * See stream_feedback.h for the protocol and the runtime switches.
 */
#include "stream_feedback.h"
#include "utils/network_utils.h"
#include <sys/system_properties.h>
#include <unistd.h>

namespace {
constexpr int64_t MS = 1000000;
constexpr int64_t KEYFRAME_MIN_INTERVAL_NS = 100 * MS;   // also enforced by the robot
constexpr int64_t KEYFRAME_LOSS_INTERVAL_NS = 250 * MS;  // one request per loss burst
constexpr int64_t RECONF_RETRY_NS = 300 * MS;            // key-frame retry while no frame arrives
constexpr int64_t RECONF_RESEND_NS = 1500 * MS;          // then re-send the config once
constexpr int64_t RECONF_GIVE_UP_NS = 5000 * MS;
// Structural changes: request key frames at these offsets after the config was sent,
// whether or not frames are already arriving (a rebuilt decoder may output frames that
// reference pictures it never had until the next IDR).
constexpr int64_t RECONF_FIXED_REQUESTS_NS[] = {0, 150 * MS, 400 * MS};

bool propOn(const char *name) {
    char v[PROP_VALUE_MAX] = {0};
    __system_property_get(name, v);
    return v[0] == '1';
}
}  // namespace

StreamFeedback::StreamFeedback(const StreamingConfig &config)
        : socket_(socket(AF_INET, SOCK_DGRAM, 0)) {
    const std::string ip = IpToString(config.jetson_ip);
    dest_.sin_family = AF_INET;
    dest_.sin_port = htons(Config::FEEDBACK_PORT);
    dest_.sin_addr.s_addr = inet_addr(ip.c_str());
    if (socket_ < 0) {
        LOG_ERROR("StreamFeedback: socket creation failed (errno=%d)", errno);
        return;
    }
    readSwitches();
    thread_ = std::thread(&StreamFeedback::loop, this);
    LOG_INFO("StreamFeedback: sending to %s:%d", ip.c_str(), Config::FEEDBACK_PORT);
}

StreamFeedback::~StreamFeedback() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    if (socket_ >= 0) close(socket_);
}

int64_t StreamFeedback::nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
}

void StreamFeedback::send(const uint8_t *data, size_t len) {
    if (socket_ >= 0) {
        sendto(socket_, data, len, 0, reinterpret_cast<const sockaddr *>(&dest_), sizeof(dest_));
    }
}

void StreamFeedback::requestKeyframe(int eye) {
    const int64_t now = nowNs();
    if (now - lastKeyframeReqNs_[eye].load() < KEYFRAME_MIN_INTERVAL_NS) return;
    lastKeyframeReqNs_[eye].store(now);
    const uint8_t msg[2] = {0x10, static_cast<uint8_t>(eye)};
    send(msg, sizeof(msg));
    keyframeReqs_[eye].fetch_add(1);
}

void StreamFeedback::onRetransmissionRequest(int eye, uint16_t seq) {
    if (!rtx_.load()) return;
    const uint8_t msg[5] = {0x11, static_cast<uint8_t>(eye), 1,
                            static_cast<uint8_t>(seq >> 8), static_cast<uint8_t>(seq & 0xFF)};
    send(msg, sizeof(msg));
    nacks_[eye].fetch_add(1);
}

void StreamFeedback::onPacketLost(int eye) {
    lost_[eye].fetch_add(1);
    if (!kfLoss_.load()) return;
    if (nowNs() - lastKeyframeReqNs_[eye].load() < KEYFRAME_LOSS_INTERVAL_NS) return;
    requestKeyframe(eye);
}

void StreamFeedback::onFrame(int eye) {
    const int64_t now = nowNs();
    if (waitingFirst_[eye].exchange(false)) firstFrameNs_[eye].store(now);
    lastFrameNs_[eye].store(now);
}

void StreamFeedback::markApply() {
    std::lock_guard<std::mutex> lk(armMutex_);
    applyNs_ = nowNs();
    for (int e = 0; e < 2; e++) lastFrameAtApplyNs_[e] = lastFrameNs_[e].load();
    armed_ = false;
}

void StreamFeedback::armReconfigure(bool structural, bool stereo) {
    std::lock_guard<std::mutex> lk(armMutex_);
    armNs_ = nowNs();
    if (applyNs_ == 0) applyNs_ = armNs_;
    structural_ = structural;
    stereo_ = stereo;
    resent_ = false;
    keyframesSent_ = 0;
    nextKeyframeNs_ = armNs_;
    // Wait for frames from here: on a structural change the old pipeline can still hand
    // over a frame while it is being torn down, between markApply() and now.
    for (int e = 0; e < 2; e++) {
        firstFrameNs_[e].store(0);
        waitingFirst_[e].store(true);
    }
    armed_ = true;
    LOG_INFO("Reconfigure: %s change sent %.1f ms after Apply (kf_reconf=%d)",
             structural ? "structural" : "live", (armNs_ - applyNs_) / 1e6, kfReconf_.load());
}

void StreamFeedback::readSwitches() {
    struct Switch { const char *prop; std::atomic<bool> *flag; };
    const Switch switches[] = {{"debug.but.rtx", &rtx_}, {"debug.but.kf_loss", &kfLoss_},
                               {"debug.but.kf_reconf", &kfReconf_}};
    for (const auto &s : switches) {
        const bool on = propOn(s.prop);
        if (on != s.flag->exchange(on)) LOG_INFO("StreamFeedback: %s = %d", s.prop, on);
    }
}

void StreamFeedback::loop() {
    int64_t nextSecond = nowNs();
    while (!stop_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const int64_t now = nowNs();

        if (now >= nextSecond) {
            nextSecond = now + 1000 * MS;
            readSwitches();
            const uint8_t heartbeat[2] = {0x12, static_cast<uint8_t>(rtx_.load() ? 1 : 0)};
            send(heartbeat, sizeof(heartbeat));
            const uint32_t lost[2] = {lost_[0].exchange(0), lost_[1].exchange(0)};
            const uint32_t nacks[2] = {nacks_[0].exchange(0), nacks_[1].exchange(0)};
            const uint32_t kfs[2] = {keyframeReqs_[0].exchange(0), keyframeReqs_[1].exchange(0)};
            if (lost[0] + lost[1] + nacks[0] + nacks[1] + kfs[0] + kfs[1] > 0) {
                LOG_INFO("StreamFeedback: lost L/R %u/%u, NACKs %u/%u, key-frame requests %u/%u",
                         lost[0], lost[1], nacks[0], nacks[1], kfs[0], kfs[1]);
            }
        }

        std::lock_guard<std::mutex> lk(armMutex_);
        if (!armed_) continue;
        const int eyes = stereo_ ? 2 : 1;

        bool allBack = true;
        for (int e = 0; e < eyes; e++) {
            if (firstFrameNs_[e].load() == 0) allBack = false;
        }
        if (allBack && (!structural_ || !kfReconf_.load() ||
                        keyframesSent_ >= (int) (sizeof(RECONF_FIXED_REQUESTS_NS) / sizeof(int64_t)))) {
            for (int e = 0; e < eyes; e++) {
                const int64_t gap = lastFrameAtApplyNs_[e] ? firstFrameNs_[e].load() - lastFrameAtApplyNs_[e] : -1;
                LOG_INFO("Reconfigure: eye %d first frame %.1f ms after Apply, gap since last frame %.1f ms",
                         e, (firstFrameNs_[e].load() - applyNs_) / 1e6, gap / 1e6);
            }
            armed_ = false;
            applyNs_ = 0;
            continue;
        }
        if (now - armNs_ > RECONF_GIVE_UP_NS) {
            LOG_ERROR("Reconfigure: no frame on every eye %.0f ms after Apply (L %d, R %d)",
                      (now - applyNs_) / 1e6, firstFrameNs_[0].load() != 0, firstFrameNs_[1].load() != 0);
            armed_ = false;
            applyNs_ = 0;
            continue;
        }
        if (!structural_ || !kfReconf_.load()) continue;

        const int fixed = (int) (sizeof(RECONF_FIXED_REQUESTS_NS) / sizeof(int64_t));
        if (keyframesSent_ < fixed && now - armNs_ >= RECONF_FIXED_REQUESTS_NS[keyframesSent_]) {
            for (int e = 0; e < eyes; e++) requestKeyframe(e);
            keyframesSent_++;
            nextKeyframeNs_ = now + RECONF_RETRY_NS;
        } else if (!allBack && now >= nextKeyframeNs_) {
            for (int e = 0; e < eyes; e++) if (firstFrameNs_[e].load() == 0) requestKeyframe(e);
            keyframesSent_++;
            nextKeyframeNs_ = now + RECONF_RETRY_NS;
        }
        if (!allBack && !resent_ && now - armNs_ > RECONF_RESEND_NS) {
            LOG_WARN("Reconfigure: no frame %.0f ms after Apply, re-sending the config", (now - applyNs_) / 1e6);
            resendConfig_.store(true);
            resent_ = true;
        }
    }
}
