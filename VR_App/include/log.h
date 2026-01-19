#pragma once

#define LOG_INFO(...) __android_log_print(ANDROID_LOG_INFO, "but_telepresence", __VA_ARGS__)
#define LOG_ERROR(...) __android_log_print(ANDROID_LOG_ERROR, "but_telepresence", __VA_ARGS__)

//NOOP
//#define LOG_INFO(...)  do {} while(0)
//#define LOG_ERROR(...) do {} while(0)