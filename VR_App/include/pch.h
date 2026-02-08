#pragma once

// =============================================================================
// Precompiled Header
// Include stable, frequently-used system and library headers here
// =============================================================================

// -----------------------------------------------------------------------------
// C++ Standard Library
// -----------------------------------------------------------------------------
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <list>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

// -----------------------------------------------------------------------------
// Platform-specific headers
// -----------------------------------------------------------------------------
#ifdef XR_USE_PLATFORM_ANDROID
#include <android/log.h>
#include <android_native_app_glue.h>
#include <android/native_window.h>
#include <jni.h>
#include <sys/system_properties.h>
#endif

// -----------------------------------------------------------------------------
// Graphics API
// -----------------------------------------------------------------------------
#ifdef XR_USE_GRAPHICS_API_OPENGL_ES
#include <EGL/egl.h>
#endif

// -----------------------------------------------------------------------------
// OpenXR
// -----------------------------------------------------------------------------
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_reflection.h>
