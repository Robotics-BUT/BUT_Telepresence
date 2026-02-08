/**
 * gui_setting.h - Data-driven GUI setting definition
 *
 * Each entry in the settings table is a GuiSetting struct that describes
 * how a single row in the VR settings panel looks and behaves. The GUI
 * renderer (render_imgui.cpp) iterates over a vector of GuiSettings and
 * calls the appropriate focusable_* rendering function based on the type.
 *
 * Interaction in VR (no mouse/keyboard):
 *   - Left thumbstick: navigate between settings (focus up/down/left/right)
 *   - Y button: calls onIncrement (increase value)
 *   - X button: calls onDecrement (decrease value)
 *   - Left trigger: calls onActivate (for button-type settings)
 */
#pragma once

#include <string>
#include <functional>

/**
 * Determines which rendering function is used for a GUI setting row.
 */
enum class GuiSettingType {
    Text,       /* rendered with focusable_text() */
    IpAddress,  /* rendered with focusable_text_ip() - supports segment navigation */
    Button      /* rendered with focusable_button() */
};

/**
 * A single entry in the settings GUI table.
 * Built in TelepresenceProgram::BuildSettings() with lambdas that capture
 * the program's this pointer for accessing AppState.
 */
struct GuiSetting {
    std::string label;                                  /* human-readable name (for identification) */
    GuiSettingType type = GuiSettingType::Text;
    std::string sectionHeader;                          /* if non-empty, renders a separator heading before this row */

    std::function<std::string()> getDisplayText;        /* returns the formatted display string */
    std::function<void()> onIncrement;                  /* Y button handler (increase value) */
    std::function<void()> onDecrement;                  /* X button handler (decrease value) */
    std::function<void()> onActivate;                   /* left trigger handler (for buttons) */

    int segments = 1;                                   /* >1 enables left/right sub-navigation (e.g. IP octets) */
};
