/**
 * adaptation_controller.cpp - see adaptation_controller.h.
 */
#include "adaptation_controller.h"

#include <android/log.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "log.h"

namespace {

double monotonicS() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string prop(const char *name) {
    char v[PROP_VALUE_MAX] = {0};
    __system_property_get(name, v);
    return std::string(v);
}

const char *modeName(AdaptMode m) {
    switch (m) {
        case AdaptMode::Stages: return "stages";
        case AdaptMode::Naive: return "naive";
        default: return "off";
    }
}

const char *videoModeName(VideoMode m) { return m == VideoMode::Mono ? "mono" : m == VideoMode::Stereo ? "stereo" : "pano"; }

}  // namespace

AdaptationController::AdaptationController(std::shared_ptr<AppState> appState, NtpTimer *ntp, AdaptConfig config)
    : appState_(std::move(appState)), ntp_(ntp), cfg_(std::move(config)), dwellS_(cfg_.dwellS) {}

AdaptationController::~AdaptationController() { stop(); }

void AdaptationController::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this]() { loop(); });
}

void AdaptationController::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

const char *AdaptationController::leverName(Lever l) {
    switch (l) {
        case Lever::Bitrate: return "bitrate";
        case Lever::Resolution: return "resolution";
        case Lever::Mono: return "mono";
        default: return "fps";
    }
}

std::optional<std::pair<uint64_t, StreamingConfig>> AdaptationController::takeRequest() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_) return std::nullopt;
    auto r = pending_;
    inFlightId_ = r->first;
    pending_.reset();
    return r;
}

void AdaptationController::reportResult(uint64_t id, bool ok) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id != inFlightId_) return;
    const double now = monotonicS();
    if (ok) {
        // Commit only now: a failed call must not leave the controller believing it acted.
        if (inFlightDegrade_) {
            stack_.push_back({inFlightLever_, inFlightBefore_});
            // a degradation soon after a recovery means the recovery overshot: wait longer before the next one
            if (now - lastRecoverS_ < cfg_.backoffWindowS) dwellS_ = std::min(dwellS_ * 2.0, cfg_.dwellMaxS);
            lastDegradeS_ = now;
        } else if (!stack_.empty()) {
            stack_.pop_back();
            lastRecoverS_ = now;
        }
        lastActionS_[static_cast<int>(inFlightLever_)] = now;
        const bool structural = inFlightLever_ == Lever::Resolution || inFlightLever_ == Lever::Mono ||
                                inFlightLever_ == Lever::Fps;   // a frame-rate change rebuilds the headset decoders
        settleUntilS_ = now + (structural ? cfg_.settleStructuralS : cfg_.settleLiveS);
        nominalSinceS_ = -1.0;
        LOG_INFO("Adapt: applied id=%llu %s %s, stack depth %zu", (unsigned long long) id,
                 inFlightDegrade_ ? "degrade" : "recover", leverName(inFlightLever_), stack_.size());
    } else {
        LOG_WARN("Adapt: id=%llu %s %s FAILED, state unchanged", (unsigned long long) id,
                 inFlightDegrade_ ? "degrade" : "recover", leverName(inFlightLever_));
    }
    inFlightId_ = 0;
}

void AdaptationController::onApplied(const StreamingConfig &applied) {
    std::lock_guard<std::mutex> lock(mutex_);
    applied_ = applied;
}

void AdaptationController::request(const StreamingConfig &next, Lever lever, bool degrade, const char *why) {
    // caller holds mutex_
    const uint64_t id = nextId_++;
    pending_ = std::make_pair(id, next);
    inFlightLever_ = lever;
    inFlightDegrade_ = degrade;
    inFlightBefore_ = *applied_;
    LOG_INFO("Adapt: request id=%llu %s %s (%s) -> br=%d res=%s mode=%s fps=%d", (unsigned long long) id,
             degrade ? "degrade" : "recover", leverName(lever), why, next.bitrate,
             next.resolution.getLabel().c_str(), videoModeName(next.videoMode), next.fps);
}

