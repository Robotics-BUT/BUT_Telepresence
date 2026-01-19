cmake_minimum_required(VERSION 3.10)
project(but_telepresence)
set(CMAKE_CXX_STANDARD 17)

add_definitions(-DXR_USE_PLATFORM_ANDROID)
add_definitions(-DXR_USE_GRAPHICS_API_OPENGL_ES)
add_definitions(-Werror)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
set(GSTREAMER_ROOT ${GSTREAMER_SDK_DIR}/${ANDROID_ABI})
set(ENV{GST_PLUGIN_PATH} "${GSTREAMER_ROOT}/lib/gstreamer-1.0")
set(ENV{GST_REGISTRY} "${GSTREAMER_ROOT}/registry.bin")
set(BOOST_ROOT ${BOOST_DIR}/libs/${ANDROID_ABI})
set(BOOST_INCLUDE ${BOOST_DIR}/include)

find_library(ANDROID_LIBRARY NAMES android)
find_library(ANDROID_LOG_LIBRARY NAMES log)
find_library(EGL_LIBRARY NAMES EGL)
find_library(OpenGLES_V3_LIBRARY NAMES GLESv3)

find_path(ANDROID_NATIVE_APP_GLUE android_native_app_glue.h PATHS ${ANDROID_NDK}/sources/android/native_app_glue)
find_path(OpenGLES_V3_INCLUDE_DIR NAMES GLES3/gl3.h)
find_path(OpenGLES_V32_INCLUDE_DIR NAMES GLES3/gl32.h)

add_library(android_native_app_glue OBJECT "${ANDROID_NATIVE_APP_GLUE}/android_native_app_glue.c")

add_library(openxr_loader SHARED IMPORTED)
set_property(
        TARGET
        openxr_loader
        PROPERTY
        IMPORTED_LOCATION
        ${OVR_OPENXR_MOBILE_SDK_DIR}/OpenXR/Libs/Android/${ANDROID_ABI}/${CMAKE_BUILD_TYPE}/libopenxr_loader.so
)

include_directories(
        ${ANDROID_NATIVE_APP_GLUE}
        ${OpenGLES_V3_INCLUDE_DIR}
        ${OpenGLES_V32_INCLUDE_DIR}
        ${PROJECT_SOURCE_DIR}/external/OpenXR-SDK/include
        ${GSTREAMER_ROOT}/include
        ${GSTREAMER_ROOT}/include/gstreamer-1.0
        ${GSTREAMER_ROOT}/include/glib-2.0
        ${GSTREAMER_ROOT}/lib/glib-2.0/include
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/include
        ${BOOST_INCLUDE}
        ${OVR_OPENXR_MOBILE_SDK_DIR}/SampleCommon/Src
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
        external/imgui/imgui.cpp
        external/imgui/imgui_demo.cpp
        external/imgui/imgui_draw.cpp
        external/imgui/imgui_tables.cpp
        external/imgui/imgui_widgets.cpp
        external/imgui/backends/imgui_impl_android.cpp
        external/imgui/backends/imgui_impl_opengl3.cpp

        $<TARGET_OBJECTS:android_native_app_glue>
)

target_link_libraries(
        ${PROJECT_NAME}
        openxr_loader
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
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstlibav.a
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
        ${GSTREAMER_ROOT}/lib/gstreamer-1.0/libgstlibav.a
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
        ${GSTREAMER_ROOT}/lib/libwebrtc_audio_processing.a
        ${GSTREAMER_ROOT}/lib/libopencore-amrnb.a
        ${GSTREAMER_ROOT}/lib/libgstriff-1.0.a
        ${GSTREAMER_ROOT}/lib/libFLAC.a
        ${GSTREAMER_ROOT}/lib/libogg.a
        ${GSTREAMER_ROOT}/lib/libavformat.a
        ${GSTREAMER_ROOT}/lib/libbz2.a
        ${GSTREAMER_ROOT}/lib/libfreetype.a
        ${GSTREAMER_ROOT}/lib/libgstrtsp-1.0.a
        ${GSTREAMER_ROOT}/lib/liba52.a
        ${GSTREAMER_ROOT}/lib/libavcodec.a
        ${GSTREAMER_ROOT}/lib/libavutil.a
        ${GSTREAMER_ROOT}/lib/libavfilter.a
        ${GSTREAMER_ROOT}/lib/libswresample.a
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

        ${BOOST_ROOT}/libboost_system.a
        ${BOOST_ROOT}/libboost_thread.a

        -pthread
)
