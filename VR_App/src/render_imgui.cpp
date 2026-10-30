/* ------------------------------------------------ *
 * The MIT License (MIT)
 * Copyright (c) 2020 terryky1220@gmail.com
 * ------------------------------------------------ */
/**
 * render_imgui.cpp - ImGui VR settings panel rendering
 *
 * Draws the in-VR settings panel: a status header (connections, display rate, latency), tabs, the rows of the open
 * tab (settings_table.cpp) and a line of controller hints. A row is operated with the right controller's ray, which
 * arrives as ImGui mouse input, or, when highlighted, with the left thumbstick and X / Y (HandleControllers). Presses
 * are queued in GUIControl and run by HandleControllers on the next frame, outside the drawing. All sizes are in
 * panel units (GuiPanelGeometry); the target holds renderScale pixels per unit. A wide panel (the Below layout) puts
 * the header on one line and the rows in two columns, with the latency bars beside the System rows.
 *
 * The status bar over the video (invoke_imgui_status_bar) is drawn with an ImGui context of its own, so drawing it
 * between two panel frames cannot end a press on the panel.
 */
#include <cfloat>
#include <cmath>
#include <algorithm>
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "render_imgui.h"
#include "openxr/openxr.h"
#include "util_openxr.h"
#include "fonts/open_sans_regular.h"
#include "fonts/open_sans_semibold.h"

namespace {

// Type sizes and geometry
constexpr float kPad = 30.0f;          // panel margin
constexpr float kBody = 30.0f;         // row labels and values
constexpr float kSmall = 23.0f;        // status, hints, captions
constexpr float kTitle = 31.0f;
constexpr float kFigure = 60.0f;       // the latency figure
constexpr float kRowH = 80.0f;
constexpr float kReadoutH = 60.0f;     // rows that only show a value
constexpr float kCtl = 58.0f;          // height of a control inside a row
constexpr float kRound = 14.0f;
constexpr float kInset = 18.0f;        // label and control inset inside a row
constexpr float kColumnGap = 40.0f;    // between the two columns of a wide panel

// Palette (sRGB)
constexpr uint32_t kBg = 0x111418, kSurface = 0x1C2129, kSurfaceHi = 0x283039, kLine = 0x2B323C;
constexpr uint32_t kText = 0xECEFF3, kMuted = 0x8D97A4, kAccent = 0x3D8BFF, kAccentHi = 0x62A2FF;
constexpr uint32_t kGood = 0x36C67C, kWarn = 0xF2B53A, kBad = 0xFF5F5F;

ImFont *gRegular = nullptr, *gSemibold = nullptr;

// An ImGui context with its own fonts and renderer backend
struct Ui { ImGuiContext *ctx = nullptr; ImFont *regular = nullptr, *semibold = nullptr; };
Ui gPanelUi, gStatusUi;

void useUi(const Ui &ui) {
    ImGui::SetCurrentContext(ui.ctx);
    gRegular = ui.regular;
    gSemibold = ui.semibold;
}

ImVec4 rgba(uint32_t rgb, float a = 1.0f) {
    return ImVec4(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a);
}
ImU32 col(uint32_t rgb, float a = 1.0f) { return ImGui::ColorConvertFloat4ToU32(rgba(rgb, a)); }
ImVec2 operator+(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }

ImDrawList *dl() { return ImGui::GetWindowDrawList(); }

ImVec2 textSize(ImFont *font, float size, const std::string &s) {
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, s.c_str());
}

void text(ImFont *font, float size, ImVec2 pos, uint32_t color, const std::string &s, float alpha = 1.0f) {
    dl()->AddText(font, size, pos, col(color, alpha), s.c_str());
}

// Text centred in a rectangle
void textIn(ImFont *font, float size, ImVec2 min, ImVec2 max, uint32_t color, const std::string &s, float alpha = 1.0f) {
    ImVec2 t = textSize(font, size, s);
    text(font, size, ImVec2(std::floor((min.x + max.x - t.x) * 0.5f), std::floor((min.y + max.y - t.y) * 0.5f)), color, s, alpha);
}