bool AdaptationController::degradeWith(Lever lever, const StreamingConfig &cur, StreamingConfig &out) const {
    out = cur;
    switch (lever) {
        case Lever::Bitrate: {
            if (cur.bitrate <= cfg_.bitrateFloorBps) return false;
            int b = static_cast<int>(std::lround(cur.bitrate * cfg_.bitrateStep / 100000.0)) * 100000;
            out.bitrate = std::max(b, cfg_.bitrateFloorBps);
            return true;
        }
        case Lever::Resolution: {
            const auto &floorRes = CameraResolution::fromLabel(cfg_.resolutionFloor);
            if (cur.resolution.getIndex() <= floorRes.getIndex()) return false;
            out.resolution = CameraResolution::fromIndex(cur.resolution.getIndex() - 1);
            return true;
        }
        case Lever::Mono:
            if (cur.videoMode != VideoMode::Stereo) return false;
            out.videoMode = VideoMode::Mono;
            return true;
        case Lever::Fps:
            if (cur.fps <= cfg_.naiveFpsLow) return false;
            out.fps = cfg_.naiveFpsLow;
            return true;
    }
    return false;
}

void AdaptationController::loop() {
    using namespace std::chrono;
    while (running_.load()) {
        const auto t0 = steady_clock::now();
        poll();
        std::this_thread::sleep_until(t0 + duration_cast<steady_clock::duration>(duration<double>(cfg_.pollS)));
    }
}

