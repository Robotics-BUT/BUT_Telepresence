# =============================================================================
# BUT Telepresence VR App - CMake Build Configuration
#
# Android NDK shared library (.so) for a VR telepresence application.
# Requires: Android NDK, Khronos OpenXR loader (via Gradle AAR prefab),
#           GStreamer Android SDK, Boost (system + thread).
#
# External dependencies (in external/): ImGui, cpp-httplib, nlohmann/json, fmt
# =============================================================================

cmake_minimum_required(VERSION 3.10)
project(but_telepresence)
set(CMAKE_CXX_STANDARD 17)

# Platform and graphics API selection for OpenXR and conditional compilation
add_definitions(-DXR_USE_PLATFORM_ANDROID)
add_definitions(-DXR_USE_GRAPHICS_API_OPENGL_ES)
add_definitions(-Werror)

# SDK paths — set GSTREAMER_SDK_DIR and BOOST_BUILD_DIR in your build environment
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
set(GSTREAMER_ROOT ${GSTREAMER_SDK_DIR}/${ANDROID_ABI})
set(ENV{GST_PLUGIN_PATH} "${GSTREAMER_ROOT}/lib/gstreamer-1.0")
set(ENV{GST_REGISTRY} "${GSTREAMER_ROOT}/registry.bin")

# --- System libraries ---
find_library(ANDROID_LIBRARY NAMES android)
find_library(ANDROID_LOG_LIBRARY NAMES log)
find_library(EGL_LIBRARY NAMES EGL)
find_library(OpenGLES_V3_LIBRARY NAMES GLESv3)

find_path(ANDROID_NATIVE_APP_GLUE android_native_app_glue.h PATHS ${ANDROID_NDK}/sources/android/native_app_glue)
find_path(OpenGLES_V3_INCLUDE_DIR NAMES GLES3/gl3.h)
find_path(OpenGLES_V32_INCLUDE_DIR NAMES GLES3/gl32.h)

add_library(android_native_app_glue OBJECT "${ANDROID_NATIVE_APP_GLUE}/android_native_app_glue.c")

# --- OpenXR loader (Khronos official loader via Gradle AAR prefab) ---
if(NOT TARGET OpenXR::openxr_loader)
    find_package(OpenXR REQUIRED)
endif()

# --- Boost (pre-built via Boost-for-Android, found via CMake config) ---
list(APPEND CMAKE_FIND_ROOT_PATH ${BOOST_BUILD_DIR}/${ANDROID_ABI})
set(Boost_COMPILER "-clang")
find_package(Boost 1.85.0 REQUIRED CONFIG COMPONENTS system thread)

# --- Include paths ---
include_directories(
        ${ANDROID_NATIVE_APP_GLUE}
        ${OpenGLES_V3_INCLUDE_DIR}
        ${OpenGLES_V32_INCLUDE_DIR}
        ${GSTREAMER_ROOT}/include
        ${GSTREAMER_ROOT}/include/gstreamer-1.0
        ${GSTREAMER_ROOT}/include/glib-2.0
        ${GSTREAMER_ROOT}/lib/glib-2.0/include
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/include
        external
        external/imgui
        external/imgui/backends
        external/cpp-httplib
        external/json/include
        external/fmt/include
        include
)

link_directories(
        ${GSTREAMER_ROOT}/lib
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0
)

# --- Main shared library: application sources + ImGui ---
add_library(
        ${PROJECT_NAME} SHARED

        src/main.cpp
        src/program.cpp
        src/render_scene.cpp
        src/gstreamer_android.c
        src/gstreamer_player.cpp
        src/robot_control_sender.cpp
        src/rest_client.cpp
        src/render_imgui.cpp
        src/util_openxr.cpp
        src/util_egl.cpp
        src/util_shader.cpp
        src/util_render_target.cpp
        src/render_texplate.cpp
        src/ntp_timer.cpp
        src/state_storage.cpp
        src/ros_network_gateway_client.cpp
        src/camera_stats.cpp
        external/imgui/imgui.cpp
        external/imgui/imgui_demo.cpp
        external/imgui/imgui_draw.cpp
        external/imgui/imgui_tables.cpp
        external/imgui/imgui_widgets.cpp
        external/imgui/backends/imgui_impl_android.cpp
        external/imgui/backends/imgui_impl_opengl3.cpp

        $<TARGET_OBJECTS:android_native_app_glue>
)

