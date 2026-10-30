/**
 * program.cpp - TelepresenceProgram implementation
 *
 * Contains the full VR application lifecycle: initialization, per-frame
 * update/render loop, controller input handling, and the actions behind the
 * settings panel (rows in settings_table.cpp). See program.h for the class overview.
 */

#include "pch.h"
#include "log.h"
#include "check.h"
#include "render_scene.h"
#include "render_imgui.h"
#include "settings_table.h"

#include <utility>
#include <algorithm>
#include <ctime>
#include <sys/system_properties.h>
#include <GLES3/gl32.h>

#include "program.h"
#include "utils/network_utils.h"
#include "xr_timing.h"

#define HANDL_IN "/user/hand/left/input"
#define HANDR_IN "/user/hand/right/input"

/**
 * Initialize the VR application.
 *
 * Startup sequence:
 *   1. OpenXR loader + instance + system
 *   2. EGL context + graphics requirements confirmation
 *   3. Load persisted app state from SharedPreferences
 *   4. Scene (shaders, geometry, textures)
 *   5. OpenXR session + reference spaces + swapchains
 *   6. NTP time sync, GStreamer player, ROS gateway client
 *   7. Collect system info (runtime, GPU)
 *   8. Input actions, streaming, GUI settings table
 */
TelepresenceProgram::TelepresenceProgram(struct android_app *app) : app_(app) {

    /* Initialize the OpenXR loader which detects and interfaces with the XR runtime */
    openxr_init_loader(app);

    openxr_create_instance(app, &openxr_instance_);

    openxr_get_system_id(&openxr_instance_, &openxr_system_id_);

    egl_init_with_pbuffer_surface();
    openxr_confirm_gfx_reqs(&openxr_instance_, &openxr_system_id_);

    stateStorage_ = std::make_unique<StateStorage>(app);

    appState_ = std::make_shared<AppState>();
    stateStorage_->LoadAppState(*appState_);
    appState_->streamingConfig.headset_ip = GetLocalIPAddr();

    init_scene(appState_->streamingConfig.resolution.getWidth(), appState_->streamingConfig.resolution.getHeight());

    openxr_create_session(&openxr_instance_, &openxr_system_id_, &openxr_session_);
    openxr_log_reference_spaces(&openxr_session_);
    openxr_create_reference_spaces(&openxr_session_, reference_spaces_);
    app_reference_space_ = reference_spaces_[0]; // "ViewFront"

    viewsurfaces_ = openxr_create_swapchains(&openxr_instance_, &openxr_system_id_, &openxr_session_);
    for (auto layout : {GuiPanelLayout::Front, GuiPanelLayout::Below}) {
        const GuiPanelGeometry g = GuiPanelState::geometryOf(layout);
        panelSurfaces_[(int) layout] = openxr_create_panel_swapchain(
                &openxr_session_, (uint32_t) std::lround(g.width * g.renderScale),
                (uint32_t) std::lround(g.height * g.renderScale));
    }
    {
        const GuiPanelGeometry g = StatusBarState::GEOMETRY;
        statusSurface_ = openxr_create_panel_swapchain(&openxr_session_, (uint32_t) std::lround(g.width * g.renderScale),
                                                       (uint32_t) std::lround(g.height * g.renderScale));
    }

    ntpTimer_ = std::make_unique<NtpTimer>(IpToString(appState_->streamingConfig.jetson_ip), "195.113.144.201");
    ntpTimer_->StartAutoSync();
    videoPlayer_ = std::make_unique<VideoPlayer>(&appState_->cameraStreamingStates, ntpTimer_.get());
    audioPlayer_ = std::make_unique<AudioPlayer>(ntpTimer_.get());
    rosNetworkGatewayClient_ = std::make_unique<RosNetworkGatewayClient>();

    appState_->systemInfo.openXrRuntime = openxr_get_runtime_name(&openxr_instance_);
    appState_->systemInfo.openXrSystem = openxr_get_system_name(&openxr_instance_, &openxr_system_id_);
    appState_->systemInfo.openGlVersion = glGetString(GL_VERSION);
    appState_->systemInfo.openGlVendor = glGetString(GL_VENDOR);
    appState_->systemInfo.openGlRenderer = glGetString(GL_RENDERER);

    InitializeActions();
    InitializeStreaming();
    BuildSettings();

    adaptation_ = std::make_unique<AdaptationController>(appState_, ntpTimer_.get());
    if (lastAppliedConfig_) adaptation_->onApplied(*lastAppliedConfig_);
    adaptation_->start();
}

TelepresenceProgram::~TelepresenceProgram() {
    if (appState_ && appState_->keepRunningOffHead) {
        SendSystemBroadcast("com.oculus.vrpowermanager.automation_disable");   // hand the proximity sensor back
    }
    if (adaptation_) adaptation_->stop();
    if (audioPlayer_) {
        audioPlayer_->stopSend();
        audioPlayer_->stopReceive();
    }
    if (restClient_) {
        restClient_->StopAudio();
    }
    if (restClient_ && appState_->connectionState.cameraServer == ConnectionStatus::Connected) {
        LOG_INFO("TelepresenceProgram: Stopping camera stream...");
        restClient_->StopStream();
    }
}

/**
 * Per-frame update: poll OpenXR events, update connection status,
 * read controller input, send control datagrams, and render.
 */
void TelepresenceProgram::UpdateFrame() {
    bool exit, request_restart;
    openxr_poll_events(&openxr_instance_, &openxr_session_, &exit, &request_restart, &appState_->headsetMounted);

    if (!openxr_is_session_running()) {
        return;
    }

    /* Update NTP sync status for HUD display */
    if (ntpTimer_) {
        if (ntpTimer_->IsSyncStale()) {
            appState_->connectionState.ntpSync = ConnectionStatus::Failed;
            appState_->ntpSyncStatus = "Stale (" +
                std::to_string(ntpTimer_->GetTimeSinceLastSyncUs() / 1'000'000) + " s)";
        } else if (ntpTimer_->IsSyncHealthy()) {
            appState_->connectionState.ntpSync = ConnectionStatus::Connected;
            appState_->ntpSyncStatus = "Synced";
        } else if (ntpTimer_->GetConsecutiveFailures() > 0) {
            appState_->connectionState.ntpSync = ConnectionStatus::Failed;
            appState_->ntpSyncStatus = "Not Synced";
        }
    }

    PollActions();

    // Drive the mic gate: push-to-talk transmits only while the LEFT grip is held;
    // otherwise honour the open-mic mute toggle. No-op unless the mic leg is active.
    if (audioPlayer_ && audioPlayer_->sending()) {
        const bool muted = appState_->micPushToTalk
            ? (userState_.squeezeValue[Side::LEFT] < 0.5f)
            : appState_->micMuted;
        audioPlayer_->setMuted(muted);
    }
    SendControllerDatagram();

    // Apply a change the adaptation controller asked for, on this (render) thread like the panel's Apply.
    if (adaptation_) {
        if (auto req = adaptation_->takeRequest()) {
            const bool ok = ApplyStreamingConfig(req->second);
            if (ok) appState_->streamingConfig = req->second;   // the panel shows what is in force
            adaptation_->reportResult(req->first, ok);
        }
    }

    RenderFrame();
}

/**
 * Begin an OpenXR frame, render stereo layers, end the frame, and measure timing.
 */
void TelepresenceProgram::RenderFrame() {
    prevFrameStart_ = frameStart_;
    frameStart_ = std::chrono::high_resolution_clock::now();

    XrTime display_time;
    openxr_begin_frame(&openxr_session_, &display_time);

    PollPoses(display_time);

    std::vector<XrCompositionLayerBaseHeader *> layers;
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::vector<XrCompositionLayerProjectionView> projectionLayerViews;
    if (RenderLayer(display_time, projectionLayerViews, layer)) {
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&layer));
    }
    // The status bar over the video, always shown
    UpdateStatusBar();
    XrCompositionLayerQuad statusLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerSettingsFB statusSettings{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
    RenderStatusLayer(statusLayer, statusSettings);
    layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&statusLayer));
    // The settings panel as a quad layer over the scene: the compositor samples it once, straight to the display
    XrCompositionLayerQuad panelLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerSettingsFB panelSettings{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
    if (renderGui_ && RenderPanelLayer(panelLayer, panelSettings)) {
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&panelLayer));
    }

    openxr_end_frame(&openxr_session_, &display_time, layers);
    auto end = std::chrono::high_resolution_clock::now();
    appState_->appFrameTime = std::chrono::duration_cast<std::chrono::microseconds>(
            end - frameStart_).count();
    auto frameDuration = std::chrono::duration_cast<std::chrono::microseconds>(
            frameStart_ - prevFrameStart_).count();
    appState_->appFrameRate = (frameDuration > 0) ? (1e6f / frameDuration) : 0.0f;
}

