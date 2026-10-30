/**
 * settings_table.h - The rows of the VR settings panel
 *
 * BuildSettingsTable() returns the panel's rows (types/gui_setting.h), grouped by tab. Rows change the AppState
 * directly; everything that needs the program (applying the stream, the audio legs, the robot base) goes through
 * SettingsActions, so the table can also be built outside the app (the desktop preview of the panel).
 */
#pragma once

#include <functional>
#include <memory>
#include <vector>
#include "types/app_state.h"
#include "types/gui_setting.h"

/** What the settings table needs from the program besides the AppState. */
struct SettingsActions {
    std::function<void()> applyStream;                          /* save the settings and apply the stream config */
    std::function<void()> restartStream;                        /* rebuild both ends with the config in force */
    std::function<void()> applyAudio;                           /* start or stop the audio legs after a change */
    std::function<void(int)> setVolume;                         /* playback volume, percent */
    std::function<void(bool)> setRobotDrive;                    /* enable the robot base (sends a stop when disabling) */
    std::function<void(bool)> setKeepRunningOffHead;            /* override the proximity sensor, or restore it */
    std::function<void(GuiPanelLayout)> setPanelLayout;         /* switch the panel's layout and place it for it */
    std::function<void()> resetHeadDrives;                      /* ask the robot to clear the camera head's faults */
    std::function<const StreamingConfig *()> appliedConfig;     /* the config in force, or null before the first apply */
};

std::vector<GuiSetting> BuildSettingsTable(const std::shared_ptr<AppState> &appState, const SettingsActions &actions);

/** Number of stream settings that differ from the applied config (all of them count before the first apply). */
int PendingStreamChanges(const StreamingConfig &now, const StreamingConfig *applied);
