/**
 * adaptation_controller.h - adaptive stream control on the headset (thesis Chapter 9).
 *
 * A thread polls the rolling per-frame window (CameraStats) about once a second, diagnoses where pressure arises,
 * and asks the render thread to apply a new StreamingConfig through the same path as the settings panel's Apply
 * (TelepresenceProgram::ApplyStreamingConfig), so structural changes rebuild the headset side correctly. The
 * controller commits its own state (degradation stack, cooldowns) only after the render thread reports success.
 *
 * Arms, selected with `adb shell setprop debug.but.adapt N` (read every poll):
 *   0  off      - no action (static arms)
 *   1  stages   - per-stage aware: delivery/network pressure -> bitrate, resolution, mono; encoder/decoder pressure
 *                 -> resolution, mono. The frame rate is never lowered (a lower rate adds latency, Chapter 7).
 *   2  naive    - aggregate only: if the total latency's P95 or the delivery is bad, lower the frame rate.
 * Presets for the static arms, `adb shell setprop debug.but.adapt.preset hq|cons` (applied once per change):
 *   hq   = stereo, FHD, 10 Mbit/s per eye, 60 FPS;   cons = mono, HD, 3 Mbit/s, 60 FPS (codec unchanged).
 * Every poll and every action is logged with the tag "Adapt:" for the evaluation (campaigns/adaptive).
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "types/app_state.h"
#include "ntp_timer.h"

enum class AdaptMode { Off = 0, Stages = 1, Naive = 2 };

struct AdaptConfig {
    double pollS{1.0};
    // Delivery: lost RTP packets per second (both eyes), delivered / configured frame rate, newest frame's age.
    // Set from the quiet link of 2026-10-23 (stereo H.265 60 FPS 10 Mbit/s, lit scene): occasional 1-4 lost/s and
    // delivery down to 0.97 with nothing wrong, so the "nominal" side leaves room for both; an 8 Mbit/s cap gives
    // hundreds of lost/s and ~3 FPS, a 1 % drop ~18 lost/s.
    double lossDownPerS{10.0}, lossUpPerS{3.0};
    double deliveryDownRatio{0.85}, deliveryUpRatio{0.95};
    double stallMs{300.0};
    // Stage P95 over the window (ms). Quiet that day: network 3-9, encode 11-15, decode ~5.
    double netDownMs{25.0}, netUpMs{15.0};
    double encDownMs{30.0}, encUpMs{25.0};
    double decDownMs{30.0}, decUpMs{25.0};
    // Naive arm: P95 of the instrumented total including the presentation stage, the quantity the telemetry and
    // campaigns/adaptive/analyse.py use (quiet: median ~66-70 ms).
    double totalTargetMs{90.0};
    // Levers
    double bitrateStep{0.6};
    int bitrateFloorBps{2000000};
    std::string resolutionFloor{"HD"};
    int naiveFpsLow{30};
    // Pacing
    double dwellS{10.0};           // nominal for this long before one step of recovery
    double dwellMaxS{60.0};        // the dwell doubles, up to this, when a recovery is undone within backoffWindowS
    double backoffWindowS{15.0};
    double settleLiveS{2.0};       // no decision this long after a live change
    double settleStructuralS{4.0}; // ... after a structural change
};

class AdaptationController {
public:
    AdaptationController(std::shared_ptr<AppState> appState, NtpTimer *ntp, AdaptConfig config = {});
    ~AdaptationController();

    void start();
    void stop();

    /** Render thread: take the pending request, if any. */
    std::optional<std::pair<uint64_t, StreamingConfig>> takeRequest();
    /** Render thread: result of a taken request. */
    void reportResult(uint64_t id, bool ok);
    /** Render thread: any successful Apply (panel or controller) sets the baseline. */
    void onApplied(const StreamingConfig &applied);

private:
    enum class Lever { Bitrate, Resolution, Mono, Fps };
    struct Step { Lever lever; StreamingConfig before; };   // one degradation, with the config to return to

    void loop();
    void poll();
    void request(const StreamingConfig &cfg, Lever lever, bool degrade, const char *why);
    bool degradeWith(Lever lever, const StreamingConfig &cur, StreamingConfig &out) const;
    static const char *leverName(Lever l);

    std::shared_ptr<AppState> appState_;
    NtpTimer *ntp_;
    AdaptConfig cfg_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    std::mutex mutex_;                            // guards everything below
    std::optional<StreamingConfig> applied_;      // last config the robot acknowledged
    uint64_t nextId_{1};
    std::optional<std::pair<uint64_t, StreamingConfig>> pending_;   // posted, not yet taken
    uint64_t inFlightId_{0};                      // taken, result not yet reported
    Lever inFlightLever_{Lever::Bitrate};
    bool inFlightDegrade_{false};
    StreamingConfig inFlightBefore_{};
    std::vector<Step> stack_;
    double settleUntilS_{0.0};
    double nominalSinceS_{-1.0};
    double lastActionS_[4]{-1e9, -1e9, -1e9, -1e9};
    double dwellS_{0.0};                          // current dwell (backs off after a failed recovery)
    double lastRecoverS_{-1e9}, lastDegradeS_{-1e9};

    // poll-local history
    uint32_t prevLost_{0};
    double prevPollS_{0.0};
    std::string lastPreset_;
    AdaptMode lastMode_{AdaptMode::Off};
};