// A clickable rectangle; returns true when pressed (repeating while held if repeat)
bool hit(const char *id, ImVec2 min, ImVec2 max, bool *hovered, bool *held, bool repeat = false) {
    ImGui::SetCursorScreenPos(min);
    if (repeat) ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    bool pressed = ImGui::InvisibleButton(id, ImVec2(max.x - min.x, max.y - min.y));
    if (repeat) ImGui::PopItemFlag();
    *hovered = ImGui::IsItemHovered();
    *held = ImGui::IsItemActive();
    return pressed;
}

void queue(AppState &app, int row, GuiAction action, int value = 0) {
    app.guiControl.focusedElement = row;
    app.guiControl.pendingRow = row;
    app.guiControl.pendingAction = action;
    app.guiControl.pendingValue = value;
}

// A square key with a minus or plus drawn as strokes
bool stepKey(const char *id, ImVec2 min, bool plus) {
    ImVec2 max = min + ImVec2(kCtl, kCtl);
    bool hov, held;
    bool pressed = hit(id, min, max, &hov, &held, true);
    dl()->AddRectFilled(min, max, col(held ? kAccent : hov ? kSurfaceHi : kSurface), kRound);
    ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const float r = 11.0f, w = 3.5f;
    dl()->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col(kText), w);
    if (plus) dl()->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), col(kText), w);
    return pressed;
}

int focusableRows(const std::vector<GuiSetting> &settings, int tab, std::vector<int> &out) {
    out.clear();
    for (int i = 0; i < (int) settings.size(); i++) {
        const auto &s = settings[i];
        if ((int) s.tab == tab && (!s.isVisible || s.isVisible())) out.push_back(i);
    }
    return (int) out.size();
}

// Left thumbstick: up/down move the highlight within the open tab; left/right switch tab, or octet on an IP row
void navigate(AppState &app, const std::vector<GuiSetting> &settings) {
    auto &gc = app.guiControl;
    std::vector<int> rows;
    focusableRows(settings, gc.tab, rows);
    auto pos = std::find(rows.begin(), rows.end(), gc.focusedElement);
    int at = pos == rows.end() ? -1 : (int) (pos - rows.begin());
    if (gc.changesEnqueued) {
        const bool onIp = at >= 0 && settings[rows[at]].control == GuiControl::IpAddress;
        if ((gc.focusMoveUp || gc.focusMoveDown) && !rows.empty()) {
            at = at < 0 ? 0 : (at + (gc.focusMoveDown ? 1 : (int) rows.size() - 1)) % (int) rows.size();
            gc.focusedSegment = 0;
        } else if (gc.focusMoveLeft || gc.focusMoveRight) {
            const int d = gc.focusMoveRight ? 1 : -1;
            if (onIp) {
                const int n = settings[rows[at]].segments;
                gc.focusedSegment = (gc.focusedSegment + d + n) % n;
            } else {
                const int n = (int) GuiTab::Count;
                gc.tab = (gc.tab + d + n) % n;
                focusableRows(settings, gc.tab, rows);
                at = 0;
                gc.focusedSegment = 0;
            }
        }
        gc.focusMoveUp = gc.focusMoveDown = gc.focusMoveLeft = gc.focusMoveRight = false;
        gc.cooldown = 20;
        gc.changesEnqueued = false;
    }
    if (at < 0) at = 0;
    gc.focusedElement = rows.empty() ? -1 : rows[at];
}

uint32_t statusColor(const std::string &s, const char *good, const char *pending = nullptr) {
    if (s == good) return kGood;
    if (pending && s.rfind(pending, 0) == 0) return kWarn;
    return kBad;
}

