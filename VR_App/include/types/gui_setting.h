/**
 * gui_setting.h - Data-driven GUI setting definition
 *
 * Each entry in the settings table is a GuiSetting that describes one row of the VR settings panel: which tab it is
 * on, what control it shows and what happens when it is changed. The renderer (render_imgui.cpp) lays the rows out
 * per tab; the table is built in settings_table.cpp.
 *
 * Interaction in VR:
 *   - Right controller ray + trigger: press the row's controls directly (- / + buttons, choices, switches, buttons)
 *   - Left thumbstick up/down: move the highlight between the rows of the open tab
 *   - Left thumbstick left/right: switch tab (on an IP address row: move between its octets)
 *   - Y / X buttons: onIncrement / onDecrement on the highlighted row
 *   - Left trigger: onActivate on the highlighted row (buttons)
 */
#pragma once

#include <string>
#include <vector>
#include <functional>

/** The tabs of the settings panel, in display order. */
enum class GuiTab {
    Stream,
    Camera,
    Audio,
    System,
    Count
};

/** A press on a row's control with the controller ray, queued for HandleControllers (GUIControl). */
enum class GuiAction {
    None,
    Increment,  /* onIncrement */
    Decrement,  /* onDecrement */
    Activate,   /* onActivate */
    Choose,     /* setChoice(value) */
    Toggle      /* setToggle(value != 0) */
};

/** How a row is drawn and operated. */
enum class GuiControl {
    Stepper,    /* - value + ; onDecrement / onIncrement */
    Choice,     /* one button per option; getChoice / setChoice (onIncrement / onDecrement step through them) */
    Toggle,     /* switch; getToggle / setToggle (onIncrement = on, onDecrement = off) */
    Button,     /* full-width button; onActivate */
    IpAddress,  /* four octets, the highlighted one changed by - / + */
    Readout     /* value only */
};

/**
 * One row of the settings panel. Built in BuildSettingsTable() with lambdas that capture the AppState.
 */
struct GuiSetting {
    std::string label;                                  /* shown on the left of the row */
    GuiControl control = GuiControl::Stepper;
    GuiTab tab = GuiTab::Stream;

    std::function<std::string()> getValue;              /* formatted value (Stepper, IpAddress, Readout, Button label) */
    std::function<void()> onIncrement;                  /* + / Y button */
    std::function<void()> onDecrement;                  /* - / X button */
    std::function<void()> onActivate;                   /* Button, left trigger */

    std::vector<std::string> choices;                   /* Choice: the options' labels */
    std::function<int()> getChoice;
    std::function<void(int)> setChoice;

    std::function<bool()> getToggle;                    /* Toggle */
    std::function<void(bool)> setToggle;

    std::function<bool()> isVisible;                    /* row shown only when this returns true (empty = always) */
    std::function<bool()> isEnabled;                    /* Button: pressable only when this returns true */
    std::function<bool()> isPending;                    /* changed but not yet applied: marked on the row */

    int segments = 1;                                   /* IpAddress: 4 */
};
