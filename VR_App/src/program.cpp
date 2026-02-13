/**
 * program.cpp - TelepresenceProgram implementation
 *
 * Contains the full VR application lifecycle: initialization, per-frame
 * update/render loop, controller input handling, and the data-driven
 * settings table. See program.h for the class overview.
 */

#include "pch.h"
#include "log.h"
#include "check.h"
#include "render_scene.h"
#include "render_imgui.h"

#include <utility>
#include <algorithm>
#include <GLES3/gl32.h>

#include "program.h"
#include "utils/network_utils.h"

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
TelepresenceProgram::TelepresenceProgram(struct android_app *app) {

    /* Initialize the OpenXR loader which detects and interfaces with the XR runtime */
    openxr_init_loader(app);

    openxr_create_instance(app, &openxr_instance_);

    openxr_get_system_id(&openxr_instance_, &openxr_system_id_);

    egl_init_with_pbuffer_surface();
    openxr_confirm_gfx_reqs(&openxr_instance_, &openxr_system_id_);

    stateStorage_ = std::make_unique<StateStorage>(app);

    appState_ = std::make_shared<AppState>(stateStorage_->LoadAppState());
    appState_->streamingConfig.headset_ip = GetLocalIPAddr();

    init_scene(appState_->streamingConfig.resolution.getWidth(), appState_->streamingConfig.resolution.getHeight());

    openxr_create_session(&openxr_instance_, &openxr_system_id_, &openxr_session_);
    openxr_log_reference_spaces(&openxr_session_);
    openxr_create_reference_spaces(&openxr_session_, reference_spaces_);
    app_reference_space_ = reference_spaces_[0]; // "ViewFront"

    viewsurfaces_ = openxr_create_swapchains(&openxr_instance_, &openxr_system_id_, &openxr_session_);

    ntpTimer_ = std::make_unique<NtpTimer>(IpToString(appState_->streamingConfig.jetson_ip), "195.113.144.201");
    ntpTimer_->StartAutoSync();
    gstreamerPlayer_ = std::make_unique<GstreamerPlayer>(&appState_->cameraStreamingStates, ntpTimer_.get());
    rosNetworkGatewayClient_ = std::make_unique<RosNetworkGatewayClient>();

    appState_->systemInfo.openXrRuntime = openxr_get_runtime_name(&openxr_instance_);
    appState_->systemInfo.openXrSystem = openxr_get_system_name(&openxr_instance_, &openxr_system_id_);
    appState_->systemInfo.openGlVersion = glGetString(GL_VERSION);
    appState_->systemInfo.openGlVendor = glGetString(GL_VENDOR);
    appState_->systemInfo.openGlRenderer = glGetString(GL_RENDERER);

    InitializeActions();
    InitializeStreaming();
    BuildSettings();
}

TelepresenceProgram::~TelepresenceProgram() {
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
        if (ntpTimer_->IsSyncHealthy()) {
            appState_->connectionState.ntpSync = ConnectionStatus::Connected;
            appState_->ntpSyncStatus = "Synced";
        } else if (ntpTimer_->GetConsecutiveFailures() > 0) {
            appState_->connectionState.ntpSync = ConnectionStatus::Failed;
            appState_->ntpSyncStatus = "Not Synced";
        }
    }

    PollActions();
    SendControllerDatagram();

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

    openxr_end_frame(&openxr_session_, &display_time, layers);
    auto end = std::chrono::high_resolution_clock::now();
    appState_->appFrameTime = std::chrono::duration_cast<std::chrono::microseconds>(
            end - frameStart_).count();
    auto frameDuration = std::chrono::duration_cast<std::chrono::microseconds>(
            frameStart_ - prevFrameStart_).count();
    appState_->appFrameRate = (frameDuration > 0) ? (1e6f / frameDuration) : 0.0f;
}

