#ifndef AICAMERA_C_INTERFACE_H
#define AICAMERA_C_INTERFACE_H

#ifdef __cplusplus
#include <string>   // 用于 std::string
extern "C" {
#endif

int start_ai_camera(const char* model_path, void* app_ptr);
int attach_ai_camera_consumer(void* app_ptr);
int detach_ai_camera_consumer(void* app_ptr);
int stop_ai_camera();
void get_buf_data(uint8_t* buffer);
int load_face_detect_model(const char* model_path);
int unload_face_detect_model();

// Start AI camera with zero-copy V4L2+DMA-BUF pipeline.
// yolo_path:     YOLO model .rknn file
// face_path:     Face detection model .rknn file (NULL to skip)
// app_ptr:       Application instance pointer
// yolo_active:   non-zero to run YOLO on every frame
// face_interval: run face every N frames (1=every, 0=off)
// Returns 0 on success, -1 on error, 1 if already running.
int start_ai_camera_v2(const char* yolo_path, const char* face_path,
                        void* app_ptr, int yolo_active, int face_interval);

#ifdef __cplusplus
}
#endif

// C++ 专属声明
#ifdef __cplusplus
#include <opencv2/opencv.hpp>
void mapCoordinates(cv::Mat input, cv::Mat output, int *x, int *y);

// 声明 base64_encode 函数（非静态）
std::string base64_encode(const unsigned char* bytes_to_encode, unsigned int in_len);
#endif

#endif // AICAMERA_C_INTERFACE_H
