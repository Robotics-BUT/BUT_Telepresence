cmake_minimum_required(VERSION 3.10.2)
project(telepresence_audio_driver)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")

find_package(PkgConfig REQUIRED)
pkg_search_module(GSTREAMER REQUIRED gstreamer-1.0)

add_definitions(${GSTREAMER_CFLAGS_OTHER})

add_executable(telepresence_audio_driver main.cpp)

# Reuse the video driver's bundled nlohmann/json single header.
target_include_directories(telepresence_audio_driver PRIVATE
        ${GSTREAMER_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/../video_driver/include)

target_link_libraries(telepresence_audio_driver ${GSTREAMER_LIBRARIES})
