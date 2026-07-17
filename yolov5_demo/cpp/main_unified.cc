// Unified pipeline: Camera → RGA → Arena → YOLO + Face → LCD
//
// Replaces cv::resize() with RGA hardware acceleration.
// YOLO and Face share the same arena CMA buffer (one RGA call, not two).
// KWS interface left for AIChat_demo scheduling.
//
// Usage: ./unified_test <yolov5.rknn> <face_detect.rknn> [camera_dev] [face_interval]
//   camera_dev:   default 11 (RV1106 MIPI CSI)
//   face_interval: run face detection every N frames (default 3, 0=off)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <time.h>
#include <signal.h>

#include "yolov5.h"
#include "face_detect.h"
#include "npu_memory_reuse.h"
#include "image_utils.h"
#include "image_drawing.h"
#include "dma_alloc.h"

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

// ── Scheduler config ────────────────────────────────

typedef struct {
    int face_interval;       // run face detect every N frames (1 = every frame)
    int face_skip_count;     // internal counter
    int yolo_frame_count;
    int face_frame_count;
    float yolo_fps;
    float face_fps;
    float total_fps;
    clock_t last_report;
} scheduler_t;

static void scheduler_init(scheduler_t* s, int face_interval) {
    memset(s, 0, sizeof(*s));
    s->face_interval = face_interval > 0 ? face_interval : 1;
    s->face_skip_count = s->face_interval - 1; // run on first frame
    s->last_report = clock();
}

// Returns true if face detection should run this frame.
static bool scheduler_should_run_face(scheduler_t* s) {
    s->face_skip_count++;
    if (s->face_skip_count >= s->face_interval) {
        s->face_skip_count = 0;
        return true;
    }
    return false;
}

static void scheduler_report(scheduler_t* s) {
    clock_t now = clock();
    float elapsed = (float)(now - s->last_report) / (float)CLOCKS_PER_SEC;
    if (elapsed >= 5.0f) {
        s->yolo_fps = (float)s->yolo_frame_count / elapsed;
        s->face_fps = (float)s->face_frame_count / elapsed;
        s->total_fps = s->yolo_fps; // total = yolo rate (face is subset)
        printf("[Sched] %.1ffps total | YOLO=%d(%.1ffps) Face=%d(%.1ffps)\n",
               s->total_fps, s->yolo_frame_count, s->yolo_fps,
               s->face_frame_count, s->face_fps);
        s->yolo_frame_count = 0;
        s->face_frame_count = 0;
        s->last_report = now;
    }
}

// ── RGA helper: CPU rgb → CMA buffer (hardware resize via convert_image) ─

// Writes resized RGB to the arena CMA buffer via RGA hardware.
// src_rgb: 640x480 RGB (cv::Mat data, CPU memory)
// dst: arena CMA buffer (fd-backed, 640x640 RGB)
static int rga_resize_to_arena(unsigned char* src_rgb, int src_w, int src_h,
                               image_buffer_t* dst) {
    image_buffer_t src;
    memset(&src, 0, sizeof(src));
    src.width   = src_w;
    src.height  = src_h;
    src.width_stride  = src_w;
    src.height_stride = src_h;
    src.format  = IMAGE_FORMAT_RGB888;
    src.virt_addr = src_rgb;
    src.fd      = -1;          // CPU memory, RGA uses virt addr
    src.size    = src_w * src_h * 3;

    return convert_image(&src, dst, NULL, NULL, 0);
}

// ── LCD helper ──────────────────────────────────────

typedef struct {
    int   fd;
    int   width;
    int   height;
    int   pixel_size;
    size_t screensize;
    void* framebuffer;
    cv::Mat disp;
    bool  active;
} lcd_t;

