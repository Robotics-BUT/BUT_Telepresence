#pragma once

#include "util_openxr.h"

int create_render_target (render_target_t *rtarget, int w, int h);
int destroy_render_target (render_target_t *rtarget);
int set_render_target (render_target_t *rtarget);
int get_render_target (render_target_t *rtarget);
int blit_render_target (render_target_t *rtarget_src, int x, int y, int w, int h);