/**
 * util_openxr.h - OpenXR initialization, session management, and frame lifecycle
 *
 * Provides a procedural API wrapping the OpenXR C API. Covers the full
 * lifecycle: loader init, instance/system/session creation, reference spaces,
 * swapchain management, input actions, and frame begin/end.
 */
#pragma once

#include <GLES3/gl3.h>
#include <string>
#include "utils/string_utils.h"
#include "types/input_types.h"

/** Convert an OpenXR version number to a "major.minor.patch" string. */
inline std::string GetXrVersionString(XrVersion ver) {
    return Fmt("%d.%d.%d",
               XR_VERSION_MAJOR(ver),
               XR_VERSION_MINOR(ver),
               XR_VERSION_PATCH(ver));
}

/** OpenGL ES framebuffer render target (color + depth + FBO). */
struct render_target_t {
    GLuint texc_id; /* color texture */
    GLuint texz_id; /* depth texture */
    GLuint fbo_id;  /* framebuffer object */
    int width;
    int height;
};

/** A swapchain surface for one eye view, with its associated render targets. */
struct viewsurface_t {
    uint32_t width, height;
    int64_t format{0};                 /* GL internal format of the swapchain images */
    XrViewConfigurationView config_view;
    XrSwapchain swapchain;
    std::vector<render_target_t> render_targets;
};

/** OpenXR action handles for all tracked controller inputs. */
struct InputState {
    XrActionSet actionSet{XR_NULL_HANDLE};
    XrAction quitAction{XR_NULL_HANDLE};
    XrAction controllerPoseAction{XR_NULL_HANDLE};
    XrAction thumbstickPoseAction{XR_NULL_HANDLE};
    XrAction thumbstickPressedAction{XR_NULL_HANDLE};
    XrAction thumbstickTouchedAction{XR_NULL_HANDLE};
    XrAction buttonAPressedAction{XR_NULL_HANDLE};
    XrAction buttonATouchedAction{XR_NULL_HANDLE};
    XrAction buttonBPressedAction{XR_NULL_HANDLE};
    XrAction buttonBTouchedAction{XR_NULL_HANDLE};
    XrAction buttonXPressedAction{XR_NULL_HANDLE};
    XrAction buttonXTouchedAction{XR_NULL_HANDLE};
    XrAction buttonYPressedAction{XR_NULL_HANDLE};
    XrAction buttonYTouchedAction{XR_NULL_HANDLE};
    XrAction squeezeValueAction{XR_NULL_HANDLE};
    XrAction triggerValueAction{XR_NULL_HANDLE};
    XrAction triggerTouchedAction{XR_NULL_HANDLE};

    XrAction userPresenceAction{XR_NULL_HANDLE};

    std::array<XrPath, Side::COUNT> handSubactionPath;

    std::array<XrSpace, Side::COUNT> controllerSpace;
};

// =============================================================================
// Initialization
// =============================================================================

/** Initialize the OpenXR loader for Android. */
int openxr_init_loader(android_app *app);

void openxr_log_layers_and_extensions();

void openxr_create_instance(android_app *app, XrInstance *instance);

void openxr_get_system_id(XrInstance *instance, XrSystemId *system_id);

void openxr_create_session(XrInstance *instance, XrSystemId *system_id, XrSession *session);

void openxr_confirm_gfx_reqs(XrInstance *instance, XrSystemId *system_id);

void openxr_log_reference_spaces(XrSession *session);

XrReferenceSpaceCreateInfo openxr_get_reference_space_create_info(std::string reference_space);

void openxr_create_reference_spaces(XrSession *session, std::vector<XrSpace> &reference_spaces);

std::vector<XrViewConfigurationView>
openxr_enumerate_view_configurations(XrInstance *instance, XrSystemId *system_id);

void openxr_log_environment_blend_modes(XrInstance *instance, XrSystemId *systemId,
                                        XrViewConfigurationType type);

// =============================================================================
// Swapchain
// =============================================================================

std::vector<viewsurface_t>
openxr_create_swapchains(XrInstance *instance, XrSystemId *system_id, XrSession *session);

void openxr_allocate_swapchain_rendertargets(viewsurface_t &viewsurface);

int openxr_acquire_viewsurface(viewsurface_t &viewSurface, render_target_t &renderTarget,
                               XrSwapchainSubImage &subImage);

int openxr_release_viewsurface(viewsurface_t &viewsurface);

/** A single-sample swapchain for a quad layer (the settings panel): sRGB with alpha when the runtime offers it. */
viewsurface_t openxr_create_panel_swapchain(XrSession *session, uint32_t width, uint32_t height);

/** Whether XR_FB_composition_layer_settings is enabled (compositor super-sampling / sharpening for a layer). */
bool openxr_has_layer_settings();

int openxr_acquire_swapchain_img(XrSwapchain swapchain);

// =============================================================================
// Actions & Input
// =============================================================================

XrActionSet
openxr_create_actionset(XrInstance *instance, std::string name, std::string localized_name,
                        int priority);

XrAction openxr_create_action(XrActionSet *actionSet, XrActionType type, std::string name,
                              std::string localized_name, int subpath_number,
                              XrPath *subpath_array);

XrPath openxr_string2path(XrInstance *instance, std::string);

int openxr_bind_interaction(XrInstance *instance, std::string profile,
                            std::vector<XrActionSuggestedBinding> &bindings);

int openxr_attach_actionset(XrSession *session, XrActionSet actionSet);

XrSpace openxr_create_action_space(XrSession *session, XrAction action, XrPath path);

// =============================================================================
// Session & Frame Lifecycle
// =============================================================================

int openxr_begin_session(XrSession *session);

/** Request the display refresh rate: `debug.but.refresh_rate` (Hz) if set, else 120 Hz; logs the rates offered and the
 *  rate in force, and falls back to the highest offered rate below a rate the runtime rejects. */
void openxr_request_refresh_rate(XrInstance *instance, XrSession *session);

/** The display refresh rate in force (as the runtime last reported it) and the rate requested, in Hz (0 before the
 *  session begins). */
float openxr_current_refresh_rate();
float openxr_requested_refresh_rate();

int openxr_handle_session_state_changed(XrSession *session, XrEventDataSessionStateChanged &ev, bool *exitLoop, bool *reqRestart);

bool openxr_is_session_running();

static XrEventDataBaseHeader *openxr_poll_event(XrInstance *instance, XrSession *session);

int openxr_poll_events(XrInstance *instance, XrSession *session, bool *exit, bool *request_restart, bool *mounted);

int openxr_begin_frame(XrSession *session, XrTime *display_time);

int openxr_end_frame(XrSession *session, XrTime *displayTime,
                     std::vector<XrCompositionLayerBaseHeader *> &layers);

/** Returns 0 when the views are valid, -1 when the runtime flags them not valid (contents undefined). */
int openxr_locate_views(XrSession *session, XrTime *displayTime, XrSpace space, uint32_t viewCount, XrView *view_array);

std::string openxr_get_runtime_name(XrInstance *instance);

std::string openxr_get_system_name(XrInstance *instance, XrSystemId* system_id);

void openxr_has_user_presence_capability(XrInstance *instance, XrSystemId *system_id);
