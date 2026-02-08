/**
 * util_egl.h - EGL context initialization for OpenXR rendering
 *
 * Creates an EGL context with a PBuffer surface (no on-screen window).
 * OpenXR renders to its own swapchain images, so a PBuffer is sufficient.
 * The EGL context is also shared with GStreamer for GL texture interop.
 */
#pragma once

/** Initialize EGL with a PBuffer surface suitable for OpenXR rendering. */
int egl_init_with_pbuffer_surface();

/** Accessors for the initialized EGL objects. */
EGLDisplay egl_get_display();
EGLContext egl_get_context();
EGLConfig  egl_get_config();
EGLSurface egl_get_surface();