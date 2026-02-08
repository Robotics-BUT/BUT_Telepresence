/**
 * log.h - Android logcat logging macros
 *
 * All log output goes to Android logcat under the tag "but_telepresence".
 * LOG_INFO and LOG_ERROR are always active. LOG_DEBUG is compiled out
 * unless the ENABLE_DEBUG_LOG flag is set in CMakeLists.txt.
 */
#pragma once

#define LOG_INFO(...) __android_log_print(ANDROID_LOG_INFO, "but_telepresence", __VA_ARGS__)
#define LOG_ERROR(...) __android_log_print(ANDROID_LOG_ERROR, "but_telepresence", __VA_ARGS__)

#ifdef ENABLE_DEBUG_LOG
#define LOG_DEBUG(...) __android_log_print(ANDROID_LOG_DEBUG, "but_telepresence", __VA_ARGS__)
#else
#define LOG_DEBUG(...) do {} while(0)
#endif