/**
 * Draw the settings panel into its swapchain and describe its quad layer: the panel's pose and size in app space.
 */
bool TelepresenceProgram::RenderPanelLayer(XrCompositionLayerQuad &layer, XrCompositionLayerSettingsFB &settings) {
    const auto &panel = appState_->guiPanel;
    viewsurface_t &surface = panelSurfaces_[(int) panel.layout];
    render_target_t target{};
    XrSwapchainSubImage subImage{};
    openxr_acquire_viewsurface(surface, target, subImage);
    draw_settings_panel(target, surface.format == GL_SRGB8_ALPHA8, appState_, settings_);
    openxr_release_viewsurface(surface);

    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;   // premultiplied: rounded corners
    layer.space = app_reference_space_;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage = subImage;
    layer.pose.orientation = panel.orientation();
    layer.pose.position = panel.position;
    layer.size = {panel.getWorldWidth(), panel.height};
    if (openxr_has_layer_settings()) {
        settings.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB;   // filter text well when minified
        layer.next = &settings;
    }
    return true;
}

/**
 * Draw the status bar into its swapchain and describe its quad layer: head-locked, centred ELEVATION_DEG above the
 * eyes at the video's distance, tilted to face them.
 */
void TelepresenceProgram::RenderStatusLayer(XrCompositionLayerQuad &layer, XrCompositionLayerSettingsFB &settings) {
    render_target_t target{};
    XrSwapchainSubImage subImage{};
    openxr_acquire_viewsurface(statusSurface_, target, subImage);
    draw_status_bar(target, statusSurface_.format == GL_SRGB8_ALPHA8, appState_);
    openxr_release_viewsurface(statusSurface_);

    constexpr float UP = StatusBarState::ELEVATION_DEG * (float) M_PI / 180.0f;
    constexpr float TALL = StatusBarState::HEIGHT_DEG * (float) M_PI / 180.0f;
    const GuiPanelGeometry g = StatusBarState::GEOMETRY;
    const XrVector3f &head = userState_.headInApp.position;
    const float d = StatusBarState::DISTANCE_M, height = 2.0f * d * std::tan(TALL * 0.5f);
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.space = app_reference_space_;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage = subImage;
    layer.pose.orientation = {std::sin(UP * 0.5f), 0.0f, 0.0f, std::cos(UP * 0.5f)};   // tilted down to the eyes
    layer.pose.position = {head.x, head.y + d * std::sin(UP), head.z - d * std::cos(UP)};
    layer.size = {height * (float) g.width / (float) g.height, height};
    if (openxr_has_layer_settings()) {
        settings.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB;
        layer.next = &settings;
    }
}

/**
 * Refresh what the status bar shows: the received frame rate and whether frames still arrive, the latency figure,
 * packet loss over the last second, and the drives' state the relay reports.
 */
void TelepresenceProgram::UpdateStatusBar() {
    auto &bar = appState_->statusBar;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (robotControlSender_) robotControlSender_->pollServoStatus(bar.servo, now);
    bar.relaySilent = bar.servo.known && now - bar.servo.receivedAtS > 3.0;
    if (now - statusBarUpdatedS_ < 0.25) return;
    statusBarUpdatedS_ = now;

    CameraStats *left = appState_->cameraStreamingStates.first.stats;
    CameraStats *right = appState_->cameraStreamingStates.second.stats;
    if (!left) {
        bar.video = false;
        return;
    }
    const CameraWindowSummary w = left->windowSummary();
    bar.video = w.frames > 1 && (double) ntpTimer_->GetCurrentTimeUs() - w.newestUs < 1e6;
    bar.fps = w.fps;
    const CameraStatsSnapshot s = left->averagedSnapshot();
    bar.latencyMs = (s.camera + s.vidConv + s.enc + s.rtpPay + s.udpStream + s.jbHold + s.rtpDepay + s.dec + s.appsink +
                     s.presentation) / 1000.0;
    const uint32_t lost = left->jbNumLost.load() + (right ? right->jbNumLost.load() : 0);
    if (now - lossCountedS_ >= 1.0) {
        bar.lossPerS = lost >= lossCount_ && lossCountedS_ > 0 ? (lost - lossCount_) / (now - lossCountedS_) : 0.0;
        lossCount_ = lost;   // the counters restart on a rebuild
        lossCountedS_ = now;
    }
}

/**
 * Render both eye views into their swapchain images.
 *
 * For each eye: acquires a swapchain image, renders the camera image plane
 * and ImGui overlay, then releases the image. View matrices are rendered at
 * the OpenXR runtime's predicted display time so time warp reprojects cleanly.
 * A separately over-predicted HMD pose (displayTime + headMovementPredictionMs)
 * is queried only for the robot pan-tilt command, to compensate for uplink
 * and servo delay.
 */