// A status item: coloured dot and its name; returns the x after it
float chip(float x, float y, uint32_t dot, const std::string &label) {
    const float r = 7.0f;
    dl()->AddCircleFilled(ImVec2(x + r, y + kSmall * 0.55f), r, col(dot));
    text(gRegular, kSmall, ImVec2(x + 2 * r + 9, y), kText, label, 0.9f);
    return x + 2 * r + 9 + textSize(gRegular, kSmall, label).x + 26;
}

struct Stage { const char *name; uint64_t us; };

// The title over the status items, or beside them on one line (wide panel)
void drawHeader(AppState &app, float x0, float x1, float y, uint64_t totalUs, bool oneLine) {
    text(gSemibold, kTitle, ImVec2(x0, oneLine ? y + 8 : y), kText, "BUT Telepresence");
    // the latency figure, right-aligned over the two header lines
    std::string figure = totalUs ? fmt::format("{}", (int) std::lround(totalUs / 1000.0)) : "–";
    ImVec2 fs = textSize(gSemibold, kFigure, figure), us = textSize(gRegular, kSmall, "ms");
    const float fx = x1 - us.x - 8 - fs.x;
    text(gSemibold, kFigure, ImVec2(fx, y - 12), kText, figure);
    text(gRegular, kSmall, ImVec2(x1 - us.x, y + 18), kMuted, "ms");
    ImVec2 cs = textSize(gRegular, kSmall, "latency");
    text(gRegular, kSmall, ImVec2(x1 - cs.x, y + 50), kMuted, "latency");

    float cx = x0, cy = y + 52;
    if (oneLine) {
        cx = x0 + textSize(gSemibold, kTitle, "BUT Telepresence").x + 44;
        cy = y + 14;
    }
    cx = chip(cx, cy, statusColor(app.cameraServerStatus, "Connected", "Connecting"), "Video");
    cx = chip(cx, cy, statusColor(app.robotControlStatus, "Connected", "Connecting"), "Robot");
    cx = chip(cx, cy, statusColor(app.ntpSyncStatus, "Synced", "Not Synced"), "Clock");
    const float hz = openxr_current_refresh_rate(), req = openxr_requested_refresh_rate();
    cx = chip(cx, cy, hz + 0.5f >= req ? kGood : kWarn, fmt::format("{:.0f} Hz", hz));
    if (app.robotControlEnabled) cx = chip(cx, cy, kAccent, "Driving");   // the base follows the right thumbstick
    if (app.keepRunningOffHead) chip(cx, cy, kWarn, "Stays on");         // the display stays on off-head
}

void drawTabs(AppState &app, float x0, float x1, float y) {
    static const char *names[] = {"Stream", "Camera", "Audio", "System"};
    const int n = (int) GuiTab::Count;
    const float h = 64.0f, w = (x1 - x0) / n;
    dl()->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + h), col(kSurface), kRound);
    for (int i = 0; i < n; i++) {
        ImVec2 min(x0 + i * w, y), max(x0 + (i + 1) * w, y + h);
        bool hov, held;
        ImGui::PushID(1000 + i);
        if (hit("tab", min, max, &hov, &held) && app.guiControl.tab != i) {
            app.guiControl.tab = i;
            app.guiControl.focusedElement = -1;   // navigate() picks the first row of the tab
            app.guiControl.focusedSegment = 0;
        }
        ImGui::PopID();
        const bool on = app.guiControl.tab == i;
        if (on) dl()->AddRectFilled(min + ImVec2(5, 5), max + ImVec2(-5, -5), col(kSurfaceHi), kRound - 4);
        else if (hov) dl()->AddRectFilled(min + ImVec2(5, 5), max + ImVec2(-5, -5), col(kSurfaceHi, 0.5f), kRound - 4);
        textIn(on ? gSemibold : gRegular, kBody - 2, min, max, on ? kText : kMuted, names[i]);
        if (on) dl()->AddRectFilled(ImVec2(min.x + w * 0.3f, max.y - 9), ImVec2(max.x - w * 0.3f, max.y - 5), col(kAccent), 2.0f);
    }
}