void AdaptationController::poll() {
    const double now = monotonicS();
    const std::string modeStr = prop("debug.but.adapt");
    const AdaptMode mode = modeStr == "1" ? AdaptMode::Stages : modeStr == "2" ? AdaptMode::Naive : AdaptMode::Off;
    const std::string preset = prop("debug.but.adapt.preset");

    // --- window
    CameraStats *left = appState_->cameraStreamingStates.first.stats;
    CameraStats *right = appState_->cameraStreamingStates.second.stats;
    if (!left) return;
    const CameraWindowSummary w = left->windowSummary();
    const uint32_t lost = left->jbNumLost.load() + (right ? right->jbNumLost.load() : 0);
    const double dt = prevPollS_ > 0 ? now - prevPollS_ : cfg_.pollS;
    const double lossRate = lost >= prevLost_ ? (lost - prevLost_) / dt : 0.0;   // counters restart on a rebuild
    prevLost_ = lost;
    prevPollS_ = now;
    const double ageMs = w.frames ? (static_cast<double>(ntp_->GetCurrentTimeUs()) - w.newestUs) / 1000.0 : 1e9;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!applied_) return;
    const StreamingConfig cur = *applied_;
    const double delivered = ageMs > cfg_.stallMs ? 0.0 : w.fps / std::max(1, cur.fps);
    const double tot = w.p95TotalUs / 1000.0, net = w.p95NetworkUs / 1000.0, enc = w.p95EncUs / 1000.0,
                 dec = w.p95DecUs / 1000.0;

    const bool pDelivery = lossRate > cfg_.lossDownPerS || delivered < cfg_.deliveryDownRatio;
    const bool pNet = net > cfg_.netDownMs;
    const bool pEnc = enc > cfg_.encDownMs, pDec = dec > cfg_.decDownMs;
    const bool nominalStages = lossRate < cfg_.lossUpPerS && delivered >= cfg_.deliveryUpRatio && net < cfg_.netUpMs &&
                               enc < cfg_.encUpMs && dec < cfg_.decUpMs;
    const bool pNaive = tot > cfg_.totalTargetMs || delivered < cfg_.deliveryDownRatio;
    const bool nominalNaive = tot < cfg_.totalTargetMs && delivered >= cfg_.deliveryUpRatio;

    const char *diag = pDelivery ? "delivery" : pNet ? "network" : pEnc ? "encoder" : pDec ? "decoder" : "none";
    LOG_INFO("Adapt: mode=%s diag=%s | fps=%.1f age=%.0fms gap=%.0fms loss=%.1f/s p95 tot=%.1f net=%.1f enc=%.1f "
             "dec=%.1f | br=%d res=%s mode=%s fps=%d | stack=%zu dwell=%.0fs", modeName(mode), diag, w.fps, ageMs,
             w.maxGapUs / 1000.0, lossRate, tot, net, enc, dec, cur.bitrate, cur.resolution.getLabel().c_str(),
             videoModeName(cur.videoMode), cur.fps, stack_.size(), dwellS_);

    // --- presets for the static arms (once per change of the property); they reset the controller's stack.
    // A preset that meets a request in flight is retried at the next poll.
    if (preset != lastPreset_) {
        if (preset != "hq" && preset != "cons") {
            lastPreset_ = preset;
        } else if (!pending_ && inFlightId_ == 0) {
            lastPreset_ = preset;
            StreamingConfig next = cur;
            next.videoMode = preset == "hq" ? VideoMode::Stereo : VideoMode::Mono;
            next.resolution = CameraResolution::fromLabel(preset == "hq" ? "FHD" : "HD");
            next.bitrate = preset == "hq" ? 10000000 : 3000000;
            next.fps = 60;
            stack_.clear();
            const uint64_t id = nextId_++;
            pending_ = std::make_pair(id, next);
            inFlightLever_ = Lever::Resolution;   // treated as structural for settling
            inFlightDegrade_ = false;
            inFlightBefore_ = cur;
            LOG_INFO("Adapt: preset %s requested id=%llu", preset.c_str(), (unsigned long long) id);
            return;
        }
    }
    if (mode != lastMode_) {
        LOG_INFO("Adapt: mode %s -> %s, stack cleared", modeName(lastMode_), modeName(mode));
        lastMode_ = mode;
        stack_.clear();
        nominalSinceS_ = -1.0;
        dwellS_ = cfg_.dwellS;
    }
    if (mode == AdaptMode::Off || pending_ || inFlightId_ != 0 || now < settleUntilS_) return;

    auto ready = [&](Lever l) {
        const bool structural = l == Lever::Resolution || l == Lever::Mono || l == Lever::Fps;
        return now - lastActionS_[static_cast<int>(l)] >= (structural ? 10.0 : 3.0);
    };
    auto tryCascade = [&](std::initializer_list<Lever> levers, const char *why) {
        for (Lever l : levers) {
            StreamingConfig next;
            if (!degradeWith(l, cur, next)) continue;   // at its floor
            if (!ready(l)) return true;                 // wait for this lever's cooldown
            request(next, l, true, why);
            return true;
        }
        LOG_INFO("Adapt: every lever at its floor (%s)", why);
        return false;
    };

    if (mode == AdaptMode::Stages) {
        if (pDelivery || pNet) { nominalSinceS_ = -1.0; tryCascade({Lever::Bitrate, Lever::Resolution, Lever::Mono}, diag); return; }
        if (pEnc || pDec) { nominalSinceS_ = -1.0; tryCascade({Lever::Resolution, Lever::Mono}, diag); return; }
        if (!nominalStages) { nominalSinceS_ = -1.0; return; }
    } else {   // Naive
        if (pNaive) { nominalSinceS_ = -1.0; tryCascade({Lever::Fps}, tot > cfg_.totalTargetMs ? "total" : "delivery"); return; }
        if (!nominalNaive) { nominalSinceS_ = -1.0; return; }
    }

    // nominal: recover one step after the dwell, in reverse order of degradation
    if (nominalSinceS_ < 0) nominalSinceS_ = now;
    if (now - lastDegradeS_ > cfg_.dwellMaxS) dwellS_ = cfg_.dwellS;   // calm for a while: back to the base dwell
    if (stack_.empty() || now - nominalSinceS_ < dwellS_) return;
    const Step &top = stack_.back();
    if (!ready(top.lever)) return;
    StreamingConfig next = cur;
    switch (top.lever) {
        case Lever::Bitrate: next.bitrate = top.before.bitrate; break;
        case Lever::Resolution: next.resolution = top.before.resolution; break;
        case Lever::Mono: next.videoMode = top.before.videoMode; break;
        case Lever::Fps: next.fps = top.before.fps; break;
    }
    request(next, top.lever, false, "nominal");
}