bool TelepresenceProgram::RenderLayer(XrTime displayTime,
                                      std::vector<XrCompositionLayerProjectionView> &layerViews,
                                      XrCompositionLayerProjection &layer) {
    auto viewCount = viewsurfaces_.size();
    std::vector<XrView> views(viewCount, {XR_TYPE_VIEW});
    // When the runtime reports the view poses as not valid for this display time, their
    // contents are undefined; rendering with them puts the video plane nowhere and the eye
    // shows only the clear colour for that refresh. Reuse the last valid views instead.
    static std::vector<XrView> lastValidViews;
    if (openxr_locate_views(&openxr_session_, &displayTime, app_reference_space_, viewCount,
                            views.data()) == 0) {
        lastValidViews = views;
        if (viewCount >= 2) {
            const auto &a = views[0].pose.position, &b = views[1].pose.position;
            userState_.headInApp = {views[0].pose.orientation, {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f}};
            if (!panelPlaced_) {   // the panel starts open: place it once the head is known
                PlacePanel();
                panelPlaced_ = true;
            }
        }
    } else {
        if (lastValidViews.size() == viewCount) views = lastValidViews;
    }

    layerViews.resize(viewCount);

    XrSpaceLocation spaceLocation{XR_TYPE_SPACE_LOCATION};

    // Query the HMD pose at the over-predicted time for the robot-side pan-tilt command.
    // This is intentionally ahead of the OpenXR predicted display time to compensate for
    // the uplink and servo delay on the robot. The rendered view matrices above use the
    // unmodified predictedDisplayTime so OpenXR time warp behaves canonically.
    // The head pose at the present instant, with no forecast: the reference for measuring how far the camera
    // head lags the operator's head. On Quest XrTime is CLOCK_MONOTONIC (ns), so 'now' needs no time
    // conversion; the sample time is taken on the NTP-corrected robot clock the packet already uses.
    timespec tsNow{};
    clock_gettime(CLOCK_MONOTONIC, &tsNow);
    const XrTime nowTime = (XrTime) tsNow.tv_sec * 1000000000LL + tsNow.tv_nsec;
    const uint64_t nowNtpUs = ntpTimer_->GetCurrentTimeUs();
    {
        XrSpaceLocation nowLocation{XR_TYPE_SPACE_LOCATION};
        if (XR_UNQUALIFIED_SUCCESS(xrLocateSpace(reference_spaces_[1], app_reference_space_, nowTime, &nowLocation)) &&
            (nowLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
            userState_.hmdPoseNow = nowLocation.pose;
            userState_.hmdPoseNowTimeUs = nowNtpUs;
        }
    }

    // The head at this frame's predicted display time, the pose the operator sees it from, with its angular velocity
    // from the same query; the display time is carried over to the robot clock through the present instant. The head
    // is the app's head-locked space located in the world (Local), so both are in the world frame, +Y up.
    {
        XrSpaceVelocity displayVelocity{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation displayLocation{XR_TYPE_SPACE_LOCATION, &displayVelocity};
        uint8_t valid = 0;
        if (XR_UNQUALIFIED_SUCCESS(xrLocateSpace(app_reference_space_, reference_spaces_[1], displayTime, &displayLocation))) {
            if ((displayLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
                userState_.hmdPoseDisplay = displayLocation.pose;
                valid |= 1;
            }
            if ((displayVelocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0) {
                userState_.hmdAngularVelocityDisplay = displayVelocity.angularVelocity;
                valid |= 2;
            }
        }
        userState_.hmdDisplayValid = valid;
        userState_.hmdDisplayTimeUs = (uint64_t) ((int64_t) nowNtpUs + (displayTime - nowTime) / 1000);
    }

    // Measurement mode for the Chapter 8 reference with no forward prediction: with
    // `adb shell setprop debug.but.lead_now 1` (checked every 90 frames) the camera-head command is the pose at
    // the present instant instead of display time + lead, and the packet reports the lead as PREDICTION_NOW.
    {
        static int leadNowCheck = 0;
        if ((leadNowCheck++ % 90) == 0) {
            char v[PROP_VALUE_MAX] = {0};
            __system_property_get("debug.but.lead_now", v);
            leadNow_ = (v[0] == '1');
        }
    }
    XrTime robotTargetTime = leadNow_ ? nowTime
                                      : displayTime + (XrTime) (appState_->headMovementPredictionMs * 1e6);
    auto res = xrLocateSpace(reference_spaces_[1], app_reference_space_, robotTargetTime,
                             &spaceLocation);
    CHECK_XRRESULT(res, "xrLocateSpace")
    if (XR_UNQUALIFIED_SUCCESS(res)) {
        if ((spaceLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
            (spaceLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {

            userState_.hmdPose = spaceLocation.pose;
        }
    } else {
        LOG_INFO("Unable to locate a visualized reference space in app space: %d", res);
    }

    Quad quad{};
    quad.Pose.position = {0.0f, 0.0f, 0.0f};
    quad.Pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    if (appState_->aspectRatioMode == AspectRatioMode::FullFOV) {
        quad.Scale = {3.56f, 3.56f / appState_->streamingConfig.resolution.getAspectRatio(), 0.0f};
    } else {
        quad.Scale = {3.56f * appState_->streamingConfig.resolution.getAspectRatio(), 3.56f, 0.0f};
    }

    for (uint32_t i = 0; i < viewCount; i++) {
        XrSwapchainSubImage subImg;
        render_target_t rtarget;

        openxr_acquire_viewsurface(viewsurfaces_[i], rtarget, subImg);

        layerViews[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        layerViews[i].pose = views[i].pose;
        layerViews[i].fov = views[i].fov;
        layerViews[i].subImage = subImg;

        // View 0 is the left lens: it shows the left stream (port 8554), view 1 the right one.
        CameraFrame *imageHandle = i == 0 ? &appState_->cameraStreamingStates.first
                                          : &appState_->cameraStreamingStates.second;

        HandleControllers();

        const auto vm = appState_->streamingConfig.videoMode;
        if (vm == VideoMode::Mono || vm == VideoMode::Panoramic) {
            imageHandle = &appState_->cameraStreamingStates.first;
        }

        // Stereo convergence (horizontal image translation): shift the two eyes'
        // image planes horizontally in opposite directions to set convergence/comfort.
        // Headset-only; only meaningful in stereo (mono/panoramic show one image to
        // both eyes). 0 = no shift = unchanged behaviour.
        quad.Pose.position.x = (vm == VideoMode::Stereo)
            ? (i == 0 ? +0.5f : -0.5f) * appState_->stereoConvergence
            : 0.0f;

        // Measure presentation latency only on the first render after a NEW camera frame.
        // Without this guard, repeated renders of the same frame produce increasing
        // values (the frame ages), and updateHistory() always captures the worst case.
        //
        // "presentation" is defined as appsink -> predicted photon emission, i.e.
        // (wait-for-render-cycle)  +  (OpenXR's remaining-to-display prediction).
        // The first term is NTP-synced wall-clock microseconds; the second is
        // derived from CLOCK_MONOTONIC because XrTime == CLOCK_MONOTONIC ns on
        // Android/Quest. Both are durations, so adding them is clock-safe.
        {
            uint64_t frameReadyTime = imageHandle->stats->frameReadyTimestamp.load();
            uint64_t lastMeasured = imageHandle->stats->lastMeasuredFrameReady.load();
            if (frameReadyTime > 0 && frameReadyTime != lastMeasured) {
                uint64_t renderTime = ntpTimer_->GetCurrentTimeUs();
                uint64_t waitForRenderUs = renderTime - frameReadyTime;

                struct timespec tsNow{};
                clock_gettime(CLOCK_MONOTONIC, &tsNow);
                int64_t monotonicNowNs =
                    static_cast<int64_t>(tsNow.tv_sec) * 1'000'000'000LL + tsNow.tv_nsec;
                int64_t predictedDisplayNs =
                    XrTiming::predictedDisplayTimeXr.load(std::memory_order_relaxed);
                int64_t predictedRemainingUs =
                    (predictedDisplayNs - monotonicNowNs) / 1000;
                if (predictedRemainingUs < 0) predictedRemainingUs = 0;

                imageHandle->stats->presentation.store(
                    waitForRenderUs + static_cast<uint64_t>(predictedRemainingUs));
                imageHandle->stats->lastMeasuredFrameReady.store(frameReadyTime);
            }
        }

        render_scene(layerViews[i], rtarget, quad, appState_, imageHandle);

        openxr_release_viewsurface(viewsurfaces_[i]);
    }

    layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = app_reference_space_;
    layer.layerFlags = 0;
    layer.viewCount = layerViews.size();
    layer.views = layerViews.data();

    return true;
}

/**
 * Set up OpenXR input actions for all controller buttons, thumbsticks,
 * triggers, and grips. Binds to both the simple_controller and
 * oculus/touch_controller interaction profiles.
 */
void TelepresenceProgram::InitializeActions() {
    input_.actionSet = openxr_create_actionset(&openxr_instance_, "gameplay", "Gameplay", 0);

    CHECK_XRCMD(xrStringToPath(openxr_instance_, "/user/hand/right",
                               &input_.handSubactionPath[Side::RIGHT]))
    CHECK_XRCMD(xrStringToPath(openxr_instance_, "/user/hand/left",
                               &input_.handSubactionPath[Side::LEFT]))


    input_.quitAction = openxr_create_action(&input_.actionSet,
                                             XR_ACTION_TYPE_BOOLEAN_INPUT,
                                             "quit_session",
                                             "Quit Session",
                                             0,
                                             nullptr);

    input_.controllerPoseAction = openxr_create_action(&input_.actionSet,
                                                       XR_ACTION_TYPE_POSE_INPUT,
                                                       "controller_pose",
                                                       "Controller Pose",
                                                       Side::COUNT,
                                                       input_.handSubactionPath.data());

    input_.thumbstickPoseAction = openxr_create_action(&input_.actionSet,
                                                       XR_ACTION_TYPE_VECTOR2F_INPUT,
                                                       "thumbstick_pose",
                                                       "Thumbstick Pose",
                                                       Side::COUNT,
                                                       input_.handSubactionPath.data());

    input_.thumbstickPressedAction = openxr_create_action(&input_.actionSet,
                                                          XR_ACTION_TYPE_BOOLEAN_INPUT,
                                                          "thumbstick_pressed",
                                                          "Thumbstick Pressed",
                                                          Side::COUNT,
                                                          input_.handSubactionPath.data());

    input_.thumbstickTouchedAction = openxr_create_action(&input_.actionSet,
                                                          XR_ACTION_TYPE_BOOLEAN_INPUT,
                                                          "thumbstick_touched",
                                                          "Thumbstick Touched",
                                                          Side::COUNT,
                                                          input_.handSubactionPath.data());

    input_.buttonAPressedAction = openxr_create_action(&input_.actionSet,
                                                       XR_ACTION_TYPE_BOOLEAN_INPUT,
                                                       "button_a_pressed",
                                                       "Button A Pressed",
                                                       1,
                                                       &input_.handSubactionPath[Side::RIGHT]);

    input_.buttonATouchedAction = openxr_create_action(&input_.actionSet,
                                                       XR_ACTION_TYPE_BOOLEAN_INPUT,
                                                       "button_a_touched",
                                                       "Button A Touched",
                                                       1,
                                                       &input_.handSubactionPath[Side::RIGHT]);

    input_.buttonBPressedAction = openxr_create_action(&input_.actionSet,
                                                       XR_ACTION_TYPE_BOOLEAN_INPUT,
                                                       "button_b_pressed",
                                                       "Button B Pressed",
                                                       1,
                                                       &input_.handSubactionPath[Side::RIGHT]);

    input_.buttonBTouchedAction = openxr_create_action(&input_.actionSet,
                                                       XR_ACTION_TYPE_BOOLEAN_INPUT,
                                                       "button_b_touched",
                                                       "Button B Touched",
                                                       1,
                                                       &input_.handSubactionPath[Side::RIGHT]);

    input_.buttonXPressedAction = openxr_create_action(&input_.actionSet,
                                                       XR_ACTION_TYPE_BOOLEAN_INPUT,
                                                       "button_x_pressed",
                                                       "Button X Pressed",
                                                       1,
                                                       &input_.handSubactionPath[Side::LEFT]);

    input_.buttonXTouchedAction = openxr_create_action(&input_.actionSet,
                                                       XR_ACTION_TYPE_BOOLEAN_INPUT,
                                                       "button_x_touched",
                                                       "Button X Touched",
                                                       1,
                                                       &input_.handSubactionPath[Side::LEFT]);

    input_.buttonYPressedAction = openxr_create_action(&input_.actionSet,
                                                       XR_ACTION_TYPE_BOOLEAN_INPUT,
                                                       "button_y_pressed",
                                                       "Button Y Pressed",
                                                       1,
                                                       &input_.handSubactionPath[Side::LEFT]);

    input_.buttonYTouchedAction = openxr_create_action(&input_.actionSet,
                                                       XR_ACTION_TYPE_BOOLEAN_INPUT,
                                                       "button_y_touched",
                                                       "Button Y Touched",
                                                       1,
                                                       &input_.handSubactionPath[Side::LEFT]);

    input_.squeezeValueAction = openxr_create_action(&input_.actionSet,
                                                     XR_ACTION_TYPE_FLOAT_INPUT,
                                                     "squeeze_value",
                                                     "Squeeze Value",
                                                     Side::COUNT,
                                                     input_.handSubactionPath.data());

    input_.triggerValueAction = openxr_create_action(&input_.actionSet,
                                                     XR_ACTION_TYPE_FLOAT_INPUT,
                                                     "trigger_value",
                                                     "Trigger Value",
                                                     Side::COUNT,
                                                     input_.handSubactionPath.data());

    input_.triggerTouchedAction = openxr_create_action(&input_.actionSet,
                                                       XR_ACTION_TYPE_BOOLEAN_INPUT,
                                                       "trigger_touched",
                                                       "Trigger Touched",
                                                       Side::COUNT,
                                                       input_.handSubactionPath.data());

    std::vector<XrActionSuggestedBinding> bindings;
    bindings.push_back({input_.quitAction, openxr_string2path(&openxr_instance_, HANDL_IN"/menu/click")});
    bindings.push_back({input_.quitAction, openxr_string2path(&openxr_instance_, HANDR_IN"/menu/click")});
    openxr_bind_interaction(&openxr_instance_, "/interaction_profiles/khr/simple_controller", bindings);

    std::vector<XrActionSuggestedBinding> touch_bindings;
    touch_bindings.push_back({input_.quitAction, openxr_string2path(&openxr_instance_, HANDL_IN"/menu/click")});
    touch_bindings.push_back({input_.controllerPoseAction, openxr_string2path(&openxr_instance_, HANDL_IN"/aim/pose")});
    touch_bindings.push_back({input_.controllerPoseAction, openxr_string2path(&openxr_instance_, HANDR_IN"/aim/pose")});
    touch_bindings.push_back({input_.thumbstickPoseAction, openxr_string2path(&openxr_instance_, HANDL_IN"/thumbstick")});
    touch_bindings.push_back({input_.thumbstickPoseAction, openxr_string2path(&openxr_instance_, HANDR_IN"/thumbstick")});
    touch_bindings.push_back({input_.thumbstickPressedAction, openxr_string2path(&openxr_instance_, HANDL_IN"/thumbstick/click")});
    touch_bindings.push_back({input_.thumbstickPressedAction, openxr_string2path(&openxr_instance_, HANDR_IN"/thumbstick/click")});
    touch_bindings.push_back({input_.thumbstickTouchedAction, openxr_string2path(&openxr_instance_, HANDL_IN"/thumbstick/touch")});
    touch_bindings.push_back({input_.thumbstickTouchedAction, openxr_string2path(&openxr_instance_, HANDR_IN"/thumbstick/touch")});
    touch_bindings.push_back({input_.buttonAPressedAction, openxr_string2path(&openxr_instance_, HANDR_IN"/a/click")});
    touch_bindings.push_back({input_.buttonATouchedAction, openxr_string2path(&openxr_instance_, HANDR_IN"/a/touch")});
    touch_bindings.push_back({input_.buttonBPressedAction, openxr_string2path(&openxr_instance_, HANDR_IN"/b/click")});
    touch_bindings.push_back({input_.buttonBTouchedAction, openxr_string2path(&openxr_instance_, HANDR_IN"/b/touch")});
    touch_bindings.push_back({input_.buttonXPressedAction, openxr_string2path(&openxr_instance_, HANDL_IN"/x/click")});
    touch_bindings.push_back({input_.buttonXTouchedAction, openxr_string2path(&openxr_instance_, HANDL_IN"/x/touch")});
    touch_bindings.push_back({input_.buttonYPressedAction, openxr_string2path(&openxr_instance_, HANDL_IN"/y/click")});
    touch_bindings.push_back({input_.buttonYTouchedAction, openxr_string2path(&openxr_instance_, HANDL_IN"/y/touch")});
    touch_bindings.push_back({input_.squeezeValueAction, openxr_string2path(&openxr_instance_, HANDL_IN"/squeeze/value")});
    touch_bindings.push_back({input_.squeezeValueAction, openxr_string2path(&openxr_instance_, HANDR_IN"/squeeze/value")});
    touch_bindings.push_back({input_.triggerValueAction, openxr_string2path(&openxr_instance_, HANDL_IN"/trigger/value")});
    touch_bindings.push_back({input_.triggerValueAction, openxr_string2path(&openxr_instance_, HANDR_IN"/trigger/value")});
    touch_bindings.push_back({input_.triggerTouchedAction, openxr_string2path(&openxr_instance_, HANDL_IN"/trigger/touch")});
    touch_bindings.push_back({input_.triggerTouchedAction, openxr_string2path(&openxr_instance_, HANDR_IN"/trigger/touch")});
    openxr_bind_interaction(&openxr_instance_, "/interaction_profiles/oculus/touch_controller", touch_bindings);

    openxr_attach_actionset(&openxr_session_, input_.actionSet);

    input_.controllerSpace[Side::LEFT] = openxr_create_action_space(&openxr_session_,
                                                                    input_.controllerPoseAction,
                                                                    input_.handSubactionPath[Side::LEFT]);
    input_.controllerSpace[Side::RIGHT] = openxr_create_action_space(&openxr_session_,
                                                                     input_.controllerPoseAction,
                                                                     input_.handSubactionPath[Side::RIGHT]);
}

/** Retrieve current controller poses from OpenXR for the predicted display time. */
void TelepresenceProgram::PollPoses(XrTime predictedDisplayTime) {
    const XrActiveActionSet activeActionSet{input_.actionSet, XR_NULL_PATH};
    XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeActionSet;
    CHECK_XRCMD(xrSyncActions(openxr_session_, &syncInfo))

    // Controller poses
    for (int i = 0; i < Side::COUNT; i++) {
        XrSpaceVelocity vel = {XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
        loc.next = &vel;

        CHECK_XRCMD(xrLocateSpace(input_.controllerSpace[i], app_reference_space_, predictedDisplayTime, &loc))
        // When the cameras lose the controller (bright light, out of view) the runtime still reports its orientation
        // from the IMU: keep aiming with it from the last known position rather than freezing the ray
        if ((loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
            userState_.controllerPose[i].orientation = loc.pose.orientation;
        }
        if ((loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0) {
            userState_.controllerPose[i].position = loc.pose.position;
        }
    }
}

/**
 * Sync OpenXR actions and read the current state of all buttons,
 * thumbsticks, triggers, and grips for both controllers.
 */
void TelepresenceProgram::PollActions() {
    const XrActiveActionSet activeActionSet{input_.actionSet, XR_NULL_PATH};
    XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeActionSet;
    CHECK_XRCMD(xrSyncActions(openxr_session_, &syncInfo))

    XrActionStateGetInfo getQuitInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, input_.quitAction, XR_NULL_PATH};
    XrActionStateBoolean quitValue{XR_TYPE_ACTION_STATE_BOOLEAN};
    CHECK_XRCMD(xrGetActionStateBoolean(openxr_session_, &getQuitInfo, &quitValue))
    if ((quitValue.isActive == XR_TRUE) && (quitValue.changedSinceLastSync == XR_TRUE) &&
        (quitValue.currentState == XR_TRUE)) {
        CHECK_XRCMD(xrRequestExitSession(openxr_session_))
    }

    // Thumbstick pose
    XrActionStateGetInfo getThumbstickPoseRightInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr,
                                                    input_.thumbstickPoseAction,
                                                    input_.handSubactionPath[Side::RIGHT]};
    XrActionStateGetInfo getThumbstickPoseLeftInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr,
                                                   input_.thumbstickPoseAction,
                                                   input_.handSubactionPath[Side::LEFT]};
    XrActionStateVector2f thumbstickPose{XR_TYPE_ACTION_STATE_VECTOR2F};

    CHECK_XRCMD(xrGetActionStateVector2f(openxr_session_, &getThumbstickPoseRightInfo, &thumbstickPose))
    userState_.thumbstickPose[Side::RIGHT] = thumbstickPose.currentState;

    CHECK_XRCMD(xrGetActionStateVector2f(openxr_session_, &getThumbstickPoseLeftInfo, &thumbstickPose))
    userState_.thumbstickPose[Side::LEFT] = thumbstickPose.currentState;

    // Thumbstick pressed
    XrActionStateGetInfo getThumbstickPressedRightInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr,
                                                       input_.thumbstickPressedAction,
                                                       input_.handSubactionPath[Side::RIGHT]};
    XrActionStateGetInfo getThumbstickPressedLeftInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr,
                                                      input_.thumbstickPressedAction,
                                                      input_.handSubactionPath[Side::LEFT]};
    XrActionStateBoolean thumbstickPressed{XR_TYPE_ACTION_STATE_BOOLEAN};

    CHECK_XRCMD(xrGetActionStateBoolean(openxr_session_, &getThumbstickPressedRightInfo, &thumbstickPressed))
    userState_.thumbstickPressed[Side::RIGHT] = thumbstickPressed.currentState;

    CHECK_XRCMD(xrGetActionStateBoolean(openxr_session_, &getThumbstickPressedLeftInfo, &thumbstickPressed))
    userState_.thumbstickPressed[Side::LEFT] = thumbstickPressed.currentState;

    // Thumbstick touched
    XrActionStateGetInfo getThumbstickTouchedRightInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr,
                                                       input_.thumbstickTouchedAction,
                                                       input_.handSubactionPath[Side::RIGHT]};
    XrActionStateGetInfo getThumbstickTouchedLeftInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr,
                                                      input_.thumbstickTouchedAction,
                                                      input_.handSubactionPath[Side::LEFT]};
    XrActionStateBoolean thumbstickTouched{XR_TYPE_ACTION_STATE_BOOLEAN};

    CHECK_XRCMD(xrGetActionStateBoolean(openxr_session_, &getThumbstickTouchedRightInfo, &thumbstickTouched))
    userState_.thumbstickTouched[Side::RIGHT] = thumbstickTouched.currentState;

    CHECK_XRCMD(xrGetActionStateBoolean(openxr_session_, &getThumbstickTouchedLeftInfo, &thumbstickTouched))
    userState_.thumbstickTouched[Side::LEFT] = thumbstickTouched.currentState;

    // Button A
    XrActionStateGetInfo getButtonAPressedInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, input_.buttonAPressedAction, XR_NULL_PATH};
    XrActionStateGetInfo getButtonATouchedInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, input_.buttonATouchedAction, XR_NULL_PATH};
    XrActionStateBoolean buttonA{XR_TYPE_ACTION_STATE_BOOLEAN};

    CHECK_XRCMD(xrGetActionStateBoolean(openxr_session_, &getButtonAPressedInfo, &buttonA))
    userState_.aPressed = buttonA.currentState;

    CHECK_XRCMD(xrGetActionStateBoolean(openxr_session_, &getButtonATouchedInfo, &buttonA))
    userState_.aTouched = buttonA.currentState;

    // Button B
    XrActionStateGetInfo getButtonBPressedInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, input_.buttonBPressedAction, XR_NULL_PATH};
    XrActionStateGetInfo getButtonBTouchedInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, input_.buttonBTouchedAction, XR_NULL_PATH};
    XrActionStateBoolean buttonB{XR_TYPE_ACTION_STATE_BOOLEAN};

    CHECK_XRCMD(xrGetActionStateBoolean(openxr_session_, &getButtonBPressedInfo, &buttonB))
    userState_.bPressed = buttonB.currentState;

    CHECK_XRCMD(xrGetActionStateBoolean(openxr_session_, &getButtonBTouchedInfo, &buttonB))
    userState_.bTouched = buttonB.currentState;

    // Button X
    XrActionStateGetInfo getButtonXPressedInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, input_.buttonXPressedAction, XR_NULL_PATH};
    XrActionStateGetInfo getButtonXTouchedInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, input_.buttonXTouchedAction, XR_NULL_PATH};
    XrActionStateBoolean buttonX{XR_TYPE_ACTION_STATE_BOOLEAN};

    CHECK_XRCMD(xrGetActionStateBoolean(openxr_session_, &getButtonXPressedInfo, &buttonX))
    userState_.xPressed = buttonX.currentState;

    CHECK_XRCMD(xrGetActionStateBoolean(openxr_session_, &getButtonXTouchedInfo, &buttonX))
    userState_.xTouched = buttonX.currentState;

    // Button Y
    XrActionStateGetInfo getButtonYPressedInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, input_.buttonYPressedAction, XR_NULL_PATH};
    XrActionStateGetInfo getButtonYTouchedInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, input_.buttonYTouchedAction, XR_NULL_PATH};
    XrActionStateBoolean buttonY{XR_TYPE_ACTION_STATE_BOOLEAN};

    CHECK_XRCMD(xrGetActionStateBoolean(openxr_session_, &getButtonYPressedInfo, &buttonY))
    userState_.yPressed = buttonY.currentState;

    CHECK_XRCMD(xrGetActionStateBoolean(openxr_session_, &getButtonYTouchedInfo, &buttonY))
    userState_.yTouched = buttonY.currentState;

    // Squeeze
    XrActionStateGetInfo getSqueezeValueRightInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr,
                                                  input_.squeezeValueAction,
                                                  input_.handSubactionPath[Side::RIGHT]};
    XrActionStateGetInfo getSqueezeValueLeftInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr,
                                                 input_.squeezeValueAction,
                                                 input_.handSubactionPath[Side::LEFT]};
    XrActionStateFloat squeezeValue{XR_TYPE_ACTION_STATE_FLOAT};

    CHECK_XRCMD(xrGetActionStateFloat(openxr_session_, &getSqueezeValueRightInfo, &squeezeValue))
    userState_.squeezeValue[Side::RIGHT] = squeezeValue.currentState;

    CHECK_XRCMD(xrGetActionStateFloat(openxr_session_, &getSqueezeValueLeftInfo, &squeezeValue))
    userState_.squeezeValue[Side::LEFT] = squeezeValue.currentState;

    // Trigger value
    XrActionStateGetInfo getTriggerValueRightInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr,
                                                  input_.triggerValueAction,
                                                  input_.handSubactionPath[Side::RIGHT]};
    XrActionStateGetInfo getTriggerValueLeftInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr,
                                                 input_.triggerValueAction,
                                                 input_.handSubactionPath[Side::LEFT]};
    XrActionStateFloat triggerValue{XR_TYPE_ACTION_STATE_FLOAT};

    CHECK_XRCMD(xrGetActionStateFloat(openxr_session_, &getTriggerValueRightInfo, &triggerValue))
    userState_.triggerValue[Side::RIGHT] = triggerValue.currentState;

    CHECK_XRCMD(xrGetActionStateFloat(openxr_session_, &getTriggerValueLeftInfo, &triggerValue))
    userState_.triggerValue[Side::LEFT] = triggerValue.currentState;

    // Trigger touched
    XrActionStateGetInfo getTriggerTouchedRightInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr,
                                                    input_.triggerTouchedAction,
                                                    input_.handSubactionPath[Side::RIGHT]};
    XrActionStateGetInfo getTriggerTouchedLeftInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr,
                                                   input_.triggerTouchedAction,
                                                   input_.handSubactionPath[Side::LEFT]};
    XrActionStateBoolean triggerTouched{XR_TYPE_ACTION_STATE_BOOLEAN};

    CHECK_XRCMD(xrGetActionStateBoolean(openxr_session_, &getTriggerTouchedRightInfo, &triggerTouched))
    userState_.triggerTouched[Side::RIGHT] = triggerTouched.currentState;

    CHECK_XRCMD(xrGetActionStateBoolean(openxr_session_, &getTriggerTouchedLeftInfo, &triggerTouched))
    userState_.triggerTouched[Side::LEFT] = triggerTouched.currentState;
}