// One row; returns its height
float drawRow(AppState &app, const GuiSetting &s, int index, float x0, float x1, float y) {
    auto &gc = app.guiControl;
    const bool focused = gc.focusedElement == index;
    ImGui::PushID(index);

    if (s.control == GuiControl::Button) {
        const bool enabled = !s.isEnabled || s.isEnabled();
        ImVec2 min(x0, y + 10), max(x1, y + kRowH + 4);
        bool hov = false, held = false;
        if (hit("button", min, max, &hov, &held) && enabled) queue(app, index, GuiAction::Activate);
        uint32_t fill = !enabled ? kSurface : held ? kAccentHi : kAccent;
        dl()->AddRectFilled(min, max, col(fill, enabled && hov && !held ? 0.9f : 1.0f), kRound);
        if (focused) dl()->AddRect(min + ImVec2(-4, -4), max + ImVec2(4, 4), col(kAccentHi, 0.8f), kRound + 3, 0, 3.0f);
        textIn(gSemibold, kBody, min, max, enabled ? 0xFFFFFF : kMuted, s.getValue ? s.getValue() : s.label);
        ImGui::PopID();
        return kRowH + 20;
    }

    const float rowH = s.control == GuiControl::Readout ? kReadoutH : kRowH;
    ImVec2 rmin(x0, y), rmax(x1, y + rowH);
    const bool rowHover = ImGui::IsMouseHoveringRect(rmin, rmax, false);
    if (focused) {
        dl()->AddRectFilled(rmin, rmax, col(kSurface), kRound);
        dl()->AddRect(rmin, rmax, col(kAccent, 0.75f), kRound, 0, 2.5f);
    } else if (rowHover) {
        dl()->AddRectFilled(rmin, rmax, col(kSurface, 0.6f), kRound);
    }
    const float cy = y + rowH * 0.5f;
    const float ctlTop = cy - kCtl * 0.5f;
    ImVec2 ls = textSize(gRegular, kBody, s.label);
    text(gRegular, kBody, ImVec2(x0 + kInset, std::floor(cy - ls.y * 0.5f)), kText, s.label);
    if (s.isPending && s.isPending())                            // changed, not applied yet
        dl()->AddCircleFilled(ImVec2(x0 + kInset + ls.x + 16, cy), 6.0f, col(kAccent));
    const float right = x1 - kInset * 0.6f;

    switch (s.control) {
        case GuiControl::Stepper: {
            const float valueW = 190.0f;
            const float xPlus = right - kCtl, xVal = xPlus - 8 - valueW, xMinus = xVal - 8 - kCtl;
            if (stepKey("dec", ImVec2(xMinus, ctlTop), false)) queue(app, index, GuiAction::Decrement);
            if (stepKey("inc", ImVec2(xPlus, ctlTop), true)) queue(app, index, GuiAction::Increment);
            textIn(gSemibold, kBody, ImVec2(xVal, ctlTop), ImVec2(xVal + valueW, ctlTop + kCtl), kText, s.getValue());
            break;
        }
        case GuiControl::Choice: {
            const int n = (int) s.choices.size(), sel = s.getChoice();
            float segW = 0;
            for (auto &c : s.choices) segW = std::max(segW, textSize(gRegular, kSmall + 3, c).x + 34);
            const float total = segW * n, xs = right - total;
            dl()->AddRectFilled(ImVec2(xs, ctlTop), ImVec2(right, ctlTop + kCtl), col(kSurface), kRound);
            for (int i = 0; i < n; i++) {
                ImVec2 min(xs + i * segW, ctlTop), max(xs + (i + 1) * segW, ctlTop + kCtl);
                bool hov, held;
                ImGui::PushID(i);
                if (hit("opt", min, max, &hov, &held) && i != sel) queue(app, index, GuiAction::Choose, i);
                ImGui::PopID();
                if (i == sel) dl()->AddRectFilled(min + ImVec2(4, 4), max + ImVec2(-4, -4), col(kAccent), kRound - 4);
                else if (hov) dl()->AddRectFilled(min + ImVec2(4, 4), max + ImVec2(-4, -4), col(kSurfaceHi), kRound - 4);
                textIn(i == sel ? gSemibold : gRegular, kSmall + 3, min, max, i == sel ? 0xFFFFFF : kText, s.choices[i]);
            }
            break;
        }
        case GuiControl::Toggle: {
            const bool on = s.getToggle();
            const float w = 104.0f, h = 56.0f, ty = cy - h * 0.5f;
            ImVec2 min(right - w, ty), max(right, ty + h);
            bool hov, held;
            if (hit("switch", ImVec2(min.x - 30, ctlTop), ImVec2(max.x, ctlTop + kCtl), &hov, &held))
                queue(app, index, GuiAction::Toggle, on ? 0 : 1);
            dl()->AddRectFilled(min, max, col(on ? kAccent : kSurfaceHi, hov && !on ? 0.85f : 1.0f), h * 0.5f);
            const float r = h * 0.5f - 6;
            dl()->AddCircleFilled(ImVec2(on ? max.x - h * 0.5f : min.x + h * 0.5f, cy), r, col(0xFFFFFF, on ? 1.0f : 0.85f), 32);
            break;
        }
        case GuiControl::IpAddress: {
            std::vector<std::string> parts;
            std::string v = s.getValue(), cur;
            for (char c : v) { if (c == '.') { parts.push_back(cur); cur.clear(); } else cur += c; }
            parts.push_back(cur);
            const float boxW = 76.0f, dot = 14.0f;
            const float xPlus = right - kCtl, xMinus = xPlus - 8 - kCtl;
            float x = xMinus - 16 - (4 * boxW + 3 * dot);
            for (int i = 0; i < (int) parts.size() && i < 4; i++) {
                ImVec2 min(x, ctlTop), max(x + boxW, ctlTop + kCtl);
                bool hov, held;
                ImGui::PushID(i);
                if (hit("octet", min, max, &hov, &held)) { gc.focusedElement = index; gc.focusedSegment = i; }
                ImGui::PopID();
                const bool sel = focused && gc.focusedSegment == i;
                dl()->AddRectFilled(min, max, col(sel ? kSurfaceHi : kSurface), kRound - 4);
                if (sel) dl()->AddRect(min, max, col(kAccent), kRound - 4, 0, 2.5f);
                textIn(gSemibold, kSmall + 3, min, max, kText, parts[i]);
                x += boxW;
                if (i < 3) { textIn(gSemibold, kSmall + 3, ImVec2(x, ctlTop), ImVec2(x + dot, ctlTop + kCtl), kMuted, "."); x += dot; }
            }
            if (stepKey("dec", ImVec2(xMinus, ctlTop), false)) queue(app, index, GuiAction::Decrement);
            if (stepKey("inc", ImVec2(xPlus, ctlTop), true)) queue(app, index, GuiAction::Increment);
            break;
        }
        case GuiControl::Readout: {
            std::string v = s.getValue();
            ImVec2 vs = textSize(gRegular, kBody, v);
            text(gRegular, kBody, ImVec2(right - vs.x, std::floor(cy - vs.y * 0.5f)), kMuted, v);
            break;
        }
        default:
            break;
    }
    ImGui::PopID();
    return rowH + 6;
}

