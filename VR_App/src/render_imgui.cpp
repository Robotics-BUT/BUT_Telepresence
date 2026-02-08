/* ------------------------------------------------ *
 * The MIT License (MIT)
 * Copyright (c) 2020 terryky1220@gmail.com
 * ------------------------------------------------ */
/**
 * render_imgui.cpp - ImGui VR settings panel rendering
 *
 * Renders the in-VR settings GUI using Dear ImGui. The focus-based
 * navigation system (no mouse) processes queued input events from
 * HandleControllers() to move focus and highlights the active element.
 * Also displays connection status indicators and pipeline latency stats.
 */
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "render_imgui.h"
#include "openxr/openxr.h"
#include "utils/string_utils.h"

#define DISPLAY_SCALE_X 1.0f
#define DISPLAY_SCALE_Y 1.0f
#define _X(x)       ((float)(x) / DISPLAY_SCALE_X)
#define _Y(y)       ((float)(y) / DISPLAY_SCALE_Y)

static ImVec2 s_win_size[10];
static ImVec2 s_win_pos[10];
static int s_win_num = 0;
static ImVec2 s_mouse_pos;

int
init_imgui() {
    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer bindings
    ImGui_ImplOpenGL3_Init(NULL);

    return 0;
}

void
imgui_mousebutton(int button, int state, int x, int y) {
    ImGuiIO &io = ImGui::GetIO();
    io.MousePos = ImVec2(_X(x), (float) _Y(y));

    if (state)
        io.MouseDown[button] = true;
    else
        io.MouseDown[button] = false;

    s_mouse_pos.x = x;
    s_mouse_pos.y = y;
}

void
imgui_mousemove(int x, int y) {
    ImGuiIO &io = ImGui::GetIO();
    io.MousePos = ImVec2(_X(x), _Y(y));

    s_mouse_pos.x = x;
    s_mouse_pos.y = y;
}

/**
 * Render the full settings panel: process focus navigation events,
 * iterate over all GuiSettings, draw connection status, and show
 * averaged pipeline latency statistics.
 */
static void render_settings_gui(const std::shared_ptr<AppState> &appState,
                                const std::vector<GuiSetting> &settings) {
    int win_w = 300;
    int win_h = 0;
    int win_x = 0;
    int win_y = 0;

    int numberOfElements = static_cast<int>(settings.size());

    s_win_num = 0;

    if (appState->guiControl.changesEnqueued) {
        if (appState->guiControl.focusMoveUp) {
            appState->guiControl.focusedElement -= 1;
            appState->guiControl.focusedSegment = 0;
            if (appState->guiControl.focusedElement < 0) {
                appState->guiControl.focusedElement = numberOfElements - 1;
            }
        }
        if (appState->guiControl.focusMoveDown) {
            appState->guiControl.focusedElement += 1;
            appState->guiControl.focusedSegment = 0;
            if (appState->guiControl.focusedElement >= numberOfElements) {
                appState->guiControl.focusedElement = 0;
            }
        }
        if (appState->guiControl.focusMoveLeft) {
            int maxSegments = settings[appState->guiControl.focusedElement].segments;
            appState->guiControl.focusedSegment -= 1;
            if (appState->guiControl.focusedSegment < 0) {
                appState->guiControl.focusedSegment = maxSegments - 1;
            }
        }
        if (appState->guiControl.focusMoveRight) {
            int maxSegments = settings[appState->guiControl.focusedElement].segments;
            appState->guiControl.focusedSegment += 1;
            if (appState->guiControl.focusedSegment >= maxSegments) {
                appState->guiControl.focusedSegment = 0;
            }
        }
        appState->guiControl.focusMoveUp = false;
        appState->guiControl.focusMoveDown = false;
        appState->guiControl.focusMoveLeft = false;
        appState->guiControl.focusMoveRight = false;
        appState->guiControl.cooldown = 20;
        appState->guiControl.changesEnqueued = false;
    }

    win_y += win_h;
    win_h = 560;
    ImGui::SetNextWindowPos(ImVec2(_X(win_x), _Y(win_y)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(_X(win_w), _Y(win_h)), ImGuiCond_FirstUseEver);
    ImGui::Begin("Settings");
    {
        for (int i = 0; i < numberOfElements; i++) {
            auto& s = settings[i];
            bool focused = (appState->guiControl.focusedElement == i);

            if (!s.sectionHeader.empty()) {
                ImGui::SeparatorText(s.sectionHeader.c_str());
            }

            switch (s.type) {
                case GuiSettingType::IpAddress:
                    focusable_text_ip(s.getDisplayText(), focused, appState->guiControl.focusedSegment);
                    break;
                case GuiSettingType::Button:
                    focusable_button(s.label, focused);
                    break;
                case GuiSettingType::Text:
                    focusable_text(s.getDisplayText(), focused);
                    break;
            }
        }

        ImGui::Text("Robot control: %s", BoolToString(appState->robotControlEnabled));

        ImGui::SeparatorText("Connection Status");
        {
            ImVec4 color = (appState->cameraServerStatus == "Connected")
                ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f)
                : (appState->cameraServerStatus == "Connecting...")
                    ? ImVec4(1.0f, 1.0f, 0.0f, 1.0f)
                    : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
            ImGui::TextColored(color, "Camera Server: %s", appState->cameraServerStatus.c_str());
        }
        {
            ImVec4 color = (appState->robotControlStatus == "Connected")
                ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f)
                : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
            ImGui::TextColored(color, "Robot Control: %s", appState->robotControlStatus.c_str());
        }
        {
            ImVec4 color = (appState->ntpSyncStatus == "Synced")
                ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f)
                : ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
            ImGui::TextColored(color, "NTP Time Sync: %s", appState->ntpSyncStatus.c_str());
        }

        ImGui::Text("");
        ImGui::Text("Latencies (avg last 50 frames):");
        auto s = appState->cameraStreamingStates.first.stats;
        if (s) {
            auto snapshot = s->averagedSnapshot();

            uint16_t cameraMs = snapshot.camera / 1000;
            uint16_t vidConvMs = snapshot.vidConv / 1000;
            uint16_t encMs = snapshot.enc / 1000;
            uint16_t rtpPayMs = snapshot.rtpPay / 1000;
            uint16_t udpStreamMs = snapshot.udpStream / 1000;
            uint16_t rtpDepayMs = snapshot.rtpDepay / 1000;
            uint16_t decMs = snapshot.dec / 1000;
            uint16_t queueMs = snapshot.queue / 1000;
            uint16_t displayMs = snapshot.presentation / 1000;

            ImGui::Text(
                    "camera: %u vidConv: %u enc: %u\nrtpPay: %u udpStream: %u rtpDepay: %u\ndec: %u queue: %u display: %u",
                    cameraMs, vidConvMs, encMs, rtpPayMs, udpStreamMs, rtpDepayMs, decMs, queueMs,
                    displayMs);
            ImGui::Text("In Total: %u: \n", cameraMs + vidConvMs + encMs + rtpPayMs + udpStreamMs +
                                             rtpDepayMs + decMs + queueMs + displayMs);
        }

        s_win_pos[s_win_num] = ImGui::GetWindowPos();
        s_win_size[s_win_num] = ImGui::GetWindowSize();
        s_win_num++;
    }

    ImGui::End();
}

