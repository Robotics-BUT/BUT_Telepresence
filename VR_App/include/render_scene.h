#pragma once

#include "util_openxr.h"
#include "linear.h"
#include "types/app_state.h"
#include "types/camera_types.h"
#include "types/gui_setting.h"

struct Quad {
    XrPosef Pose;
    XrVector3f Scale;
};

void init_scene(int textureWidth, int textureHeight, bool reinit = false);

void generate_shader();

void init_image_plane(int textureWidth, int textureHeight);

void render_scene(const XrCompositionLayerProjectionView &layerView, render_target_t &rtarget,
                  const Quad &quad, const std::shared_ptr<AppState> &appState,
                  const CameraFrame *image, bool drawSettingsGui,
                  const std::vector<GuiSetting> &settings);

int draw_image_plane(const XrMatrix4x4f &vp, const Quad &quad, const CameraFrame *image);

int draw_imgui(const XrMatrix4x4f &vp, const std::shared_ptr<AppState> &appState,
               bool drawSettingsGui, const std::vector<GuiSetting> &settings);