void drawLatency(const CameraStatsSnapshot &s, float x0, float x1, float y) {
    const Stage stages[] = {{"Camera (model)", s.camera}, {"Convert", s.vidConv}, {"Encode", s.enc}, {"Packetize", s.rtpPay},
                            {"Network", s.udpStream}, {"Jitter buffer", s.jbHold}, {"Depacketize", s.rtpDepay},
                            {"Decode", s.dec}, {"Hand-off", s.appsink}, {"Display", s.presentation}};
    uint64_t total = 0, peak = 1;
    for (auto &st : stages) { total += st.us; peak = std::max(peak, st.us); }
    text(gSemibold, kBody - 2, ImVec2(x0, y), kText, "Latency by stage");
    std::string t = fmt::format("{:.1f} ms", total / 1000.0);
    text(gSemibold, kBody - 2, ImVec2(x1 - textSize(gSemibold, kBody - 2, t).x, y), kText, t);
    y += 44;
    const float nameW = 200.0f, valW = 90.0f, barX = x0 + nameW, barW = x1 - valW - barX - 12;
    for (auto &st : stages) {
        text(gRegular, kSmall, ImVec2(x0, y), kMuted, st.name);
        const float w = std::max(3.0f, barW * (float) st.us / (float) peak);
        dl()->AddRectFilled(ImVec2(barX, y + 7), ImVec2(barX + barW, y + 21), col(kSurface), 7.0f);
        dl()->AddRectFilled(ImVec2(barX, y + 7), ImVec2(barX + w, y + 21), col(kAccent, 0.85f), 7.0f);
        std::string v = fmt::format("{:.1f}", st.us / 1000.0);
        text(gRegular, kSmall, ImVec2(x1 - textSize(gRegular, kSmall, v).x, y), kText, v);
        y += 31;
    }
}

