cmake_minimum_required(VERSION 3.10.2)
project(telepresence_streaming_driver)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CCMAKE_CXX_FLAGS} -pthread")

find_package(PkgConfig REQUIRED)
pkg_search_module(GSTREAMER REQUIRED gstreamer-1.0)
pkg_search_module(GSTREAMER_RTP REQUIRED gstreamer-rtp-1.0)

add_definitions(${GSTREAMER_CFLAGS_OTHER})

add_definitions(-DJETSON)

add_executable(telepresence_streaming_driver main.cpp)
target_compile_definitions(telepresence_streaming_driver PRIVATE STREAMING)

target_include_directories(telepresence_streaming_driver PRIVATE ${GSTREAMER_INCLUDE_DIRS} ${GSTREAMER_RTP_INCLUDE_DIRS} include)
target_link_libraries(telepresence_streaming_driver ${GSTREAMER_LIBRARIES} ${GSTREAMER_RTP_LIBRARIES})
