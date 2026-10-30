/**
 * settings_table.cpp - The rows of the VR settings panel (see settings_table.h)
 */
#include "settings_table.h"

#include <algorithm>
#include <cmath>
#define FMT_HEADER_ONLY
#include "fmt/core.h"
#include "utils/network_utils.h"

namespace {

// The codecs and video modes offered, in display order
const std::vector<Codec> kCodecs = {Codec::JPEG, Codec::H264, Codec::H265};
const std::vector<VideoMode> kModes = {VideoMode::Stereo, VideoMode::Mono, VideoMode::Panoramic};
const std::vector<AspectRatioMode> kViews = {AspectRatioMode::FullFOV, AspectRatioMode::FullScreen};
// Frame rates the stepper moves between; a value set elsewhere steps to the next one in the list
const std::vector<int> kFrameRates = {10, 15, 20, 24, 30, 40, 45, 50, 60, 72, 80};

template<typename T>
int indexOf(const std::vector<T> &list, const T &value) {
    auto it = std::find(list.begin(), list.end(), value);
    return it == list.end() ? 0 : static_cast<int>(it - list.begin());
}

int nextRate(int fps, int dir) {
    if (dir > 0) {
        for (int r : kFrameRates) if (r > fps) return r;
        return kFrameRates.back();
    }
    for (auto it = kFrameRates.rbegin(); it != kFrameRates.rend(); ++it) if (*it < fps) return *it;
    return kFrameRates.front();
}

int stepBitrate(int bps, int dir) {   // 1 Mbit/s steps up to 20 Mbit/s, 5 Mbit/s above
    const int mbps = bps / 1000000;
    const int step = (dir > 0 ? mbps >= 20 : mbps > 20) ? 5 : 1;
    return std::clamp(mbps + dir * step, 1, 100) * 1000000;
}

}  // namespace

int PendingStreamChanges(const StreamingConfig &now, const StreamingConfig *applied) {
    if (!applied) return 1;
    return (now.codec != applied->codec) + (now.encodingQuality != applied->encodingQuality) +
           (now.bitrate != applied->bitrate) + (now.videoMode != applied->videoMode) + (now.fps != applied->fps) +
           (now.resolution.getIndex() != applied->resolution.getIndex()) + (now.jetson_ip != applied->jetson_ip);
}

