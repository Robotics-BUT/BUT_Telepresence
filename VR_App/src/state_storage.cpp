/**
 * state_storage.cpp - Persistent application state via Android SharedPreferences
 *
 * Serializes AppState fields as string key-value pairs into Android
 * SharedPreferences ("AppStatePrefs"). All values are stored as strings
 * (integers converted via std::to_string). Fractional settings are scaled to integers before saving
 * (headMovementSensitivity x 100, stereoConvergence x 1000).
 * On load failure (missing keys or parse errors), returns a default AppState.
 */
#include "state_storage.h"

StateStorage::StateStorage(android_app *app) {
    // Check if the current thread is already attached to the JVM
    if (app->activity->vm->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6) != JNI_OK) {
        // Attach the thread to the JVM
        if (app->activity->vm->AttachCurrentThread(&env_, nullptr) != JNI_OK) {
            throw std::runtime_error("Failed to attach thread!");
        }
    }

    context_ = app->activity->clazz;
};

void StateStorage::SaveAppState(const AppState &appState) {
    jclass contextClass = env_->GetObjectClass(context_);
    jmethodID getSharedPreferences = env_->GetMethodID(contextClass, "getSharedPreferences",
                                                      "(Ljava/lang/String;I)Landroid/content/SharedPreferences;");

    jstring prefsName = env_->NewStringUTF("AppStatePrefs");
    jobject sharedPreferences = env_->CallObjectMethod(context_, getSharedPreferences, prefsName, 0);
    env_->DeleteLocalRef(prefsName);

    jclass prefsClass = env_->GetObjectClass(sharedPreferences);
    jmethodID edit = env_->GetMethodID(prefsClass, "edit",
                                      "()Landroid/content/SharedPreferences$Editor;");
    jobject editor = env_->CallObjectMethod(sharedPreferences, edit);

    jclass editorClass = env_->GetObjectClass(editor);
    jmethodID putString = env_->GetMethodID(editorClass, "putString",
                                           "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;");

    {
        SaveKeyValuePair(editor, putString, "headset_ip", IpToString(appState.streamingConfig.headset_ip));
        SaveKeyValuePair(editor, putString, "jetson_ip", IpToString(appState.streamingConfig.jetson_ip));
        SaveKeyValuePair(editor, putString, "port_left", appState.streamingConfig.portLeft);
        SaveKeyValuePair(editor, putString, "port_right", appState.streamingConfig.portRight);
        SaveKeyValuePair(editor, putString, "codec", static_cast<int>(appState.streamingConfig.codec));
        SaveKeyValuePair(editor, putString, "encoding_quality", appState.streamingConfig.encodingQuality);
        SaveKeyValuePair(editor, putString, "bitrate", appState.streamingConfig.bitrate);
        SaveKeyValuePair(editor, putString, "resolution", appState.streamingConfig.resolution.getLabel());
        SaveKeyValuePair(editor, putString, "video_mode", static_cast<int>(appState.streamingConfig.videoMode));
        SaveKeyValuePair(editor, putString, "fps", appState.streamingConfig.fps);

        SaveKeyValuePair(editor, putString, "aspect_ratio_mode", static_cast<int>(appState.aspectRatioMode));
        SaveKeyValuePair(editor, putString, "head_max_speed_dps", static_cast<int>(std::lround(appState.headMaxSpeedDegS)));
        SaveKeyValuePair(editor, putString, "head_movement_prediction_ms", appState.headMovementPredictionMs);
        SaveKeyValuePair(editor, putString, "head_movement_sensitivity", static_cast<int>(std::lround(appState.headMovementSensitivity * 100)));
        SaveKeyValuePair(editor, putString, "robot_control_enabled", appState.robotControlEnabled);
        SaveKeyValuePair(editor, putString, "stereo_convergence", static_cast<int>(appState.stereoConvergence * 1000)); // scaled to survive integer formatting
        SaveKeyValuePair(editor, putString, "panel_layout", static_cast<int>(appState.guiPanel.layout));

        SaveKeyValuePair(editor, putString, "audio_robot_enable", appState.audioRobotEnable);
        SaveKeyValuePair(editor, putString, "audio_mic_enable", appState.audioMicEnable);
        SaveKeyValuePair(editor, putString, "mic_push_to_talk", appState.micPushToTalk);
        SaveKeyValuePair(editor, putString, "mic_muted", appState.micMuted);
        SaveKeyValuePair(editor, putString, "audio_volume", appState.audioVolume);
    }


    jmethodID apply = env_->GetMethodID(editorClass, "apply", "()V");
    env_->CallVoidMethod(editor, apply);

    env_->DeleteLocalRef(editor);
    env_->DeleteLocalRef(editorClass);
    env_->DeleteLocalRef(sharedPreferences);
    env_->DeleteLocalRef(prefsClass);
    env_->DeleteLocalRef(contextClass);
}

bool StateStorage::SaveKeyValuePair(jobject editor, jmethodID putString, const std::string& key, const std::string& value) {
    // Convert key and value to jstring
    jstring jKey = env_->NewStringUTF(key.c_str());
    jstring jValue = env_->NewStringUTF(value.c_str());

    // Call the Java method
    env_->CallObjectMethod(editor, putString, jKey, jValue);

    // Clean up local references
    env_->DeleteLocalRef(jKey);
    env_->DeleteLocalRef(jValue);

    return true;
}