static int lcd_init(lcd_t* lcd) {
    memset(lcd, 0, sizeof(*lcd));
    lcd->fd = open("/dev/fb0", O_RDWR);
    if (lcd->fd < 0) {
        printf("[LCD] off (headless mode)\n");
        lcd->width  = 640;
        lcd->height = 480;
        return 0;
    }

    struct fb_var_screeninfo fb_var;
    struct fb_fix_screeninfo fb_fix;
    ioctl(lcd->fd, FBIOGET_VSCREENINFO, &fb_var);
    ioctl(lcd->fd, FBIOGET_FSCREENINFO, &fb_fix);
    lcd->width  = fb_var.xres;
    lcd->height = fb_var.yres;
    lcd->pixel_size = fb_var.bits_per_pixel / 8;
    lcd->screensize = lcd->width * lcd->height * lcd->pixel_size;
    lcd->framebuffer = (uint8_t*)mmap(NULL, lcd->screensize,
                                      PROT_READ | PROT_WRITE, MAP_SHARED,
                                      lcd->fd, 0);
    if (lcd->pixel_size == 4)
        lcd->disp = cv::Mat(lcd->height, lcd->width, CV_8UC3);
    else if (lcd->pixel_size == 2)
        lcd->disp = cv::Mat(lcd->height, lcd->width, CV_16UC1);

    lcd->active = true;
    printf("[LCD] %dx%d pixel_size=%d\n", lcd->width, lcd->height, lcd->pixel_size);
    return 0;
}

static void lcd_show(lcd_t* lcd, cv::Mat& bgr_disp, int yolo_count, int face_count) {
    if (!lcd->active) return;

    char text[64];
    snprintf(text, sizeof(text), "YOLO:%d FACE:%d", yolo_count, face_count);
    cv::putText(bgr_disp, text, cv::Point(0, 20),
                cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1);

    cv::resize(bgr_disp, bgr_disp,
               cv::Size(lcd->width, lcd->height), 0, 0, cv::INTER_LINEAR);

    if (lcd->pixel_size == 4) {
        cv::cvtColor(bgr_disp, lcd->disp, cv::COLOR_BGR2BGRA);
    } else if (lcd->pixel_size == 2) {
        cv::cvtColor(bgr_disp, lcd->disp, cv::COLOR_BGR2BGR565);
    }
    memcpy(lcd->framebuffer, lcd->disp.data, lcd->screensize);
}

static void lcd_deinit(lcd_t* lcd) {
    if (lcd->active) {
        close(lcd->fd);
        munmap(lcd->framebuffer, lcd->screensize);
    }
}

// ── Drawing helpers ─────────────────────────────────

static void draw_yolo_boxes(cv::Mat& img, object_detect_result_list* results,
                            int model_w, int model_h) {
    char text[64];
    float scaleX = (float)img.cols / (float)model_w;
    float scaleY = (float)img.rows / (float)model_h;
    for (int i = 0; i < results->count; i++) {
        object_detect_result* d = &results->results[i];
        int x1 = (int)(d->box.left   * scaleX);
        int y1 = (int)(d->box.top    * scaleY);
        int x2 = (int)(d->box.right  * scaleX);
        int y2 = (int)(d->box.bottom * scaleY);
        cv::rectangle(img, cv::Point(x1, y1), cv::Point(x2, y2),
                      cv::Scalar(0, 255, 0), 2);
        snprintf(text, sizeof(text), "%s %.1f%%",
                 coco_cls_to_name(d->cls_id), d->prop * 100);
        cv::putText(img, text, cv::Point(x1, y1 - 8),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1);
    }
}

static void draw_face_boxes(cv::Mat& img, face_detect_result_list* results,
                            int model_w, int model_h) {
    float scaleX = (float)img.cols / (float)model_w;
    float scaleY = (float)img.rows / (float)model_h;
    for (int i = 0; i < results->count; i++) {
        face_detect_result* f = &results->results[i];
        int x1 = (int)(f->box.left   * scaleX);
        int y1 = (int)(f->box.top    * scaleY);
        int x2 = (int)(f->box.right  * scaleX);
        int y2 = (int)(f->box.bottom * scaleY);
        cv::rectangle(img, cv::Point(x1, y1), cv::Point(x2, y2),
                      cv::Scalar(255, 0, 0), 2);
    }
}

