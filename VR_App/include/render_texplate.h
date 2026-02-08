/**
 * render_texplate.h - Low-level textured quad rendering
 *
 * Renders a texture onto a simple quad with alpha blending.
 * Used to display the ImGui settings panel as a floating panel in VR space.
 */
#pragma once

#include "pch.h"
#include "linear.h"

/** Compile the texture plate shader. */
int init_texplate();

/** Draw a textured quad with the given model-view-projection matrix. */
int draw_tex_plate(int texid, const XrMatrix4x4f& matPVM);