# --- Link dependencies ---
# Order matters: GStreamer plugin .a files first, then their supporting libs,
# then GStreamer core, GLib, and finally system libs.
target_link_libraries(
        ${PROJECT_NAME}
        OpenXR::openxr_loader
        ${ANDROID_LIBRARY}
        ${ANDROID_LOG_LIBRARY}
        ${EGL_LIBRARY}
        ${OpenGLES_V3_LIBRARY}
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstcoreelements.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgsttypefindfunctions.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstvideoconvertscale.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstvideorate.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstvideofilter.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstautodetect.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstvideotestsrc.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstplayback.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstx264.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstvideoparsersbad.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstrtp.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstrtpmanager.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstrtpmanagerbad.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstopengl.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstsrtp.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstcompositor.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstvpx.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstrtsp.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstrtspclientsink.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstopenh264.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstencoding.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstudp.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstvideoparsersbad.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstapp.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstjpeg.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstjpegformat.a
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstandroidmedia.a

        #plugin libs
        ${GSTREAMER_ROOT}/lib/liborc-0.4.a
        ${GSTREAMER_ROOT}/lib/libgstgl-1.0.a
        ${GSTREAMER_ROOT}/lib/libgraphene-1.0.a
        ${GSTREAMER_ROOT}/lib/libpng16.a
        ${GSTREAMER_ROOT}/lib/libjpeg.a
        ${GSTREAMER_ROOT}/lib/libx264.a
        ${GSTREAMER_ROOT}/lib/libgstvideo-1.0.a
        ${GSTREAMER_ROOT}/lib/libgstrtp-1.0.a
        ${GSTREAMER_ROOT}/lib/libgstsdp-1.0.a
        ${GSTREAMER_ROOT}/lib/libwebrtc-audio-processing-2.a
        ${GSTREAMER_ROOT}/lib/libopencore-amrnb.a
        ${GSTREAMER_ROOT}/lib/libgstriff-1.0.a
        ${GSTREAMER_ROOT}/lib/libFLAC.a
        ${GSTREAMER_ROOT}/lib/libogg.a
        ${GSTREAMER_ROOT}/lib/libbz2.a
        ${GSTREAMER_ROOT}/lib/libfreetype.a
        ${GSTREAMER_ROOT}/lib/libgstrtsp-1.0.a
        ${GSTREAMER_ROOT}/lib/libopus.a
        ${GSTREAMER_ROOT}/lib/libvpx.a
        ${GSTREAMER_ROOT}/lib/libgstrtspserver-1.0.a
        ${GSTREAMER_ROOT}/lib/libopenh264.a

        ${GSTREAMER_ROOT}/lib/libgstaudio-1.0.a
        ${GSTREAMER_ROOT}/lib/libgstpbutils-1.0.a
        ${GSTREAMER_ROOT}/lib/libgsttag-1.0.a
        ${GSTREAMER_ROOT}/lib/libgstphotography-1.0.a
        ${GSTREAMER_ROOT}/lib/libgstcontroller-1.0.a
        ${GSTREAMER_ROOT}/lib/libgstcodecparsers-1.0.a

        ${GSTREAMER_ROOT}/lib/libgio-2.0.a
        ${GSTREAMER_ROOT}/lib/libglib-2.0.a
        ${GSTREAMER_ROOT}/lib/libgobject-2.0.a
        ${GSTREAMER_ROOT}/lib/libgstapp-1.0.a
        ${GSTREAMER_ROOT}/lib/libgstsctp-1.0.a
        ${GSTREAMER_ROOT}/lib/libgstnet-1.0.a
        #gstreamer libs

        ${GSTREAMER_ROOT}/lib/libgstbase-1.0.a
        ${GSTREAMER_ROOT}/lib/libgstreamer-1.0.a
        ${GSTREAMER_ROOT}/lib/libgmodule-2.0.a
        ${GSTREAMER_ROOT}/lib/libgobject-2.0.a
        ${GSTREAMER_ROOT}/lib/libglib-2.0.a
        ${GSTREAMER_ROOT}/lib/libffi.a
        ${GSTREAMER_ROOT}/lib/libsrtp2.a
        ${GSTREAMER_ROOT}/lib/libpcre2-8.a
        ${GSTREAMER_ROOT}/lib/libiconv.a
        ${GSTREAMER_ROOT}/lib/libintl.a
        ${GSTREAMER_ROOT}/lib/libz.a

        Boost::system
        Boost::thread

        -pthread
)
