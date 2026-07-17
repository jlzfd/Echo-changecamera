// 双模型独立调试入口（YOLOv5 + 人脸检测，板端 headless 运行）
// 用法: ./dual_model_test <yolov5.rknn> <face_detect.rknn> [camera_dev]
//      camera_dev 默认 11（RV1106 MIPI CSI）

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
#include <linux/fb.h>
#include <time.h>
#include <signal.h>

#include "yolov5.h"
#include "face_detect.h"
#include "image_utils.h"
#include "image_drawing.h"

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "dma_alloc.cpp"

#define USE_DMA 0

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig) {
    g_running = 0;
}

void mapCoordinates(cv::Mat input, cv::Mat output, int *x, int *y) {
    float scaleX = (float)output.cols / (float)input.cols;
    float scaleY = (float)output.rows / (float)input.rows;
    *x = (int)((float)*x / scaleX);
    *y = (int)((float)*y / scaleY);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <yolov5_model.rknn> <face_model.rknn> [camera_dev]\n", argv[0]);
        printf("  camera_dev: default 11 (RV1106 MIPI CSI), 0 for USB\n");
        return -1;
    }

    const char *yolov5_model_path = argv[1];
    const char *face_model_path   = argv[2];
    int camera_dev = (argc >= 4) ? atoi(argv[3]) : 11;

    printf("=== Dual Model Test ===\n");
    printf("YOLOv5 model : %s\n", yolov5_model_path);
    printf("Face  model  : %s\n", face_model_path);
    printf("Camera dev  : /dev/video%d\n", camera_dev);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    // ── 1. 初始化 YOLOv5 模型 ──────────────────────
    rknn_app_context_t yolo_ctx;
    memset(&yolo_ctx, 0, sizeof(yolo_ctx));
    if (init_yolov5_model(yolov5_model_path, &yolo_ctx) != 0) {
        printf("Failed to init YOLOv5 model.\n");
        return -1;
    }
    init_post_process();
    printf("YOLOv5 model loaded: %dx%d\n", yolo_ctx.model_width, yolo_ctx.model_height);

    // ── 2. 初始化人脸检测模型 ──────────────────────
    rknn_app_context_t face_ctx;
    memset(&face_ctx, 0, sizeof(face_ctx));
    if (init_face_detect_model(face_model_path, &face_ctx) != 0) {
        printf("Failed to init face detection model.\n");
        release_yolov5_model(&yolo_ctx);
        return -1;
    }
    printf("Face  model loaded: %dx%d\n", face_ctx.model_width, face_ctx.model_height);

    // ── 3. LCD / framebuffer ───────────────────────
    int disp_flag = 0;
    int pixel_size = 0;
    int disp_width = 0, disp_height = 0;
    size_t screensize = 0;
    void* framebuffer = NULL;
    int framebuffer_fd = 0;
    cv::Mat disp;

    int fb = open("/dev/fb0", O_RDWR);
    if (fb >= 0) {
        struct fb_var_screeninfo fb_var;
        struct fb_fix_screeninfo fb_fix;
        ioctl(fb, FBIOGET_VSCREENINFO, &fb_var);
        ioctl(fb, FBIOGET_FSCREENINFO, &fb_fix);
        disp_width  = fb_var.xres;
        disp_height = fb_var.yres;
        pixel_size  = fb_var.bits_per_pixel / 8;
        screensize  = disp_width * disp_height * pixel_size;
        framebuffer = (uint8_t*)mmap(NULL, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
        disp_flag = 1;
        printf("LCD: %dx%d pixel_size=%d\n", disp_width, disp_height, pixel_size);
        if (pixel_size == 4)
            disp = cv::Mat(disp_height, disp_width, CV_8UC3);
        else if (pixel_size == 2)
            disp = cv::Mat(disp_height, disp_width, CV_16UC1);
#if USE_DMA
        dma_buf_alloc(RV1106_CMA_HEAP_PATH,
                      disp_width * disp_height * pixel_size,
                      &framebuffer_fd, (void **)&(disp.data));
#endif
    } else {
        printf("LCD: off (headless mode)\n");
        disp_width  = 640;
        disp_height = 480;
    }

    // ── 4. 打开摄像头 ──────────────────────────────
    cv::VideoCapture cap;
    cap.open(camera_dev);
    if (!cap.isOpened()) {
        printf("Failed to open /dev/video%d\n", camera_dev);
        release_face_detect_model(&face_ctx);
        release_yolov5_model(&yolo_ctx);
        if (disp_flag) { close(fb); munmap(framebuffer, screensize); }
        return -1;
    }
    cap.set(cv::CAP_PROP_FRAME_WIDTH,  640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
    printf("Camera opened: /dev/video%d\n", camera_dev);

    // ── 5. 主循环 ────────────────────────────────
    cv::Mat bgr;
    cv::Mat yolo_input(yolo_ctx.model_height, yolo_ctx.model_width,
                       CV_8UC3, yolo_ctx.input_mems[0]->virt_addr);
    cv::Mat face_input(face_ctx.model_height, face_ctx.model_width,
                       CV_8UC3, face_ctx.input_mems[0]->virt_addr);

    object_detect_result_list   yolo_results;
    face_detect_result_list     face_results;

    clock_t start_time, end_time;
    float fps = 0;
    int frame_count = 0;
    char text[64];

    printf("Dual model inference started. Ctrl+C to exit.\n");

    while (g_running) {
        start_time = clock();

        cap >> bgr;
        if (bgr.empty()) {
            usleep(10000);
            continue;
        }

        // ── YOLOv5 推理 ─────────────────────────
        printf("[%d] resize yolo %dx%d -> %dx%d\n", frame_count,
               bgr.cols, bgr.rows, yolo_ctx.model_width, yolo_ctx.model_height);
        fflush(stdout);
        cv::resize(bgr, yolo_input,
                   cv::Size(yolo_ctx.model_width, yolo_ctx.model_height),
                   0, 0, cv::INTER_LINEAR);
        printf("[%d] yolo infer...\n", frame_count);
        fflush(stdout);
        inference_yolov5_model(&yolo_ctx, &yolo_results);
        printf("[%d] yolo done, %d objects\n", frame_count, yolo_results.count);
        fflush(stdout);

        // ── 人脸检测推理 ─────────────────────────
        printf("[%d] resize face %dx%d -> %dx%d\n", frame_count,
               bgr.cols, bgr.rows, face_ctx.model_width, face_ctx.model_height);
        fflush(stdout);
        cv::resize(bgr, face_input,
                   cv::Size(face_ctx.model_width, face_ctx.model_height),
                   0, 0, cv::INTER_LINEAR);
        printf("[%d] face infer...\n", frame_count);
        fflush(stdout);
        inference_face_detect_model(&face_ctx, &face_results);
        printf("[%d] face done, %d faces\n", frame_count, face_results.count);
        fflush(stdout);

        // ── 输出结果 ────────────────────────────
        if (yolo_results.count > 0 || face_results.count > 0) {
            printf("[frame %d] YOLO=%d face=%d\n",
                   frame_count, yolo_results.count, face_results.count);

            for (int i = 0; i < yolo_results.count; i++) {
                object_detect_result *d = &yolo_results.results[i];
                printf("  YOLO[%d] %s box=(%d,%d,%d,%d) conf=%.2f\n",
                       i, coco_cls_to_name(d->cls_id),
                       d->box.left, d->box.top, d->box.right, d->box.bottom,
                       d->prop);
            }
            for (int i = 0; i < face_results.count; i++) {
                face_detect_result *f = &face_results.results[i];
                printf("  FACE[%d] box=(%d,%d,%d,%d) conf=%.2f\n",
                       i, f->box.left, f->box.top, f->box.right, f->box.bottom,
                       f->confidence);
            }
        }

        // ── LCD 显示 ───────────────────────────
        if (disp_flag) {
            cv::Mat bgr_disp = bgr.clone();

            // 绘制 YOLO 框
            for (int i = 0; i < yolo_results.count; i++) {
                object_detect_result *d = &yolo_results.results[i];
                int x1 = d->box.left, y1 = d->box.top;
                int x2 = d->box.right, y2 = d->box.bottom;
                mapCoordinates(bgr_disp, yolo_input, &x1, &y1);
                mapCoordinates(bgr_disp, yolo_input, &x2, &y2);
                cv::rectangle(bgr_disp, cv::Point(x1, y1), cv::Point(x2, y2),
                              cv::Scalar(0, 255, 0), 2);
                snprintf(text, sizeof(text), "%s %.1f%%",
                         coco_cls_to_name(d->cls_id), d->prop * 100);
                cv::putText(bgr_disp, text, cv::Point(x1, y1 - 8),
                            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1);
            }

            // 绘制人脸框
            for (int i = 0; i < face_results.count; i++) {
                face_detect_result *f = &face_results.results[i];
                int x1 = f->box.left, y1 = f->box.top;
                int x2 = f->box.right, y2 = f->box.bottom;
                mapCoordinates(bgr_disp, face_input, &x1, &y1);
                mapCoordinates(bgr_disp, face_input, &x2, &y2);
                cv::rectangle(bgr_disp, cv::Point(x1, y1), cv::Point(x2, y2),
                              cv::Scalar(255, 0, 0), 2);
            }

            // FPS
            snprintf(text, sizeof(text), "YOLO:%d FACE:%d fps:%.1f",
                     yolo_results.count, face_results.count, fps);
            cv::putText(bgr_disp, text, cv::Point(0, 20),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1);

            cv::resize(bgr_disp, bgr_disp, cv::Size(disp_width, disp_height), 0, 0, cv::INTER_LINEAR);
            if (pixel_size == 4)
                cv::cvtColor(bgr_disp, disp, cv::COLOR_BGR2BGRA);
            else if (pixel_size == 2)
                cv::cvtColor(bgr_disp, disp, cv::COLOR_BGR2BGR565);
            memcpy(framebuffer, disp.data, disp_width * disp_height * pixel_size);
#if USE_DMA
            dma_sync_cpu_to_device(framebuffer_fd);
#endif
        }

        end_time = clock();
        fps = (float)CLOCKS_PER_SEC / (float)(end_time - start_time);
        frame_count++;
    }

    // ── 6. 清理 ────────────────────────────────
    printf("\nExiting. Total frames: %d\n", frame_count);

    cap.release();
    deinit_post_process();
    release_face_detect_model(&face_ctx);
    release_yolov5_model(&yolo_ctx);

    if (disp_flag) {
        close(fb);
        munmap(framebuffer, screensize);
#if USE_DMA
        dma_buf_free(disp_width * disp_height * pixel_size, &framebuffer_fd, disp.data);
#endif
    }

    return 0;
}
