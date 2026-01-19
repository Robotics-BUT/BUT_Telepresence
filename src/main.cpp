#include "pch.h"

#include "log.h"
#include <gst/gst.h>
#include <gio/gio.h>
#include <chrono>

#include "program.h"
#include "gstreamer_android.h"

struct AndroidAppState {
    ANativeWindow *NativeWindow = nullptr;
    bool Resumed = false;
};

extern "C" void gst_amc_jni_set_java_vm(JavaVM *vm);


static void
ProcessAndroidCmd(struct android_app *app, int32_t cmd) {
    AndroidAppState *appState = (AndroidAppState *) app->userData;

    switch (cmd) {
        case APP_CMD_START:
            LOG_INFO ("APP_CMD_START");
            break;

        case APP_CMD_RESUME:
            LOG_INFO("APP_CMD_RESUME");
            appState->Resumed = true;
            break;

        case APP_CMD_PAUSE:
            LOG_INFO ("APP_CMD_PAUSE");
            appState->Resumed = false;
            break;

        case APP_CMD_STOP:
            LOG_INFO ("APP_CMD_STOP");
            break;

        case APP_CMD_DESTROY:
            LOG_INFO ("APP_CMD_DESTROY");
            appState->NativeWindow = nullptr;
            break;

            // The window is being shown, get it ready.
        case APP_CMD_INIT_WINDOW:
            LOG_INFO ("APP_CMD_INIT_WINDOW");
            appState->NativeWindow = app->window;
            break;

            // The window is being hidden or closed, clean it up.
        case APP_CMD_TERM_WINDOW:
            LOG_INFO ("APP_CMD_TERM_WINDOW");
            appState->NativeWindow = nullptr;
            break;
    }
}

// Function to retrieve native library path
std::string GetNativeLibraryPath(JNIEnv *env, jobject activity) {
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getAppInfoMethod = env->GetMethodID(activityClass, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    jobject appInfo = env->CallObjectMethod(activity, getAppInfoMethod);

    jclass appInfoClass = env->GetObjectClass(appInfo);
    jfieldID nativeLibDirField = env->GetFieldID(appInfoClass, "nativeLibraryDir", "Ljava/lang/String;");
    jstring nativeLibDir = (jstring) env->GetObjectField(appInfo, nativeLibDirField);

    const char *nativeLibDirCStr = env->GetStringUTFChars(nativeLibDir, nullptr);
    std::string nativeLibraryPath(nativeLibDirCStr);
    env->ReleaseStringUTFChars(nativeLibDir, nativeLibDirCStr);

    // Clean up references
    env->DeleteLocalRef(nativeLibDir);
    env->DeleteLocalRef(appInfoClass);
    env->DeleteLocalRef(appInfo);
    env->DeleteLocalRef(activityClass);

    return nativeLibraryPath;
}

// Function to log hardware-accelerated codecs
void LogHardwareAcceleratedCodecs(JNIEnv *env) {
    jclass codecListClass = env->FindClass("android/media/MediaCodecList");

    jmethodID getCodecInfosMethod = env->GetMethodID(codecListClass, "getCodecInfos", "()[Landroid/media/MediaCodecInfo;");
    jmethodID codecListConstructor = env->GetMethodID(codecListClass, "<init>", "(I)V");
    jobject codecListInstance = env->NewObject(codecListClass, codecListConstructor, 1); // Pass 1 for hardware codecs
    jobjectArray codecInfos = (jobjectArray) env->CallObjectMethod(codecListInstance, getCodecInfosMethod);

    jsize codecCount = env->GetArrayLength(codecInfos);
    jclass codecInfoClass = env->FindClass("android/media/MediaCodecInfo");
    jmethodID isEncoderMethod = env->GetMethodID(codecInfoClass, "isEncoder", "()Z");
    jmethodID getSupportedTypesMethod = env->GetMethodID(codecInfoClass, "getSupportedTypes", "()[Ljava/lang/String;");
    jmethodID getNameMethod = env->GetMethodID(codecInfoClass, "getName", "()Ljava/lang/String;");

    for (jsize i = 0; i < codecCount; i++) {
        jobject codecInfo = env->GetObjectArrayElement(codecInfos, i);
        jboolean isEncoder = env->CallBooleanMethod(codecInfo, isEncoderMethod);

        if (!isEncoder) {
            jstring codecName = (jstring) env->CallObjectMethod(codecInfo, getNameMethod);
            jobjectArray supportedTypes = (jobjectArray) env->CallObjectMethod(codecInfo, getSupportedTypesMethod);

            const char *codecNameCStr = env->GetStringUTFChars(codecName, nullptr);
            jsize typeCount = env->GetArrayLength(supportedTypes);

            for (jsize j = 0; j < typeCount; j++) {
                jstring codecType = (jstring) env->GetObjectArrayElement(supportedTypes, j);
                const char *codecTypeCStr = env->GetStringUTFChars(codecType, nullptr);

                // Log only hardware-accelerated video codecs
                if (strstr(codecNameCStr, "OMX") || strstr(codecNameCStr, "hardware") || strstr(codecNameCStr, "amc")) {
                    LOG_INFO("HW Codec: %s, Type: %s", codecNameCStr, codecTypeCStr);
                }

                env->ReleaseStringUTFChars(codecType, codecTypeCStr);
                env->DeleteLocalRef(codecType);
            }

            env->ReleaseStringUTFChars(codecName, codecNameCStr);
            env->DeleteLocalRef(codecName);
        }

        env->DeleteLocalRef(codecInfo);
    }

    env->DeleteLocalRef(codecInfoClass);
    env->DeleteLocalRef(codecListClass);
}

void android_main(struct android_app *app) {
    try {
        JNIEnv *Env;
        app->activity->vm->AttachCurrentThread(&Env, nullptr);

        AndroidAppState appState = {};
        app->userData = &appState;
        app->onAppCmd = ProcessAndroidCmd;

        // Retrieve native library path
        std::string nativeLibraryPath = GetNativeLibraryPath(Env, app->activity->clazz);
        LOG_INFO("Native Library Path: %s", nativeLibraryPath.c_str());

        // Log hardware-accelerated codecs
        //LogHardwareAcceleratedCodecs(Env);

        // Initialize GST
        jobject activity = app->activity->clazz;
        jclass activityClass = Env->GetObjectClass(activity);
        gst_amc_jni_set_java_vm(app->activity->vm);
        gst_android_set_java_vm(app->activity->vm);
        gst_android_init(Env, activityClass, activity);

        std::unique_ptr<TelepresenceProgram> telepresenceProgram = std::make_unique<TelepresenceProgram>(app);

        bool requestRestart = false;
        bool exitRenderLoop = false;

        while (!app->destroyRequested) {
            for (;;) {
                int events;
                struct android_poll_source *source;

                if (ALooper_pollAll(0, nullptr, &events, (void **) &source) < 0) {
                    break;
                }

                if (source != nullptr) {
                    source->process(app, source);
                }
            }

            telepresenceProgram->UpdateFrame();
        }

        app->activity->vm->DetachCurrentThread();
    } catch (const std::exception &ex) {
        LOG_ERROR("%s", ex.what());
    } catch (...) {
        LOG_ERROR("Unknown Error");
    }
}