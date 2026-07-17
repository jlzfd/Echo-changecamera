cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/toolchain.cmake" CACHE FILEPATH "Default toolchain file for ARM builds")
    message(STATUS "No toolchain file specified; using default: ${CMAKE_TOOLCHAIN_FILE}")
endif()
message(STATUS "Building for ARM using toolchain file: ${CMAKE_TOOLCHAIN_FILE}")

project(rknn_yolov5_demo)

if (ENABLE_ASAN)
	message(STATUS "BUILD WITH ADDRESS SANITIZER")
	set (CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
	set (CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
	set (CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
endif ()

add_definitions(-DRV1106_1103)

set(LIBRKNNRT "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/rknpu2/Linux/armhf-uclibc/librknnmrt.so")

message(STATUS "LIB_RKNNRT = ${LIBRKNNRT}")
set(rknpu2_yolov5_file rknpu2/yolov5_rv1106_1103.cc)

set(rknpu2_face_detect_file face_detect.cc)

# ── Layer 2: KWS keyword wakeup ──
set(rknpu2_kws_detector_file kws_detector.cc)
set(rknpu2_mfcc_extract_file mfcc_extract.cc)

# ── Layer 3: NPU DMA memory reuse ──
set(rknpu2_npu_memory_reuse_file npu_memory_reuse.cc)



add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/ 3rdparty.out)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/utils/ utils.out)

set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")

file(GLOB SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)

#Opencv 4
set(OpenCV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/cmake/opencv4")
find_package(OpenCV REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(CURL REQUIRED libcurl)
include_directories(${OpenCV_INCLUDE_DIRS} )

# Add the source files
add_library(yoloCameraCore STATIC
    postprocess.cc
    AIcamera_c_interface.cc
    ${rknpu2_yolov5_file}
    ${rknpu2_face_detect_file}
    ${rknpu2_kws_detector_file}
    ${rknpu2_mfcc_extract_file}
    ${rknpu2_npu_memory_reuse_file}
)

# curl only needed by AIcamera_c_interface.cc, not added here
# to avoid pulling libcrypto.so into test executables
target_link_libraries(yoloCameraCore
    imageutils
    fileutils
    imagedrawing
    ${LIBRKNNRT}
    ${OpenCV_LIBS}
)

target_include_directories(yoloCameraCore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/rknpu2/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/allocator/dma
    ${OpenCV_LIBS}
)


option(BUILD_STANDALONE_TESTS "Build standalone test executables (face_detect_test, dual_model_test)" OFF)

add_executable(${PROJECT_NAME}
    main.cc
)

target_link_libraries(${PROJECT_NAME} yoloCameraCore Threads::Threads)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/rknpu2/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/allocator/dma
    ${OpenCV_LIBS}
)

# ── Standalone test executables (no curl/crypto) ──
if(BUILD_STANDALONE_TESTS)
add_executable(face_detect_test
    main_face.cc
)
target_link_libraries(face_detect_test yoloCameraCore Threads::Threads)
target_include_directories(face_detect_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/rknpu2/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/allocator/dma
    ${OpenCV_LIBS}
)

add_executable(dual_model_test
    main_dual.cc
)
target_link_libraries(dual_model_test yoloCameraCore Threads::Threads)
target_include_directories(dual_model_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/rknpu2/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/allocator/dma
    ${OpenCV_LIBS}
)

add_executable(verify_test
    main_verify.cc
    zero_copy_pipeline.cc
    3rdparty/allocator/dma/dma_alloc.cpp
)
target_link_libraries(verify_test yoloCameraCore Threads::Threads)
target_include_directories(verify_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/rknpu2/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/allocator/dma
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
    ${OpenCV_LIBS}
)

add_executable(unified_test
    main_unified.cc
    3rdparty/allocator/dma/dma_alloc.cpp
)
target_link_libraries(unified_test yoloCameraCore Threads::Threads)
target_include_directories(unified_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/rknpu2/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/allocator/dma
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
    ${OpenCV_LIBS}
)

add_executable(zero_copy_test
    main_zero_copy.cc
    v4l2_capture.cc
    3rdparty/allocator/dma/dma_alloc.cpp
)
target_link_libraries(zero_copy_test yoloCameraCore Threads::Threads)
target_include_directories(zero_copy_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/rknpu2/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/allocator/dma
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
    ${OpenCV_LIBS}
)
endif()

set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/rv1106_yolov5_demo")

# rpath for runtime library search
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath,\$ORIGIN/lib")

install(TARGETS ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_PREFIX})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../model/bus.jpg DESTINATION ${CMAKE_INSTALL_PREFIX}/model)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../model/coco_80_labels_list.txt DESTINATION ${CMAKE_INSTALL_PREFIX}/model)
file(GLOB RKNN_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../model/*.rknn")
install(FILES ${RKNN_FILES} DESTINATION ${CMAKE_INSTALL_PREFIX}/model)
