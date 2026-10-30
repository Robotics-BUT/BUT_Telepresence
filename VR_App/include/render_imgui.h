/* ------------------------------------------------ *
 * The MIT License (MIT)
 * Copyright (c) 2020 terryky1220@gmail.com
 * ------------------------------------------------ */
/**
 * render_imgui.h - ImGui VR settings panel
 */
#pragma once

#include "pch.h"
#include "types/app_state.h"
#include "types/gui_setting.h"
#include <string>
#define FMT_HEADER_ONLY
#include "fmt/core.h"

/** Create the ImGui context, the panel's fonts and its style. */
int init_imgui();

/** Draw the settings panel into the bound render target: win_w x win_h panel units, scale render-target pixels each. */
int invoke_imgui_settings(int win_w, int win_h, float scale, const std::shared_ptr<AppState>& appState,
                          const std::vector<GuiSetting>& settings);