/**
 * Send head pose, robot control, and debug telemetry over UDP.
 *
 * Lazily initializes the RobotControlSender on first call. Tracks
 * connection health via consecutive send failures and updates the
 * AppState connection status accordingly.
 */
void TelepresenceProgram::SendControllerDatagram() {
    if (!appState_->headsetMounted) {
        return;
    }

    // Robot control sender (sends head pose and robot movement commands)
    if (robotControlSender_ == nullptr) {
        robotControlSender_ = std::make_unique<RobotControlSender>(appState_->streamingConfig, ntpTimer_.get());
        if (robotControlSender_->isInitialized()) {
            // UDP is connectionless - we can't know if destination is reachable until we try sending
            appState_->connectionState.robotControl = ConnectionStatus::Connecting;
            appState_->robotControlStatus = "Connecting";
        } else {
            appState_->connectionState.robotControl = ConnectionStatus::Failed;
            appState_->robotControlStatus = "Socket Failed";
        }
    }

    if (robotControlSender_->isInitialized()) {
        // Always send head pose
        robotControlSender_->sendHeadPose(userState_.hmdPose.orientation, userState_.hmdPoseNow.orientation,
                                          userState_.hmdPoseNowTimeUs, appState_->headMaxSpeedDegS * (float) M_PI / 180.0f,
                                          leadNow_ ? RobotControlSender::PREDICTION_NOW
                                                   : (uint32_t) appState_->headMovementPredictionMs,
                                          appState_->headMovementSensitivity, threadPool_);

        // Send robot control when enabled
        if (appState_->robotControlEnabled && !renderGui_) {
            robotControlSender_->sendRobotControl(userState_.thumbstickPose[Side::RIGHT].y,
                                              userState_.thumbstickPose[Side::RIGHT].x,
                                              userState_.thumbstickPose[Side::LEFT].x,
                                              threadPool_);
        }

        // Send debug/validation information. Left = the (only) stream in mono;
        // right is captured too so stereo reports per-eye health (right stays
        // zero-valued in mono since that pipeline/stats never updates).
        if (appState_->cameraStreamingStates.first.stats) {
            auto leftSnap = appState_->cameraStreamingStates.first.stats->snapshot();
            CameraStatsSnapshot rightSnap{};
            if (appState_->cameraStreamingStates.second.stats) {
                rightSnap = appState_->cameraStreamingStates.second.stats->snapshot();
            }
            const RobotControlSender::HeadAtDisplay head{userState_.hmdDisplayTimeUs, userState_.hmdPoseDisplay.orientation,
                                                         userState_.hmdAngularVelocityDisplay, userState_.hmdDisplayValid};
            robotControlSender_->sendDebugInfo(leftSnap, rightSnap, appState_->streamingConfig, head, threadPool_);
        }

        // NTP quality / network telemetry, throttled to ~1 Hz (it only updates per
        // sync cycle). Lets the dashboard show the clock's error bound, not just its value.
        if (ntpTimer_ && (++ntpMetricsThrottle_ % 72 == 0)) {
            robotControlSender_->sendNtpMetrics(threadPool_);
        }

        // Audio latency telemetry: robot->headset source->sink latency, emitted whenever
        // the RX pipeline has a fresh sample (read from the capture timestamp the robot
        // stamps into each Opus packet). Same relay -> InfluxDB path as the video debug info.
        if (audioPlayer_ && audioPlayer_->receiving()) {
            uint32_t audioLatencyUs;
            if (audioPlayer_->takeRxLatencyUs(audioLatencyUs)) {
                robotControlSender_->sendAudioMetrics(audioLatencyUs, threadPool_);
            }
        }

        // Update connection status based on health
        if (robotControlSender_->hasConnectionIssue()) {
            if (appState_->connectionState.robotControl != ConnectionStatus::Failed) {
                appState_->connectionState.robotControl = ConnectionStatus::Failed;
                appState_->robotControlStatus = "Connection Lost";
            }
        } else if (robotControlSender_->hasEverSucceeded() &&
                   appState_->connectionState.robotControl != ConnectionStatus::Connected) {
            // Transition to Connected only after confirmed successful sends
            appState_->connectionState.robotControl = ConnectionStatus::Connected;
            appState_->robotControlStatus = "Connected";
        }
    }
}

