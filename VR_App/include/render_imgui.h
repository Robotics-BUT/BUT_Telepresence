/* ------------------------------------------------ *
 * The MIT License (MIT)
 * Copyright (c) 2020 terryky1220@gmail.com
 * ------------------------------------------------ */
/**
 * render_imgui.h - VR-adapted ImGui rendering with focus-based navigation
 *
 * Since there is no mouse cursor in VR, the settings GUI uses a focus
 * system: the currently selected setting is highlighted, and the user
 * navigates with the left thumbstick. Each rendering function has a
 * "focused" variant that draws a highlight rectangle behind the element.
 */
#pragma once

#include "pch.h"
#include "types/app_state.h"
#include "types/gui_setting.h"
#include <string>
#define FMT_HEADER_ONLY
#include "fmt/core.h"

/** Initialize Dear ImGui context with dark style and OpenGL3 backend. */
int init_imgui();

/** Forward a mouse button event to ImGui (used for debugging, not in VR). */
void imgui_mousebutton(int button, int state, int x, int y);

/** Forward a mouse move event to ImGui (used for debugging, not in VR). */
void imgui_mousemove(int x, int y);

/** Render a text label with optional focus highlight background. */
void focusable_text(const std::string& text, bool isFocused = false);

/** Render an IP address with per-segment focus highlighting. */
void focusable_text_ip(const std::string& text, bool isFocused = false, int segment = 0);

/** Render a button with focus-dependent background color. */
void focusable_button(const std::string &label, bool isFocused);

/** Main GUI entry point: set up ImGui frame, render settings, finalize. */
int invoke_imgui_settings(int win_w, int win_h, const std::shared_ptr<AppState>& appState,
                          const std::vector<GuiSetting>& settings);
