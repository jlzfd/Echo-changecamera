cmake_minimum_required(VERSION 3.10)

# Option to choose target architecture
option(TARGET_ARM "Build for ARM architecture" OFF)

# --- yolov5 根目录 ---
set(YOLO_V5_DIR "/home/ubuntu/Echo-Mate-main/Demo/yolov5_demo")
set(MPP_RELEASE_DIR "" CACHE PATH "MPP release directory (e.g. /path/to/rv1106-sdk/media/mpp/release_mpp_rv1106_arm-...")

# Auto-detect MPP headers: try common locations
if(MPP_RELEASE_DIR STREQUAL "")
    # Try sysroot first (MPP may be installed in buildroot)
    find_path(MPP_INCLUDE_DIR rk_mpi.h
        PATHS
            ${CMAKE_SYSROOT}/usr/include/rockchip
            /home/ubuntu/rv1106-sdk/media/mpp/release_mpp_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rockchip
        DOC "MPP include directory containing rockchip/*.h"
    )
    if(MPP_INCLUDE_DIR)
        get_filename_component(MPP_RELEASE_DIR "${MPP_INCLUDE_DIR}" DIRECTORY)
        message(STATUS "Auto-detected MPP at: ${MPP_RELEASE_DIR}")
    else()
        message(WARNING "MPP_RELEASE_DIR not set and auto-detect failed.")
        message(WARNING "Build with: cmake -DMPP_RELEASE_DIR=/path/to/mpp/release ...")
    endif()
endif()

# If building for ARM, set the toolchain file before anything else
if(TARGET_ARM)
    if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
        set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/toolchain.cmake"
            CACHE FILEPATH "Default toolchain file for ARM builds")
    endif()
    message(STATUS "Building for ARM using toolchain file: ${CMAKE_TOOLCHAIN_FILE}")
else()
    message(STATUS "Building for x86 native architecture.")
endif()

# Project
project(AIChatClient LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =============================
# OpenCV
# =============================
set(OpenCV_DIR "${YOLO_V5_DIR}/cpp/lib/cmake/opencv4")
find_package(OpenCV REQUIRED)

# =============================
# 查找系统库
# =============================
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

if(TARGET_ARM)
    set(ENV{PKG_CONFIG_PATH} "${CMAKE_SYSROOT}/usr/lib/pkgconfig")
endif()

pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)
pkg_check_modules(OPUS REQUIRED opus)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)
find_package(WEBSOCKETPP REQUIRED)

# =============================
# RKNN runtime
# =============================
set(LIBRKNNRT
${YOLO_V5_DIR}/cpp/3rdparty/rknpu2/Linux/armhf-uclibc/librknnmrt.so
)

# =============================
# include 目录
# =============================
include_directories(
    ${OPUS_INCLUDE_DIRS}
    ${PORTAUDIO_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
    ${WEBSOCKETPP_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}

    ${YOLO_V5_DIR}/cpp
    ${YOLO_V5_DIR}/cpp/utils
    ${YOLO_V5_DIR}/cpp/rknpu2
    ${YOLO_V5_DIR}/cpp/3rdparty/rknpu2/include
    ${YOLO_V5_DIR}/cpp/3rdparty/allocator/dma
    ${YOLO_V5_DIR}/cpp/3rdparty/librga/include
	    ${YOLO_V5_DIR}/cpp/3rdparty/stb_image
	)


# RGA library (full path like LIBRKNNRT)
set(LIBRGA ${YOLO_V5_DIR}/cpp/3rdparty/librga/Linux/armhf_uclibc/librga.so)

if(NOT MPP_RELEASE_DIR STREQUAL "")
    include_directories(${MPP_RELEASE_DIR}/include)
    set(LIBROCKCHIP_MPP ${MPP_RELEASE_DIR}/lib/librockchip_mpp.so)
else()
    set(LIBROCKCHIP_MPP rockchip_mpp)
endif()

# =============================
# AIChatCore
# =============================
add_library(AIChatCore STATIC

    Audio/AudioProcess.cc
    WebSocket/WebsocketClient.cc
    Intent/IntentHandler.cc
    StateMachine/StateMachine.cc
    Application/Application.cc
    Application/WS_Handler.cc
    Application/StateConfig.cc
        Application/VisionFrameBuffer.cc

    Application/UserStates/Fault.cc
    Application/UserStates/Startup.cc
    Application/UserStates/Stop.cc
    Application/UserStates/Idle.cc
    Application/UserStates/Listening.cc
    Application/UserStates/Thinking.cc
    Application/UserStates/Speaking.cc

        # YOLO 视觉模块
    ${YOLO_V5_DIR}/cpp/AIcamera_c_interface.cc
    ${YOLO_V5_DIR}/cpp/postprocess.cc
    ${YOLO_V5_DIR}/cpp/rknpu2/yolov5_rv1106_1103.cc
    ${YOLO_V5_DIR}/cpp/face_detect.cc
    ${YOLO_V5_DIR}/cpp/kws_detector.cc
    ${YOLO_V5_DIR}/cpp/mfcc_extract.cc
    ${YOLO_V5_DIR}/cpp/npu_memory_reuse.cc
    ${YOLO_V5_DIR}/cpp/v4l2_capture.cc
    ${YOLO_V5_DIR}/cpp/hw_jpeg_encoder.cc
    ${YOLO_V5_DIR}/cpp/3rdparty/allocator/dma/dma_alloc.cpp
	    rga_convert.c
)

# =============================
# 链接依赖
# =============================
target_link_libraries(AIChatCore

    ${OPUS_LIBRARIES}
    ${PORTAUDIO_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    ${WEBSOCKETPP_LIBRARIES}

    ${OpenCV_LIBS}

    ${LIBRKNNRT}

    ${LIBRGA}

    ${LIBROCKCHIP_MPP}

    Threads::Threads
)

# =============================
# 主程序
# =============================
add_executable(AIChatClient main.cc)

target_link_libraries(AIChatClient
    AIChatCore
    curl
    dl
    m
)

# =============================
# 子模块
# =============================
add_subdirectory(c_interface)

# =============================
# RPATH
# =============================
set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")

# =============================
# 安装
# =============================
set(CMAKE_INSTALL_PREFIX
"${CMAKE_CURRENT_SOURCE_DIR}/rv1106_AIChatClient_demo"
)

install(TARGETS AIChatClient DESTINATION bin)

install(FILES
${LIBRKNNRT}
DESTINATION bin/lib
)
