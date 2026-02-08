/**
 * state_storage.cpp - Persistent application state via Android SharedPreferences
 *
 * Serializes AppState fields as string key-value pairs into Android
 * SharedPreferences ("AppStatePrefs"). All values are stored as strings
 * (integers converted via std::to_string). The headMovementSpeedMultiplier
 * is scaled by 10 before saving to work around integer-only formatting.
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
        SaveKeyValuePair(editor, putString, "head_movement_max_speed", appState.headMovementMaxSpeed);
        SaveKeyValuePair(editor, putString, "head_movement_prediction_ms", appState.headMovementPredictionMs);
        SaveKeyValuePair(editor, putString, "head_movement_speed_multiplier", appState.headMovementSpeedMultiplier * 10); // To build around integer formatting
        SaveKeyValuePair(editor, putString, "robot_control_enabled", appState.robotControlEnabled);
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


AppState StateStorage::LoadAppState() {
    jclass contextClass = env_->GetObjectClass(context_);
    jmethodID getSharedPreferences = env_->GetMethodID(contextClass, "getSharedPreferences", "(Ljava/lang/String;I)Landroid/content/SharedPreferences;");

    jstring prefsName = env_->NewStringUTF("AppStatePrefs");
    jobject sharedPreferences = env_->CallObjectMethod(context_, getSharedPreferences, prefsName, 0);
    env_->DeleteLocalRef(prefsName);

    jclass prefsClass = env_->GetObjectClass(sharedPreferences);
    jmethodID getString = env_->GetMethodID(prefsClass, "getString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");

    AppState appState;
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
        appState.headMovementMaxSpeed = std::stoi(LoadValue(sharedPreferences, getString, "head_movement_max_speed"));
        appState.headMovementPredictionMs = std::stoi(LoadValue(sharedPreferences, getString, "head_movement_prediction_ms"));
        appState.headMovementSpeedMultiplier = std::stof(LoadValue(sharedPreferences, getString, "head_movement_speed_multiplier") ) / 10.0f; // To build around integer formatting
        appState.robotControlEnabled = std::stoi(LoadValue(sharedPreferences, getString, "robot_control_enabled"));

    } catch(const std::exception& e) {
        env_->DeleteLocalRef(sharedPreferences);
        env_->DeleteLocalRef(prefsClass);
        env_->DeleteLocalRef(contextClass);

        return AppState();
        //throw std::runtime_error("Couldn't load from the shared preferences");
    }

    env_->DeleteLocalRef(sharedPreferences);
    env_->DeleteLocalRef(prefsClass);
    env_->DeleteLocalRef(contextClass);

    return appState;
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
