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

class TelepresenceProgram {

public:
    TelepresenceProgram(struct android_app *app);

    ~TelepresenceProgram();

    void UpdateFrame();

private:
    void InitializeActions();

    void PollActions();

    void PollPoses(XrTime predictedDisplayTime);

    void RenderFrame();

    bool RenderLayer(XrTime displayTime, std::vector<XrCompositionLayerProjectionView> &layerViews,
                     XrCompositionLayerProjection &layer);

    void SendControllerDatagram();

    void InitializeStreaming();

    void HandleControllers();

    void BuildSettings();

    XrInstance openxr_instance_ = XR_NULL_HANDLE;
    XrSystemId openxr_system_id_ = XR_NULL_SYSTEM_ID;
    XrSession openxr_session_ = XR_NULL_HANDLE;

    std::vector<viewsurface_t> viewsurfaces_;

    std::vector<XrSpace> reference_spaces_;
    XrSpace app_reference_space_;

    InputState input_;
    UserState userState_;

    bool mono_ = false;
    bool renderGui_ = true;
    bool controlLockMovement_ = false;
    bool controlLockGui_ = false;

    BS::thread_pool<BS::tp::none> gstreamerThreadPool_{1};
    BS::thread_pool<BS::tp::none> threadPool_{3};

    std::unique_ptr<GstreamerPlayer> gstreamerPlayer_;
    std::unique_ptr<RestClient> restClient_;
    std::unique_ptr<NtpTimer> ntpTimer_;
    std::unique_ptr<RosNetworkGatewayClient> rosNetworkGatewayClient_;

    std::unique_ptr<RobotControlSender> robotControlSender_;

    std::unique_ptr<StateStorage> stateStorage_;

    std::chrono::time_point<std::chrono::high_resolution_clock> prevFrameStart_, frameStart_;

    std::shared_ptr<AppState> appState_{};

    std::vector<GuiSetting> settings_;
};