void drawHints(float x0, float x1, float y) {
    const std::pair<const char *, const char *> hints[] = {{"Trigger", "select"}, {"Grip", "move"},
                                                           {"Left stick", "navigate"}, {"X / Y", "adjust"}};
    float total = 0;
    for (auto &h : hints) total += textSize(gSemibold, kSmall, h.first).x + 8 + textSize(gRegular, kSmall, h.second).x + 28;
    float x = std::floor(x0 + (x1 - x0 - (total - 28)) * 0.5f);
    for (auto &h : hints) {
        text(gSemibold, kSmall, ImVec2(x, y), kText, h.first, 0.85f);
        x += textSize(gSemibold, kSmall, h.first).x + 8;
        text(gRegular, kSmall, ImVec2(x, y), kMuted, h.second);
        x += textSize(gRegular, kSmall, h.second).x + 28;
    }
}

void render_settings_gui(const std::shared_ptr<AppState> &appState, const std::vector<GuiSetting> &settings) {
    AppState &app = *appState;
    navigate(app, settings);

    ImGuiIO &io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(W, H), ImGuiCond_Always);
    ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    const float x0 = kPad, x1 = W - kPad;

    CameraStatsSnapshot snap{};
    uint64_t total = 0;
    if (auto *st = app.cameraStreamingStates.first.stats) {
        snap = st->averagedSnapshot();
        total = snap.camera + snap.vidConv + snap.enc + snap.rtpPay + snap.udpStream + snap.jbHold + snap.rtpDepay +
                snap.dec + snap.appsink + snap.presentation;
    }
    const bool wide = W > H;
    drawHeader(app, x0, x1, kPad, total, wide);
    float y = kPad + (wide ? 92 : 104);
    drawTabs(app, x0, x1, y);
    y += 64 + 22;

    std::vector<int> rows;
    focusableRows(settings, app.guiControl.tab, rows);
    const bool system = app.guiControl.tab == (int) GuiTab::System;
    if (wide) {
        // Two columns, filled top to bottom; on the System tab the rows take the left one and the latency the right
        const float colW = (x1 - x0 - kColumnGap) * 0.5f, top = y;
        const int perColumn = system ? (int) rows.size() : ((int) rows.size() + 1) / 2;
        for (int k = 0; k < (int) rows.size(); k++) {
            if (k == perColumn) y = top;
            const float cx = k < perColumn ? x0 : x0 + colW + kColumnGap;
            y += drawRow(app, settings[rows[k]], rows[k], cx, cx + colW, y);
        }
        if (system) {
            const float lx = x0 + colW + kColumnGap;
            dl()->AddLine(ImVec2(lx - kColumnGap * 0.5f, top), ImVec2(lx - kColumnGap * 0.5f, top + 340), col(kLine), 2.0f);
            drawLatency(snap, lx + kInset * 0.5f, x1 - kInset * 0.5f, top + 4);
        }
    } else {
        for (int i : rows) y += drawRow(app, settings[i], i, x0, x1, y);
        if (system) {
            y += 18;
            dl()->AddLine(ImVec2(x0, y), ImVec2(x1, y), col(kLine), 2.0f);
            drawLatency(snap, x0 + kInset * 0.5f, x1 - kInset * 0.5f, y + 24);
        }
    }

    dl()->AddLine(ImVec2(x0, H - kPad - 52), ImVec2(x1, H - kPad - 52), col(kLine), 2.0f);
    drawHints(x0, x1, H - kPad - 30);

    // The controller ray ends behind the panel's layer: show where it points
    if (app.guiPanel.rayHitting) {
        auto *fg = ImGui::GetForegroundDrawList();
        fg->AddCircleFilled(io.MousePos, 10.0f, col(0xFFFFFF, app.guiPanel.triggerDown ? 1.0f : 0.9f), 24);
        fg->AddCircle(io.MousePos, 13.0f, col(kAccent), 24, 3.0f);
    }
    ImGui::End();
}

