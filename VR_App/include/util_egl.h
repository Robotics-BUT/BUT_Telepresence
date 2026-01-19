#pragma once

int egl_init_with_pbuffer_surface();

EGLDisplay egl_get_display();
EGLContext egl_get_context();
EGLConfig  egl_get_config();
EGLSurface egl_get_surface();