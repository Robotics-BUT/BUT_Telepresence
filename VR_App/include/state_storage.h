//
// Created by standa on 05.01.25.
//
#pragma once

#include <jni.h>
#include <android_native_app_glue.h>
#include "common.h"

class StateStorage {
public:

    explicit StateStorage(android_app *app);

    void SaveAppState(const AppState &appState);


    AppState LoadAppState();

private:

    bool SaveKeyValuePair(jobject editor, jmethodID putString, const std::string& key, const std::string& value);
    bool SaveKeyValuePair(jobject editor, jmethodID putString, const std::string& key, const int value);

    std::string LoadValue(jobject& sharedPreferences, jmethodID& getString, const std::string& key);


    JNIEnv* env_ = nullptr;
    jobject context_;
};