/**
 * Start the camera stream via REST API, then configure GStreamer pipelines.
 * If the REST call fails, the pipelines are still configured (they will
 * wait for data) so the app can recover if the server comes online later.
 */
void TelepresenceProgram::InitializeStreaming() {
    restClient_ = std::make_unique<RestClient>(appState_->streamingConfig);
    appState_->connectionState.cameraServer = ConnectionStatus::Connecting;
    appState_->cameraServerStatus = "Connecting...";

    // A start also applies the config to a stream that is already running, and waits for one that is stopping (the
    // previous run's exit) to end first; stopping here too would restart the camera on every launch
    int startResult = restClient_->StartStream();
    if (startResult != 0) {
        appState_->connectionState.cameraServer = ConnectionStatus::Failed;
        appState_->connectionState.lastError = "Camera server unreachable at " +
            IpToString(appState_->streamingConfig.jetson_ip) + ":" +
            std::to_string(Config::REST_API_PORT);
        appState_->cameraServerStatus = "Failed";
        LOG_ERROR("InitializeStreaming: Failed to start stream - camera server at %s:%d is unreachable. "
                  "Verify the server is running and the IP address is correct in the GUI settings.",
                  IpToString(appState_->streamingConfig.jetson_ip).c_str(), Config::REST_API_PORT);
    } else {
        appState_->connectionState.cameraServer = ConnectionStatus::Connected;
        appState_->cameraServerStatus = "Connected";
        LOG_INFO("InitializeStreaming: Successfully connected to camera server at %s:%d",
                 IpToString(appState_->streamingConfig.jetson_ip).c_str(), Config::REST_API_PORT);
    }

    // Configure pipelines regardless - they will wait for data
    videoPlayer_->configurePipelines(gstreamerThreadPool_, appState_->streamingConfig);

    // Record the baseline so the first Apply can diff against it and avoid an
    // unnecessary rebuild when only bitrate/quality changes.
    lastAppliedConfig_ = appState_->streamingConfig;

    // Bring the optional audio bridge in line with the (possibly persisted) flags.
    ApplyAudioState();
}

