#pragma once

#include <string>
#include <functional>

enum class GuiSettingType {
    Text,       // rendered with focusable_text
    IpAddress,  // rendered with focusable_text_ip (has segment navigation)
    Button      // rendered with focusable_button
};

struct GuiSetting {
    std::string label;
    GuiSettingType type = GuiSettingType::Text;
    std::string sectionHeader;                          // if non-empty, renders ImGui::SeparatorText before this element

    std::function<std::string()> getDisplayText;        // full formatted display string
    std::function<void()> onIncrement;                  // Y button (increase value)
    std::function<void()> onDecrement;                  // X button (decrease value)
    std::function<void()> onActivate;                   // Left trigger (for buttons)

    int segments = 1;                                   // >1 for IP-style fields with segment navigation
};