// --- the status bar over the video

struct BarItem { uint32_t dot; std::string label; bool strong; };

void render_status_bar(const AppState &app) {
    const StatusBarState &bar = app.statusBar;
    ImGuiIO &io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(W, H), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("Status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs);

    // Always: the latency and the video; the rest only while it needs the operator's attention
    std::vector<BarItem> items;
    items.push_back(bar.video ? BarItem{kGood, fmt::format("{:.0f} fps", bar.fps), false}
                              : BarItem{kBad, "No video", true});
    if (bar.video && bar.lossPerS >= 0.5) items.push_back({kWarn, fmt::format("Loss {:.0f}/s", bar.lossPerS), false});
    if (app.ntpSyncStatus != "Synced") items.push_back({app.ntpSyncStatus == "Not Synced" ? kWarn : kBad, "Clock " + app.ntpSyncStatus, false});
    const auto &servo = bar.servo;
    if (bar.relaySilent) items.push_back({kWarn, "Robot relay silent", true});
    else if (servo.known && !servo.answering) items.push_back({kBad, "Camera head not answering", true});
    else if (servo.known && servo.fault)
        items.push_back({kBad, "Camera head stopped: " + (servo.description.empty() ? std::string("fault") : servo.description) +
                               (servo.canReset ? ", reset in Camera tab" : ""), true});
    if (app.robotControlEnabled) items.push_back({kAccent, "Driving", false});
    if (app.audioMicEnable && !app.micMuted && !app.micPushToTalk) items.push_back({kWarn, "Mic on", false});
    const float hz = openxr_current_refresh_rate(), req = openxr_requested_refresh_rate();
    if (hz > 0 && hz + 0.5f < req) items.push_back({kWarn, fmt::format("{:.0f} Hz", hz), false});

    // Measure: the latency figure, then each item as dot + label
    const float pad = 26, gap = 30, dotR = 7, fig = kBody + 6;
    const std::string figure = bar.video && bar.latencyMs > 0 ? fmt::format("{:.0f}", bar.latencyMs) : "–";
    const float figW = textSize(gSemibold, fig, figure).x, msW = textSize(gRegular, kSmall, "ms").x;
    float total = pad + figW + 6 + msW;
    for (auto &it : items) total += gap + 2 * dotR + 10 + textSize(it.strong ? gSemibold : gRegular, kBody - 4, it.label).x;
    total += pad;

    const float h = 62, x0 = std::floor((W - total) * 0.5f), y0 = std::floor((H - h) * 0.5f), cy = y0 + h * 0.5f;
    dl()->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + total, y0 + h), col(kBg, 0.82f), h * 0.5f);
    float x = x0 + pad;
    const ImVec2 fs = textSize(gSemibold, fig, figure);
    text(gSemibold, fig, ImVec2(x, std::floor(cy - fs.y * 0.5f)), kText, figure);
    x += figW + 6;
    text(gRegular, kSmall, ImVec2(x, std::floor(cy - fs.y * 0.5f) + fs.y - textSize(gRegular, kSmall, "ms").y - 3), kMuted, "ms");
    x += msW;
    for (auto &it : items) {
        x += gap;
        dl()->AddCircleFilled(ImVec2(x + dotR, cy), dotR, col(it.dot), 20);
        x += 2 * dotR + 10;
        ImFont *f = it.strong ? gSemibold : gRegular;
        const ImVec2 ts = textSize(f, kBody - 4, it.label);
        text(f, kBody - 4, ImVec2(x, std::floor(cy - ts.y * 0.5f)), it.strong ? kText : kText, it.label, it.strong ? 1.0f : 0.92f);
        x += ts.x;
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

// A context with the panel's fonts and style, and a renderer backend of its own
Ui createUi() {
    Ui ui;
    ui.ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ui.ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    ui.regular = io.Fonts->AddFontFromMemoryCompressedBase85TTF(open_sans_regular_compressed_data_base85, kBody, &cfg);
    ui.semibold = io.Fonts->AddFontFromMemoryCompressedBase85TTF(open_sans_semibold_compressed_data_base85, kBody, &cfg);
    io.FontDefault = ui.regular;

    ImGuiStyle &style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);
    style.FontSizeBase = kBody;
    style.WindowPadding = ImVec2(kPad, kPad);
    style.WindowRounding = 26.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameRounding = kRound;
    style.ItemSpacing = ImVec2(12, 12);
    style.AntiAliasedFill = true;
    style.AntiAliasedLines = true;
    style.Colors[ImGuiCol_WindowBg] = rgba(kBg, 1.0f);   // opaque: the moving video must not show through
    style.Colors[ImGuiCol_Text] = rgba(kText);
    style.Colors[ImGuiCol_Border] = rgba(kLine);

    ImGui_ImplOpenGL3_Init(NULL);
    return ui;
}

}  // namespace