/**
 * Apply a streaming config: classify the change, rebuild the headset side if it is structural, and push it to the
 * robot. Used by the settings panel's Apply and by the adaptation controller (render thread only). rebuild: rebuild
 * the headset side even without a change (the panel's Restart stream, when no video arrives).
 */
bool TelepresenceProgram::ApplyStreamingConfig(StreamingConfig cfg, bool rebuild) {
    // Decide whether this change can ride on the live pipeline or needs a full teardown + rebuild. This MUST agree
    // with the robot driver's CanUpdateDynamically(): if the two ends disagree, the headset decoder jumps into a
    // mid-GOP stream and the OES->2D blit FBO is left at the old resolution -> black screen / GL 0x506. See
    // classifyStreamConfigChange().
    const StreamConfigChange change = lastAppliedConfig_ && !rebuild
        ? classifyStreamConfigChange(*lastAppliedConfig_, cfg)
        : StreamConfigChange::Structural;  // no baseline yet, or asked to -> rebuild

    // A codec change stops the decoder before the robot switches and builds the new one after, so a fresh decoder
    // is never fed the previous codec's stream: fed H.264, a new H.265 decoder never configured and the video
    // stalled (2026-10-14 and 10-20). Other structural changes keep the faster overlapped rebuild.
    const bool ordered = change == StreamConfigChange::Structural && lastAppliedConfig_ &&
                         lastAppliedConfig_->codec != cfg.codec;
    int updateResult = 0;
    if (ordered) {
        videoPlayer_->stopPipelines();
        updateResult = restClient_->UpdateStreamingConfig(cfg);
        LOG_INFO("Apply: codec change, decoder stopped and config sent before the rebuild");
    }

    switch (change) {
        case StreamConfigChange::Structural:
            LOG_INFO("Apply: structural change -> rebuilding decode pipeline + GL render targets");
            init_scene(cfg.resolution.getWidth(), cfg.resolution.getHeight(), true);
            videoPlayer_->configurePipelines(gstreamerThreadPool_, cfg);
            break;
        case StreamConfigChange::LiveOnly:
            LOG_INFO("Apply: live-only change (bitrate/quality/fps) -> no rebuild, keeping pipeline");
            videoPlayer_->setStreamFps(cfg.fps);
            break;
        case StreamConfigChange::None:
            LOG_INFO("Apply: no streaming-config change -> nothing to rebuild");
            break;
    }

    // Always push the config to the robot. For a live-only change it updates the encoder in place; for a
    // structural change it rebuilds its pipeline and emits a fresh keyframe so the rebuilt headset decoder re-syncs.
    if (!ordered) updateResult = restClient_->UpdateStreamingConfig(cfg);
    if (updateResult != 0) {
        appState_->connectionState.cameraServer = ConnectionStatus::Failed;
        appState_->cameraServerStatus = "Update Failed";
        LOG_ERROR("Apply: failed to update streaming config - camera server not responding");
        return false;
    }
    appState_->connectionState.cameraServer = ConnectionStatus::Connected;
    appState_->cameraServerStatus = "Connected";
    lastAppliedConfig_ = cfg;  // remember only what the robot acknowledged
    if (adaptation_) adaptation_->onApplied(cfg);
    return true;
}

