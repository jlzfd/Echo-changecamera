// 人脸检测独立调试入口（板端 headless 运行）
// 用法: ./face_detect_test <face_model.rknn> [test_image.jpg] [camera_dev]
//      camera_dev 默认 11（RV1106 MIPI CSI），也可指定 0

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <signal.h>

#include "face_detect.h"
#include "image_utils.h"
#include "image_drawing.h"

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig) {
    g_running = 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <face_model.rknn> [test_image.jpg] [camera_dev]\n", argv[0]);
        printf("  With image: runs single inference on the image.\n");
        printf("  Without image: opens camera for live face detection (dev=11 default).\n");
        printf("  camera_dev: e.g. 11 for RV1106 MIPI CSI, 0 for USB camera.\n");
        return -1;
    }

    const char* model_path = argv[1];
    // 判断是图片路径还是摄像头设备号
    int camera_dev = 11;  // RV1106 默认
    const char* image_path = nullptr;
    bool is_camera_mode = true;

    if (argc >= 3) {
        // 尝试检测第三个参数是否是摄像头设备号（纯数字）
        char* endptr = nullptr;
        long val = strtol(argv[2], &endptr, 10);
        if (endptr != argv[2] && *endptr == '\0' && endptr != argv[2]) {
            // 纯数字 → 摄像头设备号
            camera_dev = (int)val;
        } else {
            // 非纯数字 → 图片路径
            image_path = argv[2];
            is_camera_mode = false;
        }
    }

    // 初始化人脸检测模型
    rknn_app_context_t face_ctx;
    memset(&face_ctx, 0, sizeof(face_ctx));

    if (init_face_detect_model(model_path, &face_ctx) != 0) {
        printf("Failed to init face detection model.\n");
        return -1;
    }

    int model_w = face_ctx.model_width;
    int model_h = face_ctx.model_height;
    printf("Face model: %dx%dx%d\n", model_w, model_h, face_ctx.model_channel);

    face_detect_result_list fd_results;

    // ── 模式 1：单张图片测试 ──────────────────────
    if (!is_camera_mode) {
        cv::Mat img = cv::imread(image_path);
        if (img.empty()) {
            printf("Failed to read image: %s\n", image_path);
            release_face_detect_model(&face_ctx);
            return -1;
        }

        cv::Mat face_input(model_h, model_w, CV_8UC3, face_ctx.input_mems[0]->virt_addr);
        cv::resize(img, face_input, cv::Size(model_w, model_h), 0, 0, cv::INTER_LINEAR);

        if (inference_face_detect_model(&face_ctx, &fd_results) != 0) {
            printf("Inference failed.\n");
            release_face_detect_model(&face_ctx);
            return -1;
        }

        printf("Detected %d face(s):\n", fd_results.count);
        for (int i = 0; i < fd_results.count; i++) {
            face_detect_result* r = &fd_results.results[i];
            printf("  [%d] box=(%d,%d,%d,%d) conf=%.2f\n",
                   i, r->box.left, r->box.top, r->box.right, r->box.bottom,
                   r->confidence);
            printf("      landmarks: (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f)\n",
                   r->landmarks[0][0], r->landmarks[0][1],
                   r->landmarks[1][0], r->landmarks[1][1],
                   r->landmarks[2][0], r->landmarks[2][1],
                   r->landmarks[3][0], r->landmarks[3][1],
                   r->landmarks[4][0], r->landmarks[4][1]);
            cv::rectangle(img, cv::Point(r->box.left, r->box.top),
                          cv::Point(r->box.right, r->box.bottom),
                          cv::Scalar(0, 255, 0), 2);
            for (int k = 0; k < 5; k++) {
                cv::circle(img, cv::Point((int)r->landmarks[k][0], (int)r->landmarks[k][1]),
                           2, cv::Scalar(255, 0, 0), -1);
            }
        }
        cv::imwrite("face_detect_output.jpg", img);
        printf("Output saved to face_detect_output.jpg\n");

        release_face_detect_model(&face_ctx);
        return 0;
    }

    // ── 模式 2：摄像头实时检测（headless） ────────
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    cv::VideoCapture cap;
    cap.open(camera_dev);
    if (!cap.isOpened()) {
        printf("Failed to open camera device /dev/video%d\n", camera_dev);
        release_face_detect_model(&face_ctx);
        return -1;
    }
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);

    cv::Mat bgr;
    cv::Mat face_input(model_h, model_w, CV_8UC3, face_ctx.input_mems[0]->virt_addr);

    printf("Live face detection started on /dev/video%d. Ctrl+C to exit.\n", camera_dev);

    int frame_count = 0;
    while (g_running) {
        cap >> bgr;
        if (bgr.empty()) {
            usleep(10000);
            continue;
        }

        cv::resize(bgr, face_input, cv::Size(model_w, model_h), 0, 0, cv::INTER_LINEAR);

        if (inference_face_detect_model(&face_ctx, &fd_results) == 0) {
            if (fd_results.count > 0) {
                printf("[frame %d] Detected %d face(s):\n", frame_count, fd_results.count);
                for (int i = 0; i < fd_results.count; i++) {
                    face_detect_result* r = &fd_results.results[i];
                    printf("  [%d] box=(%d,%d,%d,%d) conf=%.2f\n",
                           i, r->box.left, r->box.top, r->box.right, r->box.bottom,
                           r->confidence);
                }
            }
        }
        frame_count++;
        usleep(30000);
    }

    printf("\nExiting. Total frames: %d\n", frame_count);
    cap.release();
    release_face_detect_model(&face_ctx);
    return 0;
}