std::vector<GuiSetting> BuildSettingsTable(const std::shared_ptr<AppState> &appState, const SettingsActions &actions) {
    auto &cfg = appState->streamingConfig;
    auto applied = actions.appliedConfig;
    // A stream setting differs from the config in force: marked on its row until applied
    auto pending = [applied](auto field) {
        return [applied, field]() { const StreamingConfig *a = applied(); return !a || field(*a); };
    };

    std::vector<GuiSetting> rows;
    auto add = [&rows](GuiSetting s) -> GuiSetting & { rows.push_back(std::move(s)); return rows.back(); };
    auto choice = [&add](std::string label, GuiTab tab, std::vector<std::string> options, std::function<int()> get,
                         std::function<void(int)> set) -> GuiSetting & {
        GuiSetting s;
        s.label = std::move(label); s.control = GuiControl::Choice; s.tab = tab;
        const int n = static_cast<int>(options.size());
        s.choices = std::move(options); s.getChoice = get; s.setChoice = set;
        s.onIncrement = [get, set, n]() { set((get() + 1) % n); };
        s.onDecrement = [get, set, n]() { set((get() + n - 1) % n); };
        return add(std::move(s));
    };
    auto toggle = [&add](std::string label, GuiTab tab, std::function<bool()> get,
                         std::function<void(bool)> set) -> GuiSetting & {
        GuiSetting s;
        s.label = std::move(label); s.control = GuiControl::Toggle; s.tab = tab;
        s.getToggle = get; s.setToggle = set;
        s.onIncrement = [set]() { set(true); };
        s.onDecrement = [set]() { set(false); };
        return add(std::move(s));
    };
    auto stepper = [&add](std::string label, GuiTab tab, std::function<std::string()> value, std::function<void()> inc,
                          std::function<void()> dec) -> GuiSetting & {
        GuiSetting s;
        s.label = std::move(label); s.control = GuiControl::Stepper; s.tab = tab;
        s.getValue = std::move(value); s.onIncrement = std::move(inc); s.onDecrement = std::move(dec);
        return add(std::move(s));
    };

    // --- Stream: takes effect on Apply ---
    choice("Codec", GuiTab::Stream, {"JPEG", "H.264", "H.265"},
           [&cfg]() { return indexOf(kCodecs, cfg.codec); },
           [&cfg](int i) { cfg.codec = kCodecs[i]; })
        .isPending = pending([&cfg](const StreamingConfig &a) { return a.codec != cfg.codec; });
    choice("Video", GuiTab::Stream, {"Stereo", "Mono", "Pano"},
           [&cfg]() { return indexOf(kModes, cfg.videoMode); },
           [&cfg](int i) { cfg.videoMode = kModes[i]; })
        .isPending = pending([&cfg](const StreamingConfig &a) { return a.videoMode != cfg.videoMode; });
    stepper("Resolution", GuiTab::Stream,
            [&cfg]() { return fmt::format("{}p", cfg.resolution.getHeight()); },
            [&cfg]() {
                if (cfg.resolution.getIndex() + 1 < CameraResolution::count())
                    cfg.resolution = CameraResolution::fromIndex(cfg.resolution.getIndex() + 1);
            },
            [&cfg]() {
                if (cfg.resolution.getIndex() > 0) cfg.resolution = CameraResolution::fromIndex(cfg.resolution.getIndex() - 1);
            })
        .isPending = pending([&cfg](const StreamingConfig &a) { return a.resolution.getIndex() != cfg.resolution.getIndex(); });
    stepper("Frame rate", GuiTab::Stream,
            [&cfg]() { return fmt::format("{} fps", cfg.fps); },
            [&cfg]() { cfg.fps = nextRate(cfg.fps, +1); },
            [&cfg]() { cfg.fps = nextRate(cfg.fps, -1); })
        .isPending = pending([&cfg](const StreamingConfig &a) { return a.fps != cfg.fps; });
    {
        auto &s = stepper("Bitrate", GuiTab::Stream,
                          [&cfg]() { return fmt::format("{} Mbit/s", cfg.bitrate / 1000000); },
                          [&cfg]() { cfg.bitrate = stepBitrate(cfg.bitrate, +1); },
                          [&cfg]() { cfg.bitrate = stepBitrate(cfg.bitrate, -1); });
        s.isVisible = [&cfg]() { return cfg.codec != Codec::JPEG; };
        s.isPending = pending([&cfg](const StreamingConfig &a) { return a.bitrate != cfg.bitrate; });
    }
    {
        auto &s = stepper("JPEG quality", GuiTab::Stream,
                          [&cfg]() { return fmt::format("{}", cfg.encodingQuality); },
                          [&cfg]() { cfg.encodingQuality = std::min(100, cfg.encodingQuality / 5 * 5 + 5); },
                          [&cfg]() { cfg.encodingQuality = std::max(5, (cfg.encodingQuality + 4) / 5 * 5 - 5); });
        s.isVisible = [&cfg]() { return cfg.codec == Codec::JPEG; };
        s.isPending = pending([&cfg](const StreamingConfig &a) { return a.encodingQuality != cfg.encodingQuality; });
    }
    {
        GuiSetting s;
        s.label = "Apply"; s.control = GuiControl::Button; s.tab = GuiTab::Stream;
        // With nothing to apply and no video arriving (the robot did not start, or the decoder stalled) the button
        // restarts the stream instead
        const auto restartable = [&cfg, applied, appState]() {
            return applied() && PendingStreamChanges(cfg, applied()) == 0 && !appState->statusBar.video;
        };
        s.getValue = [&cfg, applied, restartable]() {
            const int n = PendingStreamChanges(cfg, applied());
            if (!applied()) return std::string("Start stream");
            if (restartable()) return std::string("Restart stream");
            return n == 0 ? std::string("Applied") : fmt::format("Apply {} change{}", n, n == 1 ? "" : "s");
        };
        s.isEnabled = [&cfg, applied, restartable]() { return PendingStreamChanges(cfg, applied()) > 0 || restartable(); };
        s.onActivate = [apply = actions.applyStream, restart = actions.restartStream, restartable]() {
            if (restartable()) restart();
            else apply();
        };
        add(std::move(s));
    }

    // --- Camera head and view: take effect at once ---
    stepper("Sensitivity", GuiTab::Camera,
            [appState]() { return fmt::format("{:.2f}×", appState->headMovementSensitivity); },
            // 0.25 to 5.00: steps of 0.05 up to 2.00, of 0.25 above it
            [appState]() { float &s = appState->headMovementSensitivity;
                           s = std::min(5.0f, s < 1.999f ? std::round(s * 20 + 1) / 20 : std::round(s * 4 + 1) / 4); },
            [appState]() { float &s = appState->headMovementSensitivity;
                           s = std::max(0.25f, s > 2.001f ? std::round(s * 4 - 1) / 4 : std::round(s * 20 - 1) / 20); });
    stepper("Prediction", GuiTab::Camera,
            [appState]() { return fmt::format("{} ms", appState->headMovementPredictionMs); },
            [appState]() { auto &p = appState->headMovementPredictionMs; p = std::min<uint32_t>(100, p / 5 * 5 + 5); },
            [appState]() { auto &p = appState->headMovementPredictionMs; p = p <= 5 ? 0 : (p + 4) / 5 * 5 - 5; });
    stepper("Max speed", GuiTab::Camera,
            [appState]() { return fmt::format("{:.0f}°/s", appState->headMaxSpeedDegS); },
            [appState]() { appState->headMaxSpeedDegS = std::min(1800.0f, appState->headMaxSpeedDegS + 30.0f); },
            [appState]() { appState->headMaxSpeedDegS = std::max(30.0f, appState->headMaxSpeedDegS - 30.0f); });
    toggle("Drive robot base", GuiTab::Camera,
           [appState]() { return appState->robotControlEnabled; },
           [set = actions.setRobotDrive](bool on) { set(on); });
    choice("Video view", GuiTab::Camera, {"Full FOV", "Full screen"},
           [appState]() { return indexOf(kViews, appState->aspectRatioMode); },
           [appState](int i) { appState->aspectRatioMode = kViews[i]; });
    choice("Panel", GuiTab::Camera, {"In front", "Below video"},
           [appState]() { return static_cast<int>(appState->guiPanel.layout); },
           [set = actions.setPanelLayout](int i) { set(static_cast<GuiPanelLayout>(i)); });
    stepper("Convergence", GuiTab::Camera,
            [appState]() { return fmt::format("{:+.2f}", appState->stereoConvergence); },
            [appState]() { if (appState->stereoConvergence < 0.495f) appState->stereoConvergence += 0.01f; },
            [appState]() { if (appState->stereoConvergence > -0.495f) appState->stereoConvergence -= 0.01f; });
    {
        // A drive that latched a fault ignores the head until reset: offered while the robot reports one it can clear
        GuiSetting s;
        s.label = "Reset camera head"; s.control = GuiControl::Button; s.tab = GuiTab::Camera;
        s.onActivate = actions.resetHeadDrives;
        s.isVisible = [appState]() {
            const auto &servo = appState->statusBar.servo;
            return servo.known && servo.canReset && servo.fault;
        };
        add(std::move(s));
    }

    // --- Audio ---
    toggle("Robot sound", GuiTab::Audio,
           [appState]() { return appState->audioRobotEnable; },
           [appState, apply = actions.applyAudio](bool on) { appState->audioRobotEnable = on; apply(); });
    stepper("Volume", GuiTab::Audio,
            [appState]() { return fmt::format("{} %", appState->audioVolume); },
            [appState, set = actions.setVolume]() { appState->audioVolume = std::min(100, appState->audioVolume + 5); set(appState->audioVolume); },
            [appState, set = actions.setVolume]() { appState->audioVolume = std::max(0, appState->audioVolume - 5); set(appState->audioVolume); });
    toggle("Microphone", GuiTab::Audio,
           [appState]() { return appState->audioMicEnable; },
           [appState, apply = actions.applyAudio](bool on) { appState->audioMicEnable = on; apply(); });
    choice("Mic mode", GuiTab::Audio, {"Open", "Push to talk"},
           [appState]() { return appState->micPushToTalk ? 1 : 0; },
           [appState](int i) { appState->micPushToTalk = i == 1; });
    toggle("Mute mic", GuiTab::Audio,
           [appState]() { return appState->micMuted; },
           [appState](bool on) { appState->micMuted = on; });

    // --- System ---
    {
        GuiSetting s;
        s.label = "Robot IP"; s.control = GuiControl::IpAddress; s.tab = GuiTab::System; s.segments = 4;
        s.getValue = [&cfg]() { return IpToString(cfg.jetson_ip); };
        s.onIncrement = [appState]() { appState->streamingConfig.jetson_ip[appState->guiControl.focusedSegment] += 1; };
        s.onDecrement = [appState]() { appState->streamingConfig.jetson_ip[appState->guiControl.focusedSegment] -= 1; };
        s.isPending = pending([&cfg](const StreamingConfig &a) { return a.jetson_ip != cfg.jetson_ip; });
        add(std::move(s));
    }
    toggle("Keep running off-head", GuiTab::System,
           [appState]() { return appState->keepRunningOffHead; },
           [set = actions.setKeepRunningOffHead](bool on) { set(on); });
    {
        GuiSetting s;
        s.label = "Headset IP"; s.control = GuiControl::Readout; s.tab = GuiTab::System;
        s.getValue = [&cfg]() { return IpToString(cfg.headset_ip); };
        add(std::move(s));
    }
    {
        GuiSetting s;
        s.label = "Clock sync"; s.control = GuiControl::Readout; s.tab = GuiTab::System;
        s.getValue = [appState]() { return appState->ntpSyncStatus; };
        add(std::move(s));
    }
    return rows;
}