int
init_imgui() {
    IMGUI_CHECKVERSION();
    gStatusUi = createUi();
    gPanelUi = createUi();
    useUi(gPanelUi);
    return 0;
}

int
invoke_imgui_status_bar(int win_w, int win_h, float scale, const std::shared_ptr<AppState> &appState) {
    useUi(gStatusUi);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float) win_w, (float) win_h);
    io.DisplayFramebufferScale = {scale, scale};
    io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
    io.MouseDown[0] = false;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    render_status_bar(*appState);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    useUi(gPanelUi);
    return 0;
}

int
invoke_imgui_settings(int win_w, int win_h, float scale, const std::shared_ptr<AppState> &appState,
                      const std::vector<GuiSetting> &settings) {
    useUi(gPanelUi);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float) win_w, (float) win_h);
    io.DisplayFramebufferScale = {scale, scale};   // geometry and font glyphs are rasterised at this density

    // Inject controller ray as mouse input
    if (appState->guiPanel.rayHitting && !appState->guiPanel.grabbing) {
        io.MousePos = ImVec2(appState->guiPanel.imguiMouseX, appState->guiPanel.imguiMouseY);
        io.MouseDown[0] = appState->guiPanel.triggerDown;
        io.MouseWheel = appState->guiPanel.scrollDelta;
    } else {
        io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
        io.MouseDown[0] = false;
        io.MouseWheel = 0.0f;
    }
    io.MouseDrawCursor = false;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    render_settings_gui(appState, settings);

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    return 0;
}
