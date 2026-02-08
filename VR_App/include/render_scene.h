/**
 * render_scene.h - Scene rendering coordination
 *
 * Orchestrates per-frame rendering: the camera image plane (stereo video)
 * and the ImGui settings overlay. Uses separate shaders for GL_TEXTURE_2D
 * (JPEG software decode) and GL_TEXTURE_EXTERNAL_OES (hardware decode).
 */
#pragma once

#include "util_openxr.h"
#include "linear.h"
#include "types/app_state.h"
#include "types/camera_types.h"
#include "types/gui_setting.h"

/** A positioned and scaled quad for rendering the camera image in VR space. */
struct Quad {
    XrPosef Pose;
    XrVector3f Scale;
};

/** Initialize shaders, geometry, textures, ImGui, and the settings FBO. */
void init_scene(int textureWidth, int textureHeight, bool reinit = false);

/** Compile and link a shader program (used internally). */
void generate_shader();

/** Set up the quad geometry and texture for the camera image plane. */
void init_image_plane(int textureWidth, int textureHeight);

/**
 * Main render function. Binds the framebuffer, computes view/projection
 * matrices, draws the camera image plane, and overlays the ImGui GUI.
 */
void render_scene(const XrCompositionLayerProjectionView &layerView, render_target_t &rtarget,
                  const Quad &quad, const std::shared_ptr<AppState> &appState,
                  const CameraFrame *image, bool drawSettingsGui,
                  const std::vector<GuiSetting> &settings);

/** Render a camera frame onto the image quad (GL texture or CPU upload). */
int draw_image_plane(const XrMatrix4x4f &vp, const Quad &quad, const CameraFrame *image);

/** Render the ImGui settings panel into an off-screen FBO and draw it in VR. */
int draw_imgui(const XrMatrix4x4f &vp, const std::shared_ptr<AppState> &appState,
               bool drawSettingsGui, const std::vector<GuiSetting> &settings);