/**
 * Build the data-driven GUI settings table.
 *
 * Each GuiSetting defines a row in the in-VR settings panel with lambdas
 * for display text, increment/decrement, and activation. The lambdas capture
 * `this` to access appState_ and other members at render time.
 *
 * Sections: Network, Streaming & Rendering, Status Information.
 */
void TelepresenceProgram::ApplyAudioState() {
    if (!audioPlayer_ || !restClient_) return;

    const bool hear = appState_->audioRobotEnable;   // robot mic -> headset speakers (RX)
    const bool talk = appState_->audioMicEnable;      // headset mic -> robot speaker  (TX)
    const int volume = appState_->audioVolume;
    const bool startMuted = appState_->micPushToTalk ? true : appState_->micMuted;
    const std::string robotIp = IpToString(appState_->streamingConfig.jetson_ip);

    audioThreadPool_.detach_task([this, hear, talk, volume, startMuted, robotIp]() {
        if (!hear && !talk) {
            audioPlayer_->stopSend();
            audioPlayer_->stopReceive();
            restClient_->StopAudio();
            return;
        }

        // Tell the robot bridge which legs to run: it transmits its mic when we want to
        // hear it, and plays the operator when we transmit. AEC requested on the robot.
        restClient_->StartAudio(/*robotMicToHeadset=*/hear, /*operatorToSpeaker=*/talk, /*aecEnabled=*/true);

        if (hear) audioPlayer_->startReceive(Config::AUDIO_RX_PORT, volume);
        else      audioPlayer_->stopReceive();

        if (talk) audioPlayer_->startSend(robotIp, Config::AUDIO_TX_PORT, 64000, startMuted);
        else      audioPlayer_->stopSend();

        audioPlayer_->setVolume(volume);
    });
}

void TelepresenceProgram::BuildSettings() {
    SettingsActions actions;
    actions.applyStream = [this]() {
        stateStorage_->SaveAppState(*appState_);
        ApplyStreamingConfig(appState_->streamingConfig);
    };
    actions.restartStream = [this]() { ApplyStreamingConfig(appState_->streamingConfig, true); };
    actions.applyAudio = [this]() { ApplyAudioState(); };
    actions.setVolume = [this](int volume) { if (audioPlayer_) audioPlayer_->setVolume(volume); };
    actions.setRobotDrive = [this](bool on) {
        appState_->robotControlEnabled = on;
        if (!on && robotControlSender_ && robotControlSender_->isInitialized()) {
            robotControlSender_->sendRobotControl(0.0f, 0.0f, 0.0f, threadPool_);   // stop the base
        }
    };
    actions.setKeepRunningOffHead = [this](bool on) {
        // The system's VR power manager treats the headset as worn until told to use the proximity sensor again
        appState_->keepRunningOffHead = on;
        SendSystemBroadcast(on ? "com.oculus.vrpowermanager.prox_close" : "com.oculus.vrpowermanager.automation_disable");
    };
    actions.resetHeadDrives = [this]() {
        if (robotControlSender_ && robotControlSender_->isInitialized()) robotControlSender_->sendServoReset(threadPool_);
    };
    actions.setPanelLayout = [this](GuiPanelLayout layout) {
        appState_->guiPanel.layout = layout;
        PlacePanel();
        stateStorage_->SaveAppState(*appState_);
    };
    actions.appliedConfig = [this]() { return lastAppliedConfig_ ? &*lastAppliedConfig_ : nullptr; };
    settings_ = BuildSettingsTable(appState_, actions);
}

/**
 * The settings panel's frame: a rotation by pitch about +X, then by yaw about +Y (at 0, 0 the panel faces +Z).
 */
static XrVector3f ToPanelFrame(const GuiPanelState &panel, const XrVector3f &v) {
    const float cy = std::cos(panel.yaw), sy = std::sin(panel.yaw), cp = std::cos(panel.pitch), sp = std::sin(panel.pitch);
    const XrVector3f u{cy * v.x - sy * v.z, v.y, sy * v.x + cy * v.z};
    return {u.x, cp * u.y + sp * u.z, -sp * u.y + cp * u.z};
}

static XrVector3f FromPanelFrame(const GuiPanelState &panel, const XrVector3f &v) {
    const float cy = std::cos(panel.yaw), sy = std::sin(panel.yaw), cp = std::cos(panel.pitch), sp = std::sin(panel.pitch);
    const XrVector3f u{v.x, cp * v.y - sp * v.z, sp * v.y + cp * v.z};
    return {cy * u.x + sy * u.z, u.y, -sy * u.x + cy * u.z};
}

/**
 * Put the settings panel where its layout opens, facing the head: Front 0.7 m ahead and 0.1 m below eye level;
 * Below 0.6 m away and 36 deg down, 20 deg tall, so that its top edge meets the bottom edge of the Full FOV video
 * (26.6 deg below the eyes at 16:9).
 */
void TelepresenceProgram::PlacePanel() {
    const XrPosef &head = userState_.headInApp;
    const auto &q = head.orientation;
    XrVector3f fwd{-2.0f * (q.x * q.z + q.w * q.y), 0.0f, -(1.0f - 2.0f * (q.x * q.x + q.y * q.y))};
    const float len = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);
    if (len < 1e-3f) return;   // looking straight up or down: keep the panel where it is
    fwd.x /= len;
    fwd.z /= len;
    auto &panel = appState_->guiPanel;
    if (panel.layout == GuiPanelLayout::Below) {
        constexpr float DISTANCE = 0.6f, DOWN = 36.0f * (float) M_PI / 180.0f, TALL = 20.0f * (float) M_PI / 180.0f;
        const float ahead = DISTANCE * std::cos(DOWN);
        panel.position = {head.position.x + fwd.x * ahead, head.position.y - DISTANCE * std::sin(DOWN),
                          head.position.z + fwd.z * ahead};
        panel.height = 2.0f * DISTANCE * std::tan(TALL * 0.5f);
    } else {
        constexpr float DISTANCE = 0.7f, BELOW_EYES = 0.1f;
        panel.position = {head.position.x + fwd.x * DISTANCE, head.position.y - BELOW_EYES, head.position.z + fwd.z * DISTANCE};
        panel.height = 0.6f;
    }
    FacePanelToHead();
}

/**
 * Send a broadcast intent from the app's context (used for the system's VR power manager).
 */
void TelepresenceProgram::SendSystemBroadcast(const char *action) {
    JNIEnv *env = nullptr;
    if (!app_ || app_->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
    jclass intentClass = env->FindClass("android/content/Intent");
    jmethodID ctor = env->GetMethodID(intentClass, "<init>", "(Ljava/lang/String;)V");
    jstring jaction = env->NewStringUTF(action);
    jobject intent = env->NewObject(intentClass, ctor, jaction);
    jobject activity = app_->activity->clazz;
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID send = env->GetMethodID(activityClass, "sendBroadcast", "(Landroid/content/Intent;)V");
    env->CallVoidMethod(activity, send, intent);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        LOG_ERROR("Broadcast %s failed", action);
    } else {
        LOG_INFO("Broadcast %s sent", action);
    }
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(intent);
    env->DeleteLocalRef(jaction);
    env->DeleteLocalRef(intentClass);
}

/**
 * Turn the settings panel to face the head, sideways and up or down; it stays level (no roll).
 */
void TelepresenceProgram::FacePanelToHead() {
    auto &panel = appState_->guiPanel;
    const XrVector3f &h = userState_.headInApp.position;
    const float dx = h.x - panel.position.x, dy = h.y - panel.position.y, dz = h.z - panel.position.z;
    const float level = std::sqrt(dx * dx + dz * dz);
    if (level > 1e-3f) panel.yaw = std::atan2(dx, dz);
    if (level + std::abs(dy) > 1e-3f) panel.pitch = -std::atan2(dy, level);
}

/**
 * Process VR controller input for GUI navigation and robot control.
 *
 * Controls:
 *   Right controller ray:   point at settings panel (mouse cursor)
 *   Right trigger:          click GUI elements under cursor
 *   Right grip:             grab and move the settings panel
 *   Right thumbstick press: toggle robot movement control on/off
 *   Left thumbstick press:  toggle settings GUI visibility
 *   Left thumbstick axis:   navigate settings (up/down/left/right)
 *   Y button:               increment focused setting value
 *   X button:               decrement focused setting value
 *   Left trigger:           activate focused button setting
 */
void TelepresenceProgram::HandleControllers() {
    // A row control pressed with the controller ray (render_imgui.cpp), run here, outside the panel's drawing
    {
        auto &gc = appState_->guiControl;
        if (gc.pendingRow >= 0 && gc.pendingRow < (int) settings_.size()) {
            auto &setting = settings_[gc.pendingRow];
            switch (gc.pendingAction) {
                case GuiAction::Increment: if (setting.onIncrement) setting.onIncrement(); break;
                case GuiAction::Decrement: if (setting.onDecrement) setting.onDecrement(); break;
                case GuiAction::Activate:  if (setting.onActivate) setting.onActivate(); break;
                case GuiAction::Choose:    if (setting.setChoice) setting.setChoice(gc.pendingValue); break;
                case GuiAction::Toggle:    if (setting.setToggle) setting.setToggle(gc.pendingValue != 0); break;
                case GuiAction::None:      break;
            }
        }
        gc.pendingRow = -1;
        gc.pendingAction = GuiAction::None;
    }

    if (userState_.thumbstickPressed[Side::RIGHT] && !controlLockMovement_) {
        appState_->robotControlEnabled = !appState_->robotControlEnabled;
        if (!appState_->robotControlEnabled) {
            // Send stop command (all zeros) when disabling robot control
            if (robotControlSender_ && robotControlSender_->isInitialized()) {
                robotControlSender_->sendRobotControl(0.0f, 0.0f, 0.0f, threadPool_);
            }
        }
        controlLockMovement_ = true;
    }
    if (!userState_.thumbstickPressed[Side::RIGHT] && controlLockMovement_) {
        controlLockMovement_ = false;
    }

    // Toggling GUI rendering
    if (userState_.thumbstickPressed[Side::LEFT] && !controlLockGui_) {
        renderGui_ = !renderGui_;
        if (renderGui_) {
            PlacePanel();
        } else {
            stateStorage_->SaveAppState(*appState_);
        }
        controlLockGui_ = true;
    }
    if (!userState_.thumbstickPressed[Side::LEFT] && controlLockGui_) {
        controlLockGui_ = false;
    }

    // GUI interaction
    if (appState_->guiControl.cooldown > 0) {
        appState_->guiControl.cooldown -= 1;
    }

    if (renderGui_ && !appState_->guiControl.changesEnqueued && appState_->guiControl.cooldown == 0) {

        // Focus move UP
        if (userState_.thumbstickPose[Side::LEFT].y > 0.9f) {
            appState_->guiControl.focusMoveUp = true;
            appState_->guiControl.changesEnqueued = true;
        }

            // Focus move DOWN
        else if (userState_.thumbstickPose[Side::LEFT].y < -0.9f) {
            appState_->guiControl.focusMoveDown = true;
            appState_->guiControl.changesEnqueued = true;
        }

            // Focus move LEFT
        else if (userState_.thumbstickPose[Side::LEFT].x < -0.9f) {
            appState_->guiControl.focusMoveLeft = true;
            appState_->guiControl.changesEnqueued = true;
        }

            // Focus move RIGHT
        else if (userState_.thumbstickPose[Side::LEFT].x > 0.9f) {
            appState_->guiControl.focusMoveRight = true;
            appState_->guiControl.changesEnqueued = true;
        }

            // Value increment (Y button)
        else if (userState_.yPressed && appState_->guiControl.focusedElement >= 0) {
            auto& setting = settings_[appState_->guiControl.focusedElement];
            if (setting.onIncrement) setting.onIncrement();
            appState_->guiControl.changesEnqueued = true;
        }

            // Value decrement (X button)
        else if (userState_.xPressed && appState_->guiControl.focusedElement >= 0) {
            auto& setting = settings_[appState_->guiControl.focusedElement];
            if (setting.onDecrement) setting.onDecrement();
            appState_->guiControl.changesEnqueued = true;
        }

            // Button activation (left trigger)
        else if (userState_.triggerValue[Side::LEFT] > 0.9f && appState_->guiControl.focusedElement >= 0) {
            auto& setting = settings_[appState_->guiControl.focusedElement];
            if (setting.control == GuiControl::Button && setting.onActivate && (!setting.isEnabled || setting.isEnabled())) {
                setting.onActivate();
                appState_->guiControl.changesEnqueued = true;
            }
        }
    }

    // --- Right controller ray interaction with GUI panel ---
    if (renderGui_) {
        const XrPosef &aim = userState_.controllerPose[Side::RIGHT];
        const auto &q = aim.orientation;

        // Forward direction from aim quaternion (OpenXR: forward is -Z)
        XrVector3f fwd{
                -2.0f * (q.x * q.z + q.w * q.y),
                -2.0f * (q.y * q.z - q.w * q.x),
                -(1.0f - 2.0f * (q.x * q.x + q.y * q.y))
        };

        auto &panel = appState_->guiPanel;
        panel.rayOrigin = aim.position;
        panel.rayActive = true;
        panel.rayHitting = false;
        const float halfW = panel.getWorldWidth() * 0.5f;
        const float halfH = panel.height * 0.5f;

        // Ray against the panel's plane, in the panel's own frame (the panel faces +Z there)
        float hitX = 0.0f, hitY = 0.0f, t = -1.0f;
        {
            const XrVector3f o = ToPanelFrame(panel, {aim.position.x - panel.position.x, aim.position.y - panel.position.y,
                                                      aim.position.z - panel.position.z});
            const XrVector3f d = ToPanelFrame(panel, fwd);
            if (std::abs(d.z) > 1e-6f) {
                t = -o.z / d.z;
                hitX = o.x + t * d.x;
                hitY = o.y + t * d.y;
            }
        }
        if (t > 0.0f && std::abs(hitX) <= halfW && std::abs(hitY) <= halfH) {
            const GuiPanelGeometry g = panel.geometry();
            panel.rayHitting = true;
            panel.rayEnd = {aim.position.x + t * fwd.x, aim.position.y + t * fwd.y, aim.position.z + t * fwd.z};
            panel.imguiMouseX = (hitX + halfW) / (2.0f * halfW) * (float) g.width;
            panel.imguiMouseY = (halfH - hitY) / (2.0f * halfH) * (float) g.height;
            panel.triggerDown = userState_.triggerValue[Side::RIGHT] > 0.5f;
        }

        if (!panel.rayHitting) {
            // Extend ray to a fixed length for visualization
            constexpr float RAY_LENGTH = 3.0f;
            panel.rayEnd = {
                    aim.position.x + fwd.x * RAY_LENGTH,
                    aim.position.y + fwd.y * RAY_LENGTH,
                    aim.position.z + fwd.z * RAY_LENGTH
            };
            panel.triggerDown = false;
            panel.scrollDelta = 0.0f;
        }

        // Grab-to-move/resize with right grip
        float gripValue = userState_.squeezeValue[Side::RIGHT];
        if (gripValue > 0.5f && panel.rayHitting && !panel.grabbing) {
            panel.grabbing = true;
            panel.grabHitX = hitX;
            panel.grabHitY = hitY;
            panel.grabDistance = t;
            panel.grabInitialHeight = panel.height;
        }
        if (panel.grabbing) {
            if (gripValue < 0.3f) {
                panel.grabbing = false;
            } else {
                // Resize with right thumbstick Y while gripping
                float thumbY = userState_.thumbstickPose[Side::RIGHT].y;
                if (std::abs(thumbY) > 0.1f) {
                    panel.height += thumbY * 0.01f;
                    panel.height = std::max(0.15f, std::min(3.0f, panel.height));
                }

                // The grabbed point stays on the ray as far out as it was grabbed, and the panel keeps facing the
                // head, so it swivels both ways as it is carried around
                const float k = panel.height / panel.grabInitialHeight;
                const XrVector3f held{aim.position.x + fwd.x * panel.grabDistance, aim.position.y + fwd.y * panel.grabDistance,
                                      aim.position.z + fwd.z * panel.grabDistance};
                FacePanelToHead();
                const XrVector3f offset = FromPanelFrame(panel, {panel.grabHitX * k, panel.grabHitY * k, 0.0f});
                panel.position = {held.x - offset.x, held.y - offset.y, held.z - offset.z};
            }
        }

        // Scroll with right thumbstick Y when pointing (not gripping)
        if (panel.rayHitting && !panel.grabbing) {
            float thumbY = userState_.thumbstickPose[Side::RIGHT].y;
            panel.scrollDelta = (std::abs(thumbY) > 0.1f) ? thumbY * 0.3f : 0.0f;
        } else if (!panel.grabbing) {
            panel.scrollDelta = 0.0f;
        }
    } else {
        appState_->guiPanel.rayActive = false;
        appState_->guiPanel.grabbing = false;
    }
}