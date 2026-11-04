/**
 * program.h - Main VR application class
 *
 * TelepresenceProgram owns the entire VR session lifecycle: OpenXR setup,
 * GStreamer video pipelines, network communication, and per-frame rendering.
 * It is instantiated in main.cpp when the Android activity starts and destroyed
 * when the activity is torn down.
 */
#pragma once

#include <optional>

#include "util_openxr.h"
#include "util_egl.h"
#include "BS_thread_pool.hpp"
#include "robot_control_sender.h"
#include "video_player.h"
#include "audio_player.h"
#include "rest_client.h"
#include "adaptation_controller.h"
#include "ntp_timer.h"
#include "state_storage.h"
#include "ros_network_gateway_client.h"
#include "types/gui_setting.h"

/**
 * Core application class managing the VR telepresence session.
 *
 * Lifecycle:
 *   1. Constructor initializes OpenXR, EGL, GStreamer, NTP, and networking
 *   2. UpdateFrame() is called every frame from the Android main loop
 *   3. Destructor stops the camera stream and cleans up resources
 *
 * Threading model:
 *   - Main thread: OpenXR, rendering, and input polling
 *   - gstreamerThreadPool_ (1 thread): GStreamer pipeline management
 *   - threadPool_ (3 threads): async network operations (NTP, UDP sends)
 */
class TelepresenceProgram {

public:
    TelepresenceProgram(struct android_app *app);

    ~TelepresenceProgram();

    /** Per-frame entry point called from the Android main loop. */
    void UpdateFrame();

private:
    /** Set up OpenXR input actions and controller bindings. */
    void InitializeActions();

    /** Read current controller and button state from OpenXR. */
    void PollActions();

    /** Retrieve HMD and controller poses for the predicted display time. */
    void PollPoses(XrTime predictedDisplayTime);

    /** Begin an OpenXR frame, render, and submit it. */
    void RenderFrame();

    /** Render a single stereo layer (both eye views). */
    bool RenderLayer(XrTime displayTime, std::vector<XrCompositionLayerProjectionView> &layerViews,
                     XrCompositionLayerProjection &layer);

    /** Send head pose and robot control data over UDP. */
    void SendControllerDatagram();

    /** Start the camera stream via REST API and configure GStreamer pipelines. */
    void InitializeStreaming();
    /** Apply a streaming config to both ends (panel Apply and the adaptation controller); true if the robot acknowledged it. */
    bool ApplyStreamingConfig(StreamingConfig cfg, bool rebuild = false);

    /** Reconcile the optional audio bridge (robot + headset pipelines) with the
     *  current audioRobotEnable / audioMicEnable flags. Safe no-op when both off. */
    void ApplyAudioState();

    /** Process VR controller input for GUI navigation and robot control. */
    void HandleControllers();
    void PlacePanel();
    bool RenderPanelLayer(XrCompositionLayerQuad &layer, XrCompositionLayerSettingsFB &settings);
    void RenderStatusLayer(XrCompositionLayerQuad &layer, XrCompositionLayerSettingsFB &settings);
    void UpdateStatusBar();
    void CheckReceiveWatchdog();
    void SendSystemBroadcast(const char *action);
    void FacePanelToHead();

    /** Populate the data-driven settings_ table with GUI entries. */
    void BuildSettings();

    /* --- OpenXR handles --- */
    XrInstance openxr_instance_ = XR_NULL_HANDLE;
    XrSystemId openxr_system_id_ = XR_NULL_SYSTEM_ID;
    XrSession openxr_session_ = XR_NULL_HANDLE;

    std::vector<viewsurface_t> viewsurfaces_;
    viewsurface_t panelSurfaces_[2]{};   // the settings panel's quad layer, one per GuiPanelLayout
    viewsurface_t statusSurface_{};      // the status bar's quad layer
    double statusBarUpdatedS_ = 0.0;     // UpdateStatusBar: last refresh, and the loss counter a second ago
    double lossCountedS_ = 0.0;
    uint32_t lossCount_ = 0;
    uint64_t lastRebuildUs_ = 0;         // last structural rebuild of the decode pipeline (receive watchdog)
    uint64_t watchdogCheckedUs_ = 0;
    uint32_t watchdogLost_[2]{};          // each eye's jitter-buffer loss count, sampled once a second
    uint64_t watchdogLostUs_[2]{};
    uint64_t watchdogLastLossUs_[2]{};    // when a lost packet was last seen on each eye
    uint32_t watchdogStreak_ = 0;         // watchdog rebuilds in a row that brought no frame (backoff)
    int watchdogEye_ = -1;                // the eye whose stall caused the last watchdog rebuild

    std::vector<XrSpace> reference_spaces_;
    XrSpace app_reference_space_;

    /* --- Input state --- */
    InputState input_;
    UserState userState_;
    bool leadNow_ = false;  // debug.but.lead_now: camera-head command = present-instant pose (Chapter 8 reference)

    /* --- Runtime flags --- */
    bool renderGui_ = true;              /* toggle with left thumbstick press */
    bool panelPlaced_ = false;           /* the panel has been placed for its layout at least once */
    bool controlLockMovement_ = false;   /* debounce lock for right thumbstick press */
    bool controlLockGui_ = false;        /* debounce lock for left thumbstick press */

    /* --- Thread pools --- */
    BS::thread_pool<BS::tp::none> gstreamerThreadPool_{1};  /* GStreamer pipeline ops */
    BS::thread_pool<BS::tp::none> threadPool_{3};           /* async network ops */
    BS::thread_pool<BS::tp::none> audioThreadPool_{1};      /* audio reconfig */

    /* --- Subsystem modules --- */
    std::unique_ptr<VideoPlayer> videoPlayer_;
    std::unique_ptr<AudioPlayer> audioPlayer_;
    std::unique_ptr<RestClient> restClient_;
    std::unique_ptr<NtpTimer> ntpTimer_;
    std::unique_ptr<RosNetworkGatewayClient> rosNetworkGatewayClient_;
    std::unique_ptr<RobotControlSender> robotControlSender_;
    std::unique_ptr<StateStorage> stateStorage_;
    struct android_app *app_{nullptr};
    std::unique_ptr<AdaptationController> adaptation_;   /* Chapter 9 controller, debug.but.adapt */

    /* --- Frame timing --- */
    std::chrono::time_point<std::chrono::high_resolution_clock> prevFrameStart_, frameStart_;

    /* Throttle for the low-rate NTP-metrics telemetry packet (~1 Hz). */
    uint32_t ntpMetricsThrottle_ = 0;

    /* --- Shared application state --- */
    std::shared_ptr<AppState> appState_{};

    /* --- Last streaming config successfully pushed to the robot. Used by the
     *     Apply handler to decide between a full decode/render rebuild
     *     (structural change) and a fast live encoder update (bitrate/quality). */
    std::optional<StreamingConfig> lastAppliedConfig_{};

    /* --- Data-driven GUI settings table --- */
    std::vector<GuiSetting> settings_;
};