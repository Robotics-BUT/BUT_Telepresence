/**
 * state_storage.h - Persistent application state via Android SharedPreferences
 *
 * Saves and loads the AppState (streaming config, head tracking settings)
 * to Android SharedPreferences through JNI. This allows settings to persist
 * across app restarts. Saving is triggered when the user closes the GUI
 * or presses the Apply button.
 */
#pragma once

#include <jni.h>
#include <android_native_app_glue.h>
#include "types/app_state.h"
#include "utils/network_utils.h"

class StateStorage {
public:

    explicit StateStorage(android_app *app);

    /** Serialize key AppState fields to SharedPreferences. */
    void SaveAppState(const AppState &appState);

    /** Deserialize AppState fields from SharedPreferences (with defaults for missing keys). */
    AppState LoadAppState();

private:

    bool SaveKeyValuePair(jobject editor, jmethodID putString, const std::string& key, const std::string& value);
    bool SaveKeyValuePair(jobject editor, jmethodID putString, const std::string& key, const int value);

    std::string LoadValue(jobject& sharedPreferences, jmethodID& getString, const std::string& key);

    JNIEnv* env_ = nullptr;
    jobject context_;
};