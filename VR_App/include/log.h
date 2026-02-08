#pragma once

#define LOG_INFO(...) __android_log_print(ANDROID_LOG_INFO, "but_telepresence", __VA_ARGS__)
#define LOG_ERROR(...) __android_log_print(ANDROID_LOG_ERROR, "but_telepresence", __VA_ARGS__)

// Toggle verbose debug logging at compile time
#ifdef ENABLE_DEBUG_LOG
#define LOG_DEBUG(...) __android_log_print(ANDROID_LOG_DEBUG, "but_telepresence", __VA_ARGS__)
#else
#define LOG_DEBUG(...) do {} while(0)
#endif