// ── Main ────────────────────────────────────────────

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig) { g_running = 0; }

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage: %s <yolov5.rknn> <face_detect.rknn> [camera_dev] [face_interval]\n",
               argv[0]);
        printf("  camera_dev:    default 11 (RV1106 MIPI CSI)\n");
        printf("  face_interval: run face every N frames (default 3, 0=off)\n");
        return -1;
    }

    const char* yolo_path  = argv[1];
    const char* face_path  = argv[2];
    int camera_dev         = (argc >= 4) ? atoi(argv[3]) : 11;
    int face_interval      = (argc >= 5) ? atoi(argv[4]) : 3;

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    printf("=== Unified Pipeline (RGA + Arena + Dual Model) ===\n");
    printf("YOLO: %s\n", yolo_path);
    printf("Face: %s\n", face_path);
    printf("Camera: /dev/video%d\n", camera_dev);
    printf("Face interval: every %d frame(s)\n", face_interval);

    // ── 1. Init YOLO ────────────────────────────────
    rknn_app_context_t yolo_ctx;
    memset(&yolo_ctx, 0, sizeof(yolo_ctx));
    if (init_yolov5_model(yolo_path, &yolo_ctx) != 0) {
        fprintf(stderr, "Failed to init YOLO model.\n");
        return -1;
    }
    init_post_process();
    printf("YOLO loaded: %dx%d\n", yolo_ctx.model_width, yolo_ctx.model_height);

    // ── 2. Init Face ────────────────────────────────
    rknn_app_context_t face_ctx;
    memset(&face_ctx, 0, sizeof(face_ctx));
    if (init_face_detect_model(face_path, &face_ctx) != 0) {
        fprintf(stderr, "Failed to init face model.\n");
        release_yolov5_model(&yolo_ctx);
        return -1;
    }
    printf("Face loaded: %dx%d\n", face_ctx.model_width, face_ctx.model_height);

    // ── 3. Arena: create → register → allocate → adopt ─
    npu_arena_t* arena = npu_arena_create();
    npu_set_global_arena(arena);

    npu_arena_register(arena, yolo_ctx.rknn_ctx,
                       yolo_ctx.input_attrs,  yolo_ctx.io_num.n_input,
                       yolo_ctx.output_attrs, yolo_ctx.io_num.n_output);
    npu_arena_register(arena, face_ctx.rknn_ctx,
                       face_ctx.input_attrs,  face_ctx.io_num.n_input,
                       face_ctx.output_attrs, face_ctx.io_num.n_output);
    npu_arena_allocate(arena, yolo_ctx.rknn_ctx);

    npu_arena_adopt_yolov5(arena, &yolo_ctx);
    npu_arena_adopt_yolov5(arena, &face_ctx);
    printf("Arena: %zu bytes, %zu saved\n",
           npu_arena_total_size(arena), npu_arena_saved_bytes(arena));

    // ── 4. LCD ──────────────────────────────────────
    lcd_t lcd;
    lcd_init(&lcd);

    // ── 6. RGA destination descriptor ───────────────
    // Reuse arena CMA buffer as RGA output (fd-backed, no CPU copy).
    // Both YOLO and Face share this buffer → only ONE RGA call per frame.
    image_buffer_t rga_dst;
    memset(&rga_dst, 0, sizeof(rga_dst));
    rga_dst.width   = yolo_ctx.model_width;   // 640
    rga_dst.height  = yolo_ctx.model_height;  // 640
    rga_dst.width_stride  = yolo_ctx.model_width;
    rga_dst.height_stride = yolo_ctx.model_height;
    rga_dst.format  = IMAGE_FORMAT_RGB888;
    rknn_tensor_mem* arena_in = npu_arena_input_mem(arena);
    rga_dst.virt_addr = (unsigned char*)arena_in->virt_addr;
    rga_dst.fd      = arena_in->fd;
    rga_dst.size    = yolo_ctx.model_width * yolo_ctx.model_height * 3;

    cv::Mat bgr, rgb;
    cv::Mat rgb_src(480, 640, CV_8UC3);

    // ── 5. Camera ───────────────────────────────────
    cv::VideoCapture cap;
    cap.open(camera_dev);
    if (!cap.isOpened()) {
        fprintf(stderr, "Failed to open /dev/video%d\n", camera_dev);
        goto cleanup;
    }
    cap.set(cv::CAP_PROP_FRAME_WIDTH,  640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
    printf("Camera opened: /dev/video%d\n", camera_dev);

    // ── 7. Main loop ────────────────────────────────
    object_detect_result_list yolo_results;
    face_detect_result_list   face_results;
    scheduler_t sched;
    scheduler_init(&sched, face_interval);

    printf("\n=== Running (Ctrl+C to stop) ===\n\n");

    while (g_running) {
        cap >> bgr;
        if (bgr.empty()) { usleep(10000); continue; }

        // BGR→RGB (CPU, ~0.9ms for 640x480 — future: V4L2 gives RGB natively)
        cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
        rgb_src = rgb; // keep ref for RGA src.virt_addr

        // RGA hardware resize: 640x480 RGB → 640x640 RGB (arena CMA, DMA)
        int rga_ret = rga_resize_to_arena(rgb_src.data, 640, 480, &rga_dst);
        if (rga_ret != 0) {
            // RGA failed → fallback to CPU resize
            rknn_tensor_mem* arena_in = npu_arena_input_mem(arena);
            cv::Mat fallback(yolo_ctx.model_height, yolo_ctx.model_width,
                             CV_8UC3, arena_in->virt_addr);
            cv::resize(rgb, fallback,
                       cv::Size(yolo_ctx.model_width, yolo_ctx.model_height),
                       0, 0, cv::INTER_LINEAR);
            dma_sync_cpu_to_device(arena_in->fd);
        }

        // ── YOLO inference ──────────────────────────
        ARENA_PROFILE_RUN_BEGIN();
        npu_arena_bind_yolov5(arena, &yolo_ctx);
        inference_yolov5_model(&yolo_ctx, &yolo_results);
        ARENA_PROFILE_RUN_END("YOLO");
        sched.yolo_frame_count++;

        // ── Face inference (scheduled) ──────────────
        bool run_face = scheduler_should_run_face(&sched);
        if (run_face) {
            ARENA_PROFILE_RUN_BEGIN();
            npu_arena_bind_yolov5(arena, &face_ctx);
            inference_face_detect_model(&face_ctx, &face_results);
            ARENA_PROFILE_RUN_END("Face");
            sched.face_frame_count++;
        }

        // ── LCD display ─────────────────────────────
        if (lcd.active) {
            cv::Mat bgr_disp = bgr.clone();

            draw_yolo_boxes(bgr_disp, &yolo_results,
                            yolo_ctx.model_width, yolo_ctx.model_height);
            if (run_face) {
                draw_face_boxes(bgr_disp, &face_results,
                                face_ctx.model_width, face_ctx.model_height);
            }

            lcd_show(&lcd, bgr_disp, yolo_results.count,
                     run_face ? face_results.count : -1);
        }

        // ── Console output ──────────────────────────
        if (yolo_results.count > 0 || (run_face && face_results.count > 0)) {
            printf("[frame] YOLO=%d", yolo_results.count);
            if (run_face) printf(" Face=%d", face_results.count);
            printf("\n");
        }

        scheduler_report(&sched);
    }

    printf("\nExiting.\n");

cleanup:
    cap.release();
    lcd_deinit(&lcd);
    npu_arena_destroy(arena);
    release_face_detect_model(&face_ctx);
    release_yolov5_model(&yolo_ctx);
    deinit_post_process();
    printf("Cleanup done.\n");
    return 0;
}