void focusable_text(const std::string &text, bool isFocused) {
    ImVec2 p = ImGui::GetCursorScreenPos(); // Top-left position of the current drawing cursor
    ImVec2 textSize = ImGui::CalcTextSize(text.c_str()); // Size of the text

    // Draw a background rectangle if isFocused is true
    if (isFocused) {
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(
                p,
                ImVec2(p.x + textSize.x, p.y + textSize.y),
                IM_COL32(100, 100, 255, 100) // Light blue color with transparency
        );
    }

    // Draw the text
    ImGui::Text("%s", text.c_str());
}

void focusable_text_ip(const std::string &text, bool isFocused, int segment) {
    ImVec2 p = ImGui::GetCursorScreenPos(); // Top-left position of the current drawing cursor

    // Split the text into 4 IP segments
    size_t start = text.find_first_of("0123456789");
    for (int i = 0; i < segment && start != std::string::npos; ++i) {
        start = text.find_first_of("0123456789", text.find_first_not_of("0123456789", start));
    }
    size_t end = text.find_first_not_of("0123456789", start);

    // Draw highlight if focused
    if (isFocused && start != std::string::npos) {
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        ImVec2 highlightStart = ImVec2(p.x + ImGui::CalcTextSize(text.substr(0, start).c_str()).x,
                                       p.y);
        ImVec2 highlightSize = ImGui::CalcTextSize(text.substr(start, end - start).c_str());
        drawList->AddRectFilled(highlightStart, ImVec2(highlightStart.x + highlightSize.x,
                                                       highlightStart.y + highlightSize.y),
                                IM_COL32(100, 100, 255, 100));
    }

    // Render the full text
    ImGui::Text("%s", text.c_str());
}

void focusable_button(const std::string &label, bool isFocused) {
    if (isFocused) {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(100, 100, 255, 100));  // Light blue
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              IM_COL32(100, 100, 255, 20));  // Transparent light blue
    }

    ImGui::Button(label.c_str());

    ImGui::PopStyleColor(1);
}

int
invoke_imgui_settings(int win_w, int win_h, const std::shared_ptr<AppState> &appState,
                      const std::vector<GuiSetting> &settings) {
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(_X(win_w), _Y(win_h));
    io.DisplayFramebufferScale = {DISPLAY_SCALE_X, DISPLAY_SCALE_Y};

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    render_settings_gui(appState, settings);

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    return 0;
}