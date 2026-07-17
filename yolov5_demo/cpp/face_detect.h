#ifndef _RKNN_DEMO_FACE_DETECT_H_
#define _RKNN_DEMO_FACE_DETECT_H_

#define RV1106_1103
#include "rknn_api.h"
#include "common.h"

// 人脸检测结果：边界框 + 5 点关键点 + 置信度
typedef struct {
    image_rect_t box;
    float confidence;
    float landmarks[5][2];   // 左眼/右眼/鼻尖/左嘴角/右嘴角
} face_detect_result;

#define MAX_FACE_COUNT 32

typedef struct {
    int count;
    face_detect_result results[MAX_FACE_COUNT];
} face_detect_result_list;

// 复⽤ YOLOv5 的 rknn_app_context_t 结构，接⼝⼀致
#include "yolov5.h"

int init_face_detect_model(const char* model_path, rknn_app_context_t* app_ctx);
int release_face_detect_model(rknn_app_context_t* app_ctx);
int inference_face_detect_model(rknn_app_context_t* app_ctx,
                                face_detect_result_list* fd_results);

#endif