bool StateStorage::SaveKeyValuePair(jobject editor, jmethodID putString, const std::string& key, const int value) {
    return SaveKeyValuePair(editor, putString, key, std::to_string(value));
}


void StateStorage::LoadAppState(AppState& appState) {
    jclass contextClass = env_->GetObjectClass(context_);
    jmethodID getSharedPreferences = env_->GetMethodID(contextClass, "getSharedPreferences", "(Ljava/lang/String;I)Landroid/content/SharedPreferences;");

    jstring prefsName = env_->NewStringUTF("AppStatePrefs");
    jobject sharedPreferences = env_->CallObjectMethod(context_, getSharedPreferences, prefsName, 0);
    env_->DeleteLocalRef(prefsName);

    jclass prefsClass = env_->GetObjectClass(sharedPreferences);
    jmethodID getString = env_->GetMethodID(prefsClass, "getString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");

    try {
        appState.streamingConfig.headset_ip = StringToIp(LoadValue(sharedPreferences, getString, "headset_ip"));
        appState.streamingConfig.jetson_ip = StringToIp(LoadValue(sharedPreferences, getString, "jetson_ip"));
        appState.streamingConfig.portLeft = std::stoi(LoadValue(sharedPreferences, getString, "port_left"));
        appState.streamingConfig.portRight = std::stoi(LoadValue(sharedPreferences, getString, "port_right"));
        appState.streamingConfig.codec = Codec(std::stoi(LoadValue(sharedPreferences, getString, "codec")));
        appState.streamingConfig.encodingQuality = std::stoi(LoadValue(sharedPreferences, getString, "encoding_quality"));
        appState.streamingConfig.bitrate = std::stoi(LoadValue(sharedPreferences, getString, "bitrate"));
        appState.streamingConfig.resolution = CameraResolution::fromLabel(LoadValue(sharedPreferences, getString, "resolution"));
        appState.streamingConfig.videoMode = VideoMode(std::stoi(LoadValue(sharedPreferences, getString, "video_mode")));
        appState.streamingConfig.fps = std::stoi(LoadValue(sharedPreferences, getString, "fps"));

        appState.aspectRatioMode = static_cast<AspectRatioMode>(std::stoi(LoadValue(sharedPreferences, getString, "aspect_ratio_mode")));
        appState.headMovementPredictionMs = std::stoi(LoadValue(sharedPreferences, getString, "head_movement_prediction_ms"));
        appState.robotControlEnabled = std::stoi(LoadValue(sharedPreferences, getString, "robot_control_enabled"));
        appState.stereoConvergence = std::stof(LoadValue(sharedPreferences, getString, "stereo_convergence")) / 1000.0f;

        appState.audioRobotEnable = std::stoi(LoadValue(sharedPreferences, getString, "audio_robot_enable"));
        appState.audioMicEnable = std::stoi(LoadValue(sharedPreferences, getString, "audio_mic_enable"));
        appState.micPushToTalk = std::stoi(LoadValue(sharedPreferences, getString, "mic_push_to_talk"));
        appState.micMuted = std::stoi(LoadValue(sharedPreferences, getString, "mic_muted"));
        appState.audioVolume = std::stoi(LoadValue(sharedPreferences, getString, "audio_volume"));

    } catch(const std::exception& e) {
        // Parse failure: leave appState as the caller's default-constructed state.
    }
    try {   // added later than the keys above: read on its own so that, while missing, only it keeps its default
        appState.headMovementSensitivity = std::stof(LoadValue(sharedPreferences, getString, "head_movement_sensitivity")) / 100.0f;
    } catch(const std::exception& e) {}
    try {   // likewise
        appState.headMaxSpeedDegS = std::stof(LoadValue(sharedPreferences, getString, "head_max_speed_dps"));
    } catch(const std::exception& e) {}
    try {   // likewise
        appState.guiPanel.layout = std::stoi(LoadValue(sharedPreferences, getString, "panel_layout")) == 1
                                   ? GuiPanelLayout::Below : GuiPanelLayout::Front;
    } catch(const std::exception& e) {}

    env_->DeleteLocalRef(sharedPreferences);
    env_->DeleteLocalRef(prefsClass);
    env_->DeleteLocalRef(contextClass);
}

std::string StateStorage::LoadValue(jobject& sharedPreferences, jmethodID& getString, const std::string& key) {
    jstring jKey = env_->NewStringUTF(key.c_str());
    jstring jDefaultValue = env_->NewStringUTF("unknown");
    jstring jResult = (jstring)env_->CallObjectMethod(sharedPreferences, getString, jKey, jDefaultValue);

    env_->DeleteLocalRef(jKey);
    env_->DeleteLocalRef(jDefaultValue);

    const char* resultCStr = env_->GetStringUTFChars(jResult, nullptr);
    std::string result(resultCStr);

    env_->ReleaseStringUTFChars(jResult, resultCStr);
    env_->DeleteLocalRef(jResult);

    return result;
}
