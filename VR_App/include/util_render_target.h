/**
 * util_render_target.h - Framebuffer / render target management
 *
 * Wraps OpenGL ES FBO creation and switching. Used for both the main
 * eye swapchain render targets and the off-screen ImGui settings panel FBO.
 */
#pragma once

#include "util_openxr.h"

/** Create an FBO with color and depth textures at the given dimensions. */
int create_render_target (render_target_t *rtarget, int w, int h);

/** Delete the FBO and its textures. */
int destroy_render_target (render_target_t *rtarget);

/** Bind this render target as the active framebuffer. */
int set_render_target (render_target_t *rtarget);

/** Query the currently bound framebuffer into a render_target_t. */
int get_render_target (render_target_t *rtarget);

/** Blit (copy) the source render target's color to the current framebuffer. */
int blit_render_target (render_target_t *rtarget_src, int x, int y, int w, int h);