/**
 * Render both eye views into their swapchain images.
 *
 * For each eye: acquires a swapchain image, renders the camera image plane
 * and ImGui overlay, then releases the image. Head movement prediction is
 * applied by offsetting displayTime by headMovementPredictionMs.
 */
bool TelepresenceProgram::RenderLayer(XrTime displayTime,
                                      std::vector<XrCompositionLayerProjectionView> &layerViews,
                                      XrCompositionLayerProjection &layer) {
    displayTime += appState_->headMovementPredictionMs * 1e6;
    auto viewCount = viewsurfaces_.size();
    std::vector<XrView> views(viewCount, {XR_TYPE_VIEW});
    openxr_locate_views(&openxr_session_, &displayTime, app_reference_space_, viewCount,
                        views.data());

    layerViews.resize(viewCount);

    XrSpaceLocation spaceLocation{XR_TYPE_SPACE_LOCATION};

    // Locate "Local" space relative to "ViewFront"
    auto res = xrLocateSpace(reference_spaces_[1], app_reference_space_, displayTime,
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

        CameraFrame *imageHandle = i == 0 ? &appState_->cameraStreamingStates.second
                                          : &appState_->cameraStreamingStates.first;

        HandleControllers();

        if (mono_) imageHandle = &appState_->cameraStreamingStates.first;

        // Calculate presentation latency (frame ready → about to render)
        uint64_t frameReadyTime = imageHandle->stats->frameReadyTimestamp.load();
        if (frameReadyTime > 0) {
            uint64_t renderTime = ntpTimer_->GetCurrentTimeUs();
            imageHandle->stats->presentation.store(renderTime - frameReadyTime);
        }

        render_scene(layerViews[i], rtarget, quad, appState_, imageHandle, renderGui_, settings_);

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
        if ((loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0) {
            userState_.controllerPose[i] = loc.pose;
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
        robotControlSender_->sendHeadPose(userState_.hmdPose.orientation, appState_->headMovementMaxSpeed, threadPool_);

        // Send robot control when enabled
        if (appState_->robotControlEnabled && !renderGui_) {
            robotControlSender_->sendRobotControl(userState_.thumbstickPose[Side::RIGHT].y,
                                              userState_.thumbstickPose[Side::RIGHT].x,
                                              userState_.thumbstickPose[Side::LEFT].x,
                                              threadPool_);
        }

        // Send debug/validation information
        if (appState_->cameraStreamingStates.first.stats) {
            auto snapshot = appState_->cameraStreamingStates.first.stats->snapshot();
            robotControlSender_->sendDebugInfo(snapshot, threadPool_);
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

    // Stop any existing stream (OK to fail if not running)
    restClient_->StopStream();

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
    gstreamerPlayer_->configurePipelines(gstreamerThreadPool_, appState_->streamingConfig);
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
void TelepresenceProgram::BuildSettings() {
    auto noop = []() {};

    settings_ = {
        // --- Network ---
        {
            "Headset IP", GuiSettingType::IpAddress, "Network",
            [this]() { return fmt::format("Headset IP: {}", IpToString(appState_->streamingConfig.headset_ip)); },
            noop, noop, nullptr, 4
        },
        {
            "Telepresence IP", GuiSettingType::IpAddress, "",
            [this]() { return fmt::format("Telepresence IP: {}", IpToString(appState_->streamingConfig.jetson_ip)); },
            [this]() { appState_->streamingConfig.jetson_ip[appState_->guiControl.focusedSegment] += 1; },
            [this]() { appState_->streamingConfig.jetson_ip[appState_->guiControl.focusedSegment] -= 1; },
            nullptr, 4
        },

        // --- Streaming & Rendering ---
        {
            "Codec", GuiSettingType::Text, "Streaming & Rendering",
            [this]() { return fmt::format("Codec: {}", CodecToString(appState_->streamingConfig.codec)); },
            [this]() {
                auto& codec = appState_->streamingConfig.codec;
                codec = static_cast<Codec>(
                    (static_cast<int>(codec) + 1 + static_cast<int>(Codec::Count)) % static_cast<int>(Codec::Count));
                if (codec == Codec::VP8 || codec == Codec::VP9) codec = Codec::H264;
            },
            [this]() {
                auto& codec = appState_->streamingConfig.codec;
                codec = static_cast<Codec>(
                    (static_cast<int>(codec) - 1 + static_cast<int>(Codec::Count)) % static_cast<int>(Codec::Count));
                if (codec == Codec::VP8 || codec == Codec::VP9) codec = Codec::JPEG;
            }
        },
        {
            "Encoding quality", GuiSettingType::Text, "",
            [this]() { return fmt::format("Encoding quality: {}", appState_->streamingConfig.encodingQuality); },
            [this]() { appState_->streamingConfig.encodingQuality = std::min(appState_->streamingConfig.encodingQuality + 1, 100); },
            [this]() { appState_->streamingConfig.encodingQuality = std::max(appState_->streamingConfig.encodingQuality - 1, 0); }
        },
        {
            "Bitrate", GuiSettingType::Text, "",
            [this]() { return fmt::format("Bitrate: {}", appState_->streamingConfig.bitrate); },
            [this]() { appState_->streamingConfig.bitrate = std::min(appState_->streamingConfig.bitrate + 1000000, 100000000); },
            [this]() { appState_->streamingConfig.bitrate = std::max(appState_->streamingConfig.bitrate - 1000000, 1000000); }
        },
        {
            "Video Mode", GuiSettingType::Text, "",
            [this]() { return fmt::format("{}", VideoModeToString(appState_->streamingConfig.videoMode)); },
            [this]() {
                auto& vm = appState_->streamingConfig.videoMode;
                vm = static_cast<VideoMode>(
                    (static_cast<int>(vm) + 1 + static_cast<int>(VideoMode::Count)) % static_cast<int>(VideoMode::Count));
                mono_ = (vm == VideoMode::Mono);
            },
            [this]() {
                auto& vm = appState_->streamingConfig.videoMode;
                vm = static_cast<VideoMode>(
                    (static_cast<int>(vm) - 1 + static_cast<int>(VideoMode::Count)) % static_cast<int>(VideoMode::Count));
                mono_ = (vm == VideoMode::Mono);
            }
        },
        {
            "Aspect Ratio", GuiSettingType::Text, "",
            [this]() { return fmt::format("{}", AspectRatioModeToString(appState_->aspectRatioMode)); },
            [this]() {
                appState_->aspectRatioMode = static_cast<AspectRatioMode>(
                    (static_cast<int>(appState_->aspectRatioMode) + 1 + static_cast<int>(AspectRatioMode::Count)) % static_cast<int>(AspectRatioMode::Count));
            },
            [this]() {
                appState_->aspectRatioMode = static_cast<AspectRatioMode>(
                    (static_cast<int>(appState_->aspectRatioMode) - 1 + static_cast<int>(AspectRatioMode::Count)) % static_cast<int>(AspectRatioMode::Count));
            }
        },
        {
            "FPS", GuiSettingType::Text, "",
            [this]() { return fmt::format("FPS: {}", appState_->streamingConfig.fps); },
            [this]() { if (appState_->streamingConfig.fps < 80) appState_->streamingConfig.fps += 1; },
            [this]() { if (appState_->streamingConfig.fps > 1) appState_->streamingConfig.fps -= 1; }
        },
        {
            "Resolution", GuiSettingType::Text, "",
            [this]() {
                auto& r = appState_->streamingConfig.resolution;
                return fmt::format("Resolution: {}x{}({})", r.getWidth(), r.getHeight(), r.getLabel());
            },
            [this]() {
                auto& r = appState_->streamingConfig.resolution;
                if (r.getIndex() < CameraResolution::count() - 1)
                    appState_->streamingConfig.resolution = CameraResolution::fromIndex(r.getIndex() + 1);
            },
            [this]() {
                auto& r = appState_->streamingConfig.resolution;
                if (r.getIndex() > 0)
                    appState_->streamingConfig.resolution = CameraResolution::fromIndex(r.getIndex() - 1);
            }
        },

        // --- Apply button ---
        {
            "Apply", GuiSettingType::Button, "",
            nullptr, noop, noop,
            [this]() {
                stateStorage_->SaveAppState(*appState_);
                init_scene(appState_->streamingConfig.resolution.getWidth(),
                           appState_->streamingConfig.resolution.getHeight(), true);
                gstreamerPlayer_->configurePipelines(gstreamerThreadPool_, appState_->streamingConfig);

                int updateResult = restClient_->UpdateStreamingConfig(appState_->streamingConfig);
                if (updateResult != 0) {
                    appState_->connectionState.cameraServer = ConnectionStatus::Failed;
                    appState_->cameraServerStatus = "Update Failed";
                    LOG_ERROR("HandleControllers: Failed to update streaming config - camera server not responding");
                } else {
                    appState_->connectionState.cameraServer = ConnectionStatus::Connected;
                    appState_->cameraServerStatus = "Connected";
                }
            }
        },

        // --- Status Information ---
        {
            "Head movement max speed", GuiSettingType::Text, "Status Information",
            [this]() { return fmt::format("Camera head movement max speed: {}", appState_->headMovementMaxSpeed); },
            [this]() { if (appState_->headMovementMaxSpeed < 990000) appState_->headMovementMaxSpeed += 10000; },
            [this]() { if (appState_->headMovementMaxSpeed > 110000) appState_->headMovementMaxSpeed -= 10000; }
        },
        {
            "Head movement speed multiplier", GuiSettingType::Text, "",
            [this]() { return fmt::format("Head movement speed multiplier: {:.2}", appState_->headMovementSpeedMultiplier); },
            [this]() { if (appState_->headMovementSpeedMultiplier < 2.0f) appState_->headMovementSpeedMultiplier += 0.1f; },
            [this]() { if (appState_->headMovementSpeedMultiplier > 0.5f) appState_->headMovementSpeedMultiplier -= 0.1f; }
        },
        {
            "Headset movement prediction", GuiSettingType::Text, "",
            [this]() { return fmt::format("Headset movement prediction: {} ms", appState_->headMovementPredictionMs); },
            [this]() { if (appState_->headMovementPredictionMs < 100) appState_->headMovementPredictionMs += 1; },
            [this]() { if (appState_->headMovementPredictionMs > 0) appState_->headMovementPredictionMs -= 1; }
        },
    };
}

/**
 * Process VR controller input for GUI navigation and robot control.
 *
 * Controls:
 *   Right thumbstick press: toggle robot movement control on/off
 *   Left thumbstick press:  toggle settings GUI visibility
 *   Left thumbstick axis:   navigate settings (up/down/left/right)
 *   Y button:               increment focused setting value
 *   X button:               decrement focused setting value
 *   Left trigger:           activate focused button setting
 */
void TelepresenceProgram::HandleControllers() {
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
        if (!renderGui_) {
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
        else if (userState_.yPressed) {
            auto& setting = settings_[appState_->guiControl.focusedElement];
            if (setting.onIncrement) setting.onIncrement();
            appState_->guiControl.changesEnqueued = true;
        }

            // Value decrement (X button)
        else if (userState_.xPressed) {
            auto& setting = settings_[appState_->guiControl.focusedElement];
            if (setting.onDecrement) setting.onDecrement();
            appState_->guiControl.changesEnqueued = true;
        }

            // Button activation (left trigger)
        else if (userState_.triggerValue[Side::LEFT] > 0.9f) {
            auto& setting = settings_[appState_->guiControl.focusedElement];
            if (setting.type == GuiSettingType::Button && setting.onActivate) {
                setting.onActivate();
                appState_->guiControl.changesEnqueued = true;
            }
        }
    }
}