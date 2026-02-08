/* ------------------------------------------------ *
 * The MIT License (MIT)
 * Copyright (c) 2020 terryky1220@gmail.com
 * ------------------------------------------------ */
#pragma once

#include "pch.h"
#include "types/app_state.h"
#include "types/gui_setting.h"
#include <string>
#define FMT_HEADER_ONLY
#include "fmt/core.h"

int init_imgui();

void imgui_mousebutton(int button, int state, int x, int y);

void imgui_mousemove(int x, int y);

void focusable_text(const std::string& text, bool isFocused = false);
void focusable_text_ip(const std::string& text, bool isFocused = false, int segment = 0);
void focusable_button(const std::string &label, bool isFocused);

int invoke_imgui_settings(int win_w, int win_h, const std::shared_ptr<AppState>& appState,
                          const std::vector<GuiSetting>& settings);
