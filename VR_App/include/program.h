/**
 * program.h - Main VR application class
 *
 * TelepresenceProgram owns the entire VR session lifecycle: OpenXR setup,
 * GStreamer video pipelines, network communication, and per-frame rendering.
 * It is instantiated in main.cpp when the Android activity starts and destroyed
 * when the activity is torn down.
 */
#pragma once

#include "util_openxr.h"
#include "util_egl.h"
#include "BS_thread_pool.hpp"
#include "robot_control_sender.h"
#include "gstreamer_player.h"
#include "rest_client.h"
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

    /** Process VR controller input for GUI navigation and robot control. */
    void HandleControllers();

    /** Populate the data-driven settings_ table with GUI entries. */
    void BuildSettings();

    /* --- OpenXR handles --- */
    XrInstance openxr_instance_ = XR_NULL_HANDLE;
    XrSystemId openxr_system_id_ = XR_NULL_SYSTEM_ID;
    XrSession openxr_session_ = XR_NULL_HANDLE;

    std::vector<viewsurface_t> viewsurfaces_;

    std::vector<XrSpace> reference_spaces_;
    XrSpace app_reference_space_;

    /* --- Input state --- */
    InputState input_;
    UserState userState_;

    /* --- Runtime flags --- */
    bool mono_ = false;                  /* true when VideoMode is Mono */
    bool renderGui_ = true;              /* toggle with left thumbstick press */
    bool controlLockMovement_ = false;   /* debounce lock for right thumbstick press */
    bool controlLockGui_ = false;        /* debounce lock for left thumbstick press */

    /* --- Thread pools --- */
    BS::thread_pool<BS::tp::none> gstreamerThreadPool_{1};  /* GStreamer pipeline ops */
    BS::thread_pool<BS::tp::none> threadPool_{3};           /* async network ops */

    /* --- Subsystem modules --- */
    std::unique_ptr<GstreamerPlayer> gstreamerPlayer_;
    std::unique_ptr<RestClient> restClient_;
    std::unique_ptr<NtpTimer> ntpTimer_;
    std::unique_ptr<RosNetworkGatewayClient> rosNetworkGatewayClient_;
    std::unique_ptr<RobotControlSender> robotControlSender_;
    std::unique_ptr<StateStorage> stateStorage_;

    /* --- Frame timing --- */
    std::chrono::time_point<std::chrono::high_resolution_clock> prevFrameStart_, frameStart_;

    /* --- Shared application state --- */
    std::shared_ptr<AppState> appState_{};

    /* --- Data-driven GUI settings table --- */
    std::vector<GuiSetting> settings_;
};