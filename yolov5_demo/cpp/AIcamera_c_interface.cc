// Copyright (c) 2023 by Rockchip Electronics Co., Ltd. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*-------------------------------------------
                Includes
-------------------------------------------*/
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <vector>
#include <thread>
#include <mutex> // 寮曞叆 C++ mutex 浠ョ畝鍖?curl 鍒濆鍖?
#include "yolov5.h"
#include "image_utils.h"
#include "file_utils.h"
#include "image_drawing.h"

#include <unistd.h>   
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <time.h>
#include <curl/curl.h>

//opencv
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/opencv.hpp>

#include "AIcamera_c_interface.h"
#include "face_detect.h"
#include "npu_memory_reuse.h"
#include "v4l2_capture.h"
#include "dma_alloc.h"
#include "hw_jpeg_encoder.h"
#include "/home/ubuntu/Echo-Mate-main/Demo/AIChat_demo/Client/Application/Application.h"

struct AICameraArgs {
    std::string model_path;
    Application* app_instance;
    int yolo_active;
    int face_interval;
};

// ── Scheduler (same design as main_zero_copy.cc) ──────
typedef struct {
    int face_interval;
    int face_skip_count;
    int yolo_frame_count;
    int face_frame_count;
    clock_t last_report;
} sched_t;

static void sched_init(sched_t* s, int face_interval) {
    memset(s, 0, sizeof(*s));
    s->face_interval = face_interval > 0 ? face_interval : 1;
    s->face_skip_count = s->face_interval - 1;
    s->last_report = clock();
}

static bool sched_should_run_face(sched_t* s) {
    s->face_skip_count++;
    if (s->face_skip_count >= s->face_interval) {
        s->face_skip_count = 0;
        return true;
    }
    return false;
}

static void sched_report(sched_t* s) {
    clock_t now = clock();
    float elapsed = (float)(now - s->last_report) / (float)CLOCKS_PER_SEC;
    if (elapsed >= 5.0f) {
        printf("[Sched] YOLO=%d Face=%d in %.1fs\n",
               s->yolo_frame_count, s->face_frame_count, elapsed);
        s->yolo_frame_count = 0;
        s->face_frame_count = 0;
        s->last_report = now;
    }
}

// ── Zero-copy pipeline state ─────────────────────────
static v4l2_capture_t g_v4l2_cap;
static bool g_use_zero_copy = false;
static image_buffer_t g_rga_src;
static image_buffer_t g_rga_dst;
static image_buffer_t g_rga_lcd;    // LCD output: BGR565 320x240
static int g_lcd_fd = -1;           // CMA fd for LCD buffer
static int g_yolo_model_w = 0;
static int g_yolo_model_h = 0;

// ── Double-buffer for zero-copy frame delivery (CMA-backed) ─
// Camera alternates between two CMA buffers. RGA converts arena
// RGB→BGR directly into the active CMA buffer (fd→fd). The state
// thread holds a cv::Mat wrapper (shared refcount) pointing to the
// same CMA virt_addr while the camera writes to the alternate buffer.
static unsigned char* g_cam_buf_virt[2] = {NULL, NULL};
static int g_cam_buf_fd[2] = {-1, -1};
static image_buffer_t g_rga_bgr_dst[2];  // RGA descriptors for BGR output
static int g_cam_buf_idx = 0;

void mapCoordinates(cv::Mat input, cv::Mat output, int *x, int *y);

static int disp_width = 320;
static int disp_height = 240;

rknn_app_context_t rknn_app_ctx;
rknn_app_context_t face_detect_ctx;
bool  face_model_loaded = false;
static pthread_mutex_t face_model_mutex = PTHREAD_MUTEX_INITIALIZER;
uint8_t* yolo_pic_buf = NULL; // 鍒濆鍖栦负 NULL
size_t yolo_pic_buf_size;

static pthread_t ai_camera_thread;
static std::atomic_bool ai_camera_running{false};
static std::atomic_bool ai_camera_stop{false};
static Application* ai_camera_consumer = nullptr;

// 浜掓枼閿侊細鐢ㄤ簬淇濇姢杩愯鐘舵€佸拰鍥惧儚缂撳啿鍖虹殑骞跺彂璇诲啓
static pthread_mutex_t running_mutex = PTHREAD_MUTEX_INITIALIZER;  
static pthread_mutex_t pic_buf_mutex = PTHREAD_MUTEX_INITIALIZER; // 鏂板锛氬浘鍍忕紦鍐插尯浜掓枼閿?
static pthread_mutex_t consumer_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::once_flag curl_init_flag; // 鐢ㄤ簬淇濊瘉 curl_global_init 鍙墽琛屼竴娆?
bool is_person_detected(object_detect_result_list od_results) {
    for (int i = 0; i < od_results.count; i++) {
        // 鍦?COCO 鏁版嵁闆嗕腑锛? 閫氬父鏄?person
        if (od_results.results[i].cls_id == 0 && od_results.results[i].prop > 0.5) {
            return true;
        }
    }
    return false;
}

static void set_ai_camera_consumer(Application* app) {
    pthread_mutex_lock(&consumer_mutex);
    ai_camera_consumer = app;
    pthread_mutex_unlock(&consumer_mutex);
}

static void clear_ai_camera_consumer_if(Application* app) {
    pthread_mutex_lock(&consumer_mutex);
    if (ai_camera_consumer == app) {
        ai_camera_consumer = nullptr;
    }
    pthread_mutex_unlock(&consumer_mutex);
}

static Application* get_ai_camera_consumer() {
    pthread_mutex_lock(&consumer_mutex);
    Application* app = ai_camera_consumer;
    pthread_mutex_unlock(&consumer_mutex);
    return app;
}

static void reset_ai_camera_state_after_thread_failure() {
    pthread_mutex_lock(&pic_buf_mutex);
    if (yolo_pic_buf != NULL) {
        dma_buf_free(yolo_pic_buf_size, &g_lcd_fd, yolo_pic_buf);
        yolo_pic_buf = NULL;
    }
    yolo_pic_buf_size = 0;
    pthread_mutex_unlock(&pic_buf_mutex);

    set_ai_camera_consumer(nullptr);
    ai_camera_stop.store(false);
    ai_camera_running.store(false);
}

static const std::string base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const unsigned char* bytes_to_encode, unsigned int in_len) {
    std::string ret;
    int i = 0, j = 0;
    unsigned char char_array_3[3], char_array_4[4];
    while (in_len--) {
        char_array_3[i++] = *(bytes_to_encode++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            for(i = 0; (i <4) ; i++) ret += base64_chars[char_array_4[i]];
            i = 0;
        }
    }
    if (i) {
        for(j = i; j < 3; j++) char_array_3[j] = '\0';
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        for (j = 0; (j < i + 1); j++) ret += base64_chars[char_array_4[j]];
        while((i++ < 3)) ret += '=';
    }
    return ret;
}

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

void upload_to_aliyun(cv::Mat frame) {
    cv::Mat small_frame;
    cv::resize(frame, small_frame, cv::Size(448, 448));
    std::vector<uchar> buf;
    cv::imencode(".jpg", small_frame, buf);
    std::string img_base64 = base64_encode(buf.data(), buf.size());

    // 2. 鏋勯€?JSON (娉ㄦ剰锛氳鍔″繀鏇挎崲涓轰綘鐪熷疄鐨?API Key)
    std::string apiKey = "sk-898ad55d5c9346b2bfb13b13009cd266"; 
    std::string url = "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation";
    
    std::string jsonPayload = "{\"model\": \"qwen-vl-plus\", \"input\": {\"messages\": [{\"role\": \"user\", \"content\": [{\"image\": \"data:image/jpeg;base64," + img_base64 + "\"}, {\"text\": \"Describe the current frame briefly in Chinese.\"}]}]}}";

    CURL* curl = curl_easy_init();
    if(curl) {
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, ("Authorization: Bearer " + apiKey).c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json");

        std::string response_string;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonPayload.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);

        CURLcode res = curl_easy_perform(curl);
        if(res == CURLE_OK) {
            printf("闃块噷浜戝洖澶? %s\n", response_string.c_str());
            // 杩欓噷鍙互杩涗竴姝ヨВ鏋?response_string 骞惰Е鍙?TTS 璇煶鎾斁
        } else {
            printf("璇锋眰澶辫触: %s\n", curl_easy_strerror(res));
        }
        
        curl_slist_free_all(headers); // 淇鍐呭瓨娉勬紡锛氶噴鏀?headers
        curl_easy_cleanup(curl);
    }
}

/*-------------------------------------------
              Zero-Copy Inference Loop
-------------------------------------------*/
static void* _inference_loop_zero_copy(void* arg) {
    printf("[AI Camera ZC] thread started\n");

    AICameraArgs* args = (AICameraArgs*)arg;
    set_ai_camera_consumer(args->app_instance);
    Application* app = args->app_instance;
    int yolo_active = args->yolo_active;
    int face_interval = args->face_interval;
    delete args;

    npu_arena_t* arena = npu_get_global_arena();
    if (!arena || npu_arena_state(arena) != ARENA_ALLOCATED) {
        fprintf(stderr, "[AI Camera ZC] arena not allocated, abort\n");
        pthread_detach(pthread_self());
        reset_ai_camera_state_after_thread_failure();
        return nullptr;
    }
    rknn_tensor_mem* arena_in = npu_arena_input_mem(arena);

    // Per-frame result buffers
    object_detect_result_list od_results;
    face_detect_result_list fd_results;

    // Motion detection state
    cv::Mat prev_gray;
    int consecutive_motion_frames = 0;

    // Scheduler
    sched_t sched;
    sched_init(&sched, face_interval);

    while (!ai_camera_stop.load()) {
        // 1. V4L2 get frame (DMA-BUF fd, zero copy)
        int isp_fd, buf_idx;
        buf_idx = v4l2_capture_get_frame(&g_v4l2_cap, &isp_fd);
        if (buf_idx < 0) { usleep(10000); continue; }

        // 2. RGA NV12(fd) → RGB(arena fd), zero CPU copy
        g_rga_src.fd = isp_fd;
        int rga_ret = convert_image(&g_rga_src, &g_rga_dst, NULL, NULL, 0);
        if (rga_ret != 0) {
            v4l2_capture_put_frame(&g_v4l2_cap, buf_idx);
            continue;
        }

        // 3. Return V4L2 buffer immediately — isp_fd no longer needed after RGA
        v4l2_capture_put_frame(&g_v4l2_cap, buf_idx);

        // 4. Sync: ensure NPU sees RGA output in CMA
        dma_sync_device_to_cpu(arena_in->fd);

        // 5. YOLO inference (if active)
        if (yolo_active) {
            npu_arena_lock(arena);
            npu_arena_bind_yolov5(arena, &rknn_app_ctx);
            memset(&od_results, 0, sizeof(od_results));
            inference_yolov5_model(&rknn_app_ctx, &od_results);
            npu_arena_unlock(arena);

            sched.yolo_frame_count++;

            if (app && is_person_detected(od_results)) {
                app->SubmitActiveVisionEvent("person_detected", 0.7f, 5000);
            }
        }

        // 6. Face inference (scheduled)
        bool run_face = false;
        {
            pthread_mutex_lock(&face_model_mutex);
            run_face = face_model_loaded && face_interval > 0;
            pthread_mutex_unlock(&face_model_mutex);
        }
        if (run_face && sched_should_run_face(&sched)) {
            npu_arena_lock(arena);
            npu_arena_bind_yolov5(arena, &face_detect_ctx);
            memset(&fd_results, 0, sizeof(fd_results));
            int fd_ret = inference_face_detect_model(&face_detect_ctx, &fd_results);
            npu_arena_unlock(arena);

            sched.face_frame_count++;

            if (fd_ret == 0 && fd_results.count > 0 && app) {
                float max_conf = 0;
                for (int i = 0; i < fd_results.count; i++) {
                    if (fd_results.results[i].confidence > max_conf)
                        max_conf = fd_results.results[i].confidence;
                }
                app->SubmitActiveVisionEvent("face_detected", max_conf, 8000);
            }
        }

        // 7. Draw YOLO boxes on arena RGB (before motion/frame delivery/LCD)
        cv::Mat arena_rgb(g_yolo_model_h, g_yolo_model_w, CV_8UC3, arena_in->virt_addr);
        if (yolo_active) {
            for (int i = 0; i < od_results.count; i++) {
                object_detect_result* det = &od_results.results[i];
                cv::rectangle(arena_rgb,
                    cv::Point(det->box.left, det->box.top),
                    cv::Point(det->box.right, det->box.bottom),
                    cv::Scalar(0, 255, 0), 2);
            }
        }

        // 8. Motion detection + Frame delivery
        if (app) {
            cv::Mat gray;
            cv::cvtColor(arena_rgb, gray, cv::COLOR_RGB2GRAY);
            cv::resize(gray, gray, cv::Size(320, 240), 0, 0, cv::INTER_LINEAR);
            cv::GaussianBlur(gray, gray, cv::Size(5, 5), 0);

            if (!prev_gray.empty()) {
                cv::Mat diff;
                cv::absdiff(prev_gray, gray, diff);
                cv::threshold(diff, diff, 25, 255, cv::THRESH_BINARY);
                double motion_ratio = (double)cv::countNonZero(diff) / (double)(diff.rows * diff.cols);
                if (motion_ratio > 0.08) {
                    consecutive_motion_frames++;
                } else {
                    consecutive_motion_frames = 0;
                }
                if (consecutive_motion_frames >= 3) {
                    app->SubmitActiveVisionEvent("motion_detected", (float)motion_ratio, 5000);
                    consecutive_motion_frames = 0;
                }
            }
            prev_gray = gray.clone();

            // Frame delivery: RGA arena_rgb(RGB fd) → CMA BGR(fd), zero CPU copy
        {
            int idx = g_cam_buf_idx;
            convert_image(&g_rga_dst, &g_rga_bgr_dst[idx], NULL, NULL, 0);
            dma_sync_device_to_cpu(g_cam_buf_fd[idx]);
            cv::Mat bgr(g_yolo_model_h, g_yolo_model_w, CV_8UC3, g_cam_buf_virt[idx]);
            app->UpdateLatestFrame(bgr, g_cam_buf_fd[idx]);
            g_cam_buf_idx ^= 1;
        }
        }

        // 9. LCD: RGA arena_rgb(RGB 640x640) → LCD buffer(RGB565 320x240), single pass
        dma_sync_cpu_to_device(arena_in->fd);
        int lcd_ret = convert_image(&g_rga_dst, &g_rga_lcd, NULL, NULL, 0);
        if (lcd_ret == 0) {
            dma_sync_device_to_cpu(g_lcd_fd);
        }

        // 10. FPS report
        sched_report(&sched);

        usleep(10000);
    }

    printf("[AI Camera ZC] thread stopped\n");
    return nullptr;
}

#ifdef USE_OPENCV_CAMERA
/*-------------------------------------------
                  Main Function
-------------------------------------------*/
static void* _inference_loop(void* arg) {
    printf("[AI Camera] thread started\n");

    AICameraArgs* args = (AICameraArgs*)arg;
    set_ai_camera_consumer(args->app_instance);
    delete args;

    cv::VideoCapture cap;
    cv::Mat bgr;

    cap.open(11);

    if (!cap.isOpened()) {
        printf("[AI Camera] Failed to open camera!\n");
        pthread_detach(pthread_self());
        reset_ai_camera_state_after_thread_failure();
        return nullptr;
    }

    printf("[AI Camera] Camera opened successfully.\n");

    cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);

    printf("[AI Camera] Background vision stream started...\n");

    cv::Mat prev_gray;
    int consecutive_motion_frames = 0;

    while(!ai_camera_stop.load()) {

        cap >> bgr;

        if (bgr.empty()) {
            printf("[AI Camera] frame empty!\n");
            usleep(10000);
            continue;
        }
    
        //printf("[AI Camera] frame ok %d x %d\n", bgr.cols, bgr.rows);

        Application* app = get_ai_camera_consumer();
        if (app != nullptr) {
            app->UpdateLatestFrame(bgr);
        }

        cv::Mat gray;
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
        cv::GaussianBlur(gray, gray, cv::Size(5, 5), 0);
        if (!prev_gray.empty() && app != nullptr) {
            cv::Mat diff;
            cv::absdiff(prev_gray, gray, diff);
            cv::threshold(diff, diff, 25, 255, cv::THRESH_BINARY);
            const double motion_ratio = static_cast<double>(cv::countNonZero(diff)) /
                                        static_cast<double>(diff.rows * diff.cols);
            if (motion_ratio > 0.08) {
                consecutive_motion_frames++;
            } else {
                consecutive_motion_frames = 0;
            }

            if (consecutive_motion_frames >= 3) {
                app->SubmitActiveVisionEvent("motion_detected", static_cast<float>(motion_ratio), 5000);
                consecutive_motion_frames = 0;
            }
        }
        prev_gray = gray.clone();

        // ── 人脸检测分支 ──────────────────────────
        bool run_face = false;
        {
            pthread_mutex_lock(&face_model_mutex);
            run_face = face_model_loaded;
            pthread_mutex_unlock(&face_model_mutex);
        }
        if (run_face && app != nullptr) {
            cv::Mat face_input(face_detect_ctx.model_height,
                               face_detect_ctx.model_width,
                               CV_8UC3, face_detect_ctx.input_mems[0]->virt_addr);
            cv::resize(bgr, face_input,
                       cv::Size(face_detect_ctx.model_width,
                                face_detect_ctx.model_height),
                       0, 0, cv::INTER_LINEAR);

            face_detect_result_list fd_results;
            memset(&fd_results, 0, sizeof(fd_results));
            if (inference_face_detect_model(&face_detect_ctx, &fd_results) == 0
                && fd_results.count > 0) {
                float max_conf = 0;
                for (int i = 0; i < fd_results.count; i++) {
                    if (fd_results.results[i].confidence > max_conf)
                        max_conf = fd_results.results[i].confidence;
                }
                app->SubmitActiveVisionEvent("face_detected", max_conf, 8000);
            }
        }

        cv::Mat resized;
        cv::Mat rgb565;
        cv::resize(bgr, resized, cv::Size(disp_width, disp_height), 0, 0, cv::INTER_LINEAR);
        cv::cvtColor(resized, rgb565, cv::COLOR_BGR2BGR565);
        pthread_mutex_lock(&pic_buf_mutex);
        if (yolo_pic_buf != NULL && yolo_pic_buf_size >= static_cast<size_t>(disp_width * disp_height * 2)) {
            memcpy(yolo_pic_buf, rgb565.data, disp_width * disp_height * 2);
        }
        pthread_mutex_unlock(&pic_buf_mutex);

        usleep(30000);
    }

    cap.release();

    printf("[AI Camera] Background vision stream stopped.\n");

    return nullptr;
}

#endif // USE_OPENCV_CAMERA

#if 0
static void* _inference_loop(void* arg) {
    
    // 1. 瑙ｅ寘鍙傛暟
    AICameraArgs* args = (AICameraArgs*)arg;
    std::string model_path_str = args->model_path;
    Application* app = args->app_instance;
    delete args; // 瑙ｅ寘鍚庨噴鏀剧粨鏋勪綋鍐呭瓨
    
    clock_t start_time;
    clock_t end_time;
    char text[8];
    float fps = 0;
    int ret;

    // Model Input (Yolov5)
    int model_width    = 640;
    int model_height   = 640;
    int channels = 3;
    
    // object detect result
    object_detect_result_list od_results;
    memset(&rknn_app_ctx, 0, sizeof(rknn_app_context_t));

    // 2. 淇锛氳繖閲屼娇鐢ㄨВ鍖呭嚭鐨?model_path_str锛岃€屼笉鏄?arg
    init_yolov5_model((char*)model_path_str.c_str(), &rknn_app_ctx);
    printf("model_path: %s\n", model_path_str.c_str());
    init_post_process();

    // Register YOLO with shared DMA arena (first model → triggers allocation)
    {
        npu_arena_t* arena = npu_get_global_arena();
        if (arena) {
            npu_arena_register(arena,
                               rknn_app_ctx.rknn_ctx,
                               rknn_app_ctx.input_attrs,  rknn_app_ctx.io_num.n_input,
                               rknn_app_ctx.output_attrs, rknn_app_ctx.io_num.n_output);
            npu_arena_allocate(arena, rknn_app_ctx.rknn_ctx);
            npu_arena_adopt_yolov5(arena, &rknn_app_ctx);
        }
    }
    
    //fb paras
    int pixel_size = 2;

    // disp
    cv::Mat disp;
    if( pixel_size == 4 )//ARGB8888
        disp = cv::Mat(disp_height, disp_width, CV_8UC3);
    else if ( pixel_size == 2 ) //RGB565
        disp = cv::Mat(disp_height, disp_width, CV_16UC1); 
    
    //Init Opencv-mobile 
    cv::VideoCapture cap;
    cv::Mat bgr(disp_height, disp_width, CV_8UC3); 
    cv::Mat bgr_model_input(model_height, model_width, CV_8UC3, rknn_app_ctx.input_mems[0]->virt_addr);
    cap.set(cv::CAP_PROP_FRAME_WIDTH,  disp_width*2);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, disp_height*2);
    cap.open(0); 

    // API 璇锋眰鍐峰嵈鏃堕棿鎺у埗鍙橀噺
    static clock_t last_upload_time = 0;
    const double UPLOAD_COOLDOWN_SEC = 5.0; // 璁剧疆涓?绉掗槻鎶栧喎鍗?
    while(!ai_camera_stop.load())
    {
        start_time = clock();
        cap >> bgr;

        //letterbox        
        cv::resize(bgr, bgr_model_input, cv::Size(model_width,model_height), 0, 0, cv::INTER_LINEAR);
        inference_yolov5_model(&rknn_app_ctx, &od_results);

        // Add rectangle and probability
        for (int i = 0; i < od_results.count; i++)
        {
            object_detect_result *det_result = &(od_results.results[i]); 
            mapCoordinates(bgr, bgr_model_input, &det_result->box.left,  &det_result->box.top);
            mapCoordinates(bgr, bgr_model_input, &det_result->box.right, &det_result->box.bottom);  
            
            cv::rectangle(bgr,cv::Point(det_result->box.left ,det_result->box.top),
                              cv::Point(det_result->box.right,det_result->box.bottom),cv::Scalar(0,255,0),3);

            sprintf(text, "%s %.1f%%", coco_cls_to_name(det_result->cls_id), det_result->prop * 100);
            cv::putText(bgr,text,cv::Point(det_result->box.left, det_result->box.top - 8),
                                         cv::FONT_HERSHEY_SIMPLEX,1.5,
                                         cv::Scalar(0,255,0),1.5); 
        }
        cv::resize(bgr, bgr, cv::Size(disp_width, disp_height), 0, 0, cv::INTER_LINEAR);

        static int frame_count = 0;
        frame_count++;

        // 璇锋眰鍐峰嵈鏈哄埗 (闃叉姈)
        clock_t current_time = clock();
        double elapsed_sec = (double)(current_time - last_upload_time) / CLOCKS_PER_SEC;

        if (frame_count % 150 == 0 || (od_results.count > 0 && is_person_detected(od_results))) {
            if (last_upload_time == 0 || elapsed_sec >= UPLOAD_COOLDOWN_SEC) {
                printf("瑙﹀彂浜戠瑙嗚鐞嗚В (鍐峰嵈閫氳繃)...\n");
                last_upload_time = current_time; // 閲嶇疆璁℃椂鍣?                
                // 3. 淇锛氬交搴曞幓鎺?upload_to_aliyun锛岀洿鎺ヨ皟鐢?Application 鐨勬柟娉曪紒
                if (app != nullptr) {
                    app->DescribeCurrentScene(bgr.clone()); 
                } else {
                    printf("Warning: app instance is null, cannot describe scene.\n");
                }
            }
        }

        //Fps Show
        sprintf(text,"fps=%.1f",fps); 
        cv::putText(bgr,text,cv::Point(0, 20),
                    cv::FONT_HERSHEY_SIMPLEX,0.5,
                    cv::Scalar(0,255,0),1);

        //LCD Show 
        if( pixel_size == 4 ) 
            cv::cvtColor(bgr, disp, cv::COLOR_BGR2BGRA);
        else if( pixel_size == 2 )
            cv::cvtColor(bgr, disp, cv::COLOR_BGR2BGR565);
            
        // 鍔犻攣淇濇姢 yolo_pic_buf 鍐欏叆
        pthread_mutex_lock(&pic_buf_mutex);
        if (yolo_pic_buf != NULL) {
            memcpy(yolo_pic_buf, disp.data, disp_width * disp_height * pixel_size);
        }
        pthread_mutex_unlock(&pic_buf_mutex);

        //Update Fps
        end_time = clock();
        fps= (float) (CLOCKS_PER_SEC / (end_time - start_time)) ;
        memset(text,0,8); 
    }

    deinit_post_process();

    ret = release_yolov5_model(&rknn_app_ctx);
    if (ret != 0)
    {
        printf("release_yolov5_model fail! ret=%d\n", ret);
    }

    return 0;
}
#endif


extern "C" int start_ai_camera_v2(const char* yolo_path, const char* face_path,
                                     void* app_ptr, int yolo_active, int face_interval) {
    printf("[AI Camera V2] start_ai_camera_v2 called\n");

    Application* app = static_cast<Application*>(app_ptr);

    pthread_mutex_lock(&running_mutex);
    if (ai_camera_running.load()) {
        pthread_mutex_unlock(&running_mutex);
        if (app) set_ai_camera_consumer(app);
        return 1;
    }

    // 1. Init YOLO model
    memset(&rknn_app_ctx, 0, sizeof(rknn_app_ctx));
    if (init_yolov5_model((char*)yolo_path, &rknn_app_ctx) != 0) {
        fprintf(stderr, "[AI Camera V2] YOLO init failed\n");
        pthread_mutex_unlock(&running_mutex);
        return -1;
    }
    init_post_process();
    printf("[AI Camera V2] YOLO loaded: %dx%d\n",
           rknn_app_ctx.model_width, rknn_app_ctx.model_height);

    // 2. Init Face model (if provided)
    memset(&face_detect_ctx, 0, sizeof(face_detect_ctx));
    if (face_path && face_path[0] != '\0') {
        if (init_face_detect_model(face_path, &face_detect_ctx) != 0) {
            fprintf(stderr, "[AI Camera V2] Face init failed\n");
            release_yolov5_model(&rknn_app_ctx);
            deinit_post_process();
            pthread_mutex_unlock(&running_mutex);
            return -1;
        }
        face_model_loaded = true;
        printf("[AI Camera V2] Face loaded: %dx%d\n",
               face_detect_ctx.model_width, face_detect_ctx.model_height);
    }

    // 3. Arena: create → register → allocate → adopt
    npu_arena_t* arena = npu_arena_create();
    npu_set_global_arena(arena);

    npu_arena_register(arena, rknn_app_ctx.rknn_ctx,
                       rknn_app_ctx.input_attrs,  rknn_app_ctx.io_num.n_input,
                       rknn_app_ctx.output_attrs, rknn_app_ctx.io_num.n_output);

    if (face_model_loaded) {
        npu_arena_register(arena, face_detect_ctx.rknn_ctx,
                           face_detect_ctx.input_attrs,  face_detect_ctx.io_num.n_input,
                           face_detect_ctx.output_attrs, face_detect_ctx.io_num.n_output);
    }

    npu_arena_allocate(arena, rknn_app_ctx.rknn_ctx);
    npu_arena_adopt_yolov5(arena, &rknn_app_ctx);
    if (face_model_loaded) {
        npu_arena_adopt_yolov5(arena, &face_detect_ctx);
    }
    printf("[AI Camera V2] Arena: %zu bytes, %zu saved\n",
           npu_arena_total_size(arena), npu_arena_saved_bytes(arena));

    // 4. Init V4L2 DMA-BUF capture
    if (v4l2_capture_init(&g_v4l2_cap, 11, 640, 480) != 0) {
        fprintf(stderr, "[AI Camera V2] V4L2 init failed\n");
        npu_arena_destroy(arena);
        if (face_model_loaded) { release_face_detect_model(&face_detect_ctx); face_model_loaded = false; }
        release_yolov5_model(&rknn_app_ctx);
        deinit_post_process();
        pthread_mutex_unlock(&running_mutex);
        return -1;
    }

    // 5. Set up RGA source descriptor (NV12, fd from V4L2)
    memset(&g_rga_src, 0, sizeof(g_rga_src));
    g_rga_src.width  = 640;
    g_rga_src.height = 480;
    g_rga_src.width_stride  = 640;
    g_rga_src.height_stride = 480;
    g_rga_src.format  = IMAGE_FORMAT_YUV420SP_NV12;
    g_rga_src.virt_addr = NULL;   // fd-only path
    g_rga_src.size    = 640 * 480 * 3 / 2;

    // 6. Set up RGA destination descriptor (arena CMA fd)
    g_yolo_model_w = rknn_app_ctx.model_width;
    g_yolo_model_h = rknn_app_ctx.model_height;
    rknn_tensor_mem* arena_in = npu_arena_input_mem(arena);

    memset(&g_rga_dst, 0, sizeof(g_rga_dst));
    g_rga_dst.width  = g_yolo_model_w;
    g_rga_dst.height = g_yolo_model_h;
    g_rga_dst.width_stride  = g_yolo_model_w;
    g_rga_dst.height_stride = g_yolo_model_h;
    g_rga_dst.format  = IMAGE_FORMAT_RGB888;
    g_rga_dst.virt_addr = (unsigned char*)arena_in->virt_addr;
    g_rga_dst.fd      = arena_in->fd;
    g_rga_dst.size    = g_yolo_model_w * g_yolo_model_h * 3;
    printf("[AI Camera V2] RGA: NV12 640x480(fd) → RGB %dx%d(fd=%d)\n",
           g_yolo_model_w, g_yolo_model_h, g_rga_dst.fd);

    // 7. Alloc LCD buffer (CMA for RGA fd→fd)
    yolo_pic_buf_size = disp_width * disp_height * 2;
    if (dma_buf_alloc(RV1106_CMA_HEAP_PATH, yolo_pic_buf_size,
                      &g_lcd_fd, (void**)&yolo_pic_buf) != 0) {
        fprintf(stderr, "[AI Camera V2] LCD CMA alloc failed\n");
        v4l2_capture_deinit(&g_v4l2_cap);
        npu_arena_destroy(arena);
        if (face_model_loaded) { release_face_detect_model(&face_detect_ctx); face_model_loaded = false; }
        release_yolov5_model(&rknn_app_ctx);
        deinit_post_process();
        pthread_mutex_unlock(&running_mutex);
        return -1;
    }
    memset(&g_rga_lcd, 0, sizeof(g_rga_lcd));
    g_rga_lcd.width  = disp_width;
    g_rga_lcd.height = disp_height;
    g_rga_lcd.width_stride  = disp_width;
    g_rga_lcd.height_stride = disp_height;
    g_rga_lcd.format = IMAGE_FORMAT_RGB565;
    g_rga_lcd.virt_addr = yolo_pic_buf;
    g_rga_lcd.fd = g_lcd_fd;
    g_rga_lcd.size = yolo_pic_buf_size;
    printf("[AI Camera V2] LCD buffer: RGB565 %dx%d CMA fd=%d\n",
           disp_width, disp_height, g_lcd_fd);

    // 8. Alloc CMA double-buffer for frame delivery (RGA RGB→BGR)
    {
        size_t cam_buf_size = (size_t)g_yolo_model_w * g_yolo_model_h * 3;
        for (int i = 0; i < 2; i++) {
            if (dma_buf_alloc(RV1106_CMA_HEAP_PATH, cam_buf_size,
                              &g_cam_buf_fd[i], (void**)&g_cam_buf_virt[i]) != 0) {
                fprintf(stderr, "[AI Camera V2] CMA double-buffer[%d] alloc failed\n", i);
                for (int j = 0; j < i; j++) {
                    dma_buf_free(cam_buf_size, &g_cam_buf_fd[j], g_cam_buf_virt[j]);
                    g_cam_buf_fd[j] = -1; g_cam_buf_virt[j] = NULL;
                }
                dma_buf_free(yolo_pic_buf_size, &g_lcd_fd, yolo_pic_buf); yolo_pic_buf = NULL;
                v4l2_capture_deinit(&g_v4l2_cap);
                npu_arena_destroy(arena);
                if (face_model_loaded) { release_face_detect_model(&face_detect_ctx); face_model_loaded = false; }
                release_yolov5_model(&rknn_app_ctx);
                deinit_post_process();
                pthread_mutex_unlock(&running_mutex);
                return -1;
            }
            memset(&g_rga_bgr_dst[i], 0, sizeof(g_rga_bgr_dst[i]));
            g_rga_bgr_dst[i].width  = g_yolo_model_w;
            g_rga_bgr_dst[i].height = g_yolo_model_h;
            g_rga_bgr_dst[i].width_stride  = g_yolo_model_w;
            g_rga_bgr_dst[i].height_stride = g_yolo_model_h;
            g_rga_bgr_dst[i].format = IMAGE_FORMAT_BGR888;
            g_rga_bgr_dst[i].virt_addr = g_cam_buf_virt[i];
            g_rga_bgr_dst[i].fd = g_cam_buf_fd[i];
            g_rga_bgr_dst[i].size = cam_buf_size;
        }
        printf("[AI Camera V2] Double-buffer: 2×BGR888 %dx%d CMA fd=[%d,%d]\n",
               g_yolo_model_w, g_yolo_model_h, g_cam_buf_fd[0], g_cam_buf_fd[1]);
    }

    // 9. Init HW JPEG encoder (non-fatal: upload falls back to CPU on failure)
    {
        int hw_jpeg_ret = hw_jpeg_encoder_init(640, 640, 448, 448, 85);
        if (hw_jpeg_ret != 0) {
            printf("[AI Camera V2] HW JPEG encoder init failed (ret=%d), will use CPU fallback\n", hw_jpeg_ret);
        }
    }

    // 10. Launch zero-copy thread
    g_use_zero_copy = true;
    ai_camera_running.store(true);
    ai_camera_stop.store(false);
    pthread_mutex_unlock(&running_mutex);

    AICameraArgs* thread_args = new AICameraArgs();
    thread_args->model_path   = yolo_path;
    thread_args->app_instance = app;
    thread_args->yolo_active  = yolo_active;
    thread_args->face_interval = face_interval;

    if (pthread_create(&ai_camera_thread, NULL, _inference_loop_zero_copy,
                       (void*)thread_args) != 0) {
        for (int j = 0; j < 2; j++) {
            if (g_cam_buf_virt[j]) {
                dma_buf_free((size_t)g_yolo_model_w * g_yolo_model_h * 3,
                             &g_cam_buf_fd[j], g_cam_buf_virt[j]);
                g_cam_buf_fd[j] = -1; g_cam_buf_virt[j] = NULL;
            }
        }
        dma_buf_free(yolo_pic_buf_size, &g_lcd_fd, yolo_pic_buf); yolo_pic_buf = NULL;
        ai_camera_running.store(false);
        set_ai_camera_consumer(nullptr);
        delete thread_args;
        return -1;
    }
    return 0;
}

#ifdef USE_OPENCV_CAMERA
extern "C" int start_ai_camera(const char* model_path, void* app_ptr) {
    printf("[AI Camera] start_ai_camera called\n");

    // Create global NPU DMA arena (one-time, shared by all models)
    if (!npu_get_global_arena()) {
        npu_arena_t* arena = npu_arena_create();
        npu_set_global_arena(arena);
        printf("[AI Camera] NPU DMA arena created\n");
    }

    Application* app = static_cast<Application*>(app_ptr);
    pthread_mutex_lock(&running_mutex);
    if (ai_camera_running.load()) {
        pthread_mutex_unlock(&running_mutex);
        if (app != nullptr) {
            set_ai_camera_consumer(app);
        }
        printf("[AI Camera] already running.\n");
        return 1;
    }
    ai_camera_running.store(true);
    ai_camera_stop.store(false);
    pthread_mutex_unlock(&running_mutex);
    
    yolo_pic_buf_size = disp_width * disp_height * 2; 
    yolo_pic_buf = (uint8_t*)malloc(yolo_pic_buf_size);

    // 鎶?model_path 鍜?app 鎸囬拡鎵撳寘
    AICameraArgs* args = new AICameraArgs();
    args->model_path = model_path;
    args->app_instance = app;

    if(pthread_create(&ai_camera_thread, NULL, _inference_loop, (void*)args) != 0){
        // 閿欒澶勭悊...
        free(yolo_pic_buf);
        yolo_pic_buf = NULL;
        yolo_pic_buf_size = 0;
        ai_camera_running.store(false);
        set_ai_camera_consumer(nullptr);
        delete args; // 闃叉鍐呭瓨娉勬紡
        return -1;
    }
    return 0;
}
#endif // USE_OPENCV_CAMERA

extern "C" int attach_ai_camera_consumer(void* app_ptr) {
    Application* app = static_cast<Application*>(app_ptr);
    if (app == nullptr) {
        return -1;
    }
    set_ai_camera_consumer(app);
    return ai_camera_running.load() ? 0 : 1;
}

extern "C" int detach_ai_camera_consumer(void* app_ptr) {
    Application* app = static_cast<Application*>(app_ptr);
    if (app == nullptr) {
        return -1;
    }
    clear_ai_camera_consumer_if(app);
    return 0;
}

int stop_ai_camera() {
    pthread_mutex_lock(&running_mutex);
    if (!ai_camera_running.load()) {
        pthread_mutex_unlock(&running_mutex);
        printf("AI camera is not running.\n");
        return -1;
    }
    pthread_mutex_unlock(&running_mutex);
    ai_camera_stop.store(true);

    pthread_join(ai_camera_thread, NULL);

    // Zero-copy cleanup: V4L2 deinit + arena + YOLO + HW JPEG
    if (g_use_zero_copy) {
        hw_jpeg_encoder_deinit();
        v4l2_capture_deinit(&g_v4l2_cap);
        g_use_zero_copy = false;

        // Arena must be destroyed BEFORE releasing models
        // (arena_destroy NULLs out adopted model mem pointers)
        npu_arena_destroy(npu_get_global_arena());

        release_yolov5_model(&rknn_app_ctx);
        deinit_post_process();
    }

    pthread_mutex_lock(&face_model_mutex);
    if (face_model_loaded) {
        release_face_detect_model(&face_detect_ctx);
        face_model_loaded = false;
    }
    pthread_mutex_unlock(&face_model_mutex);
    // 绛夊緟绾跨▼缁撴潫
    
    // 淇锛氬畨鍏ㄩ噴鏀惧苟缃┖鎸囬拡
    pthread_mutex_lock(&pic_buf_mutex);
    if (yolo_pic_buf != NULL) {
        dma_buf_free(yolo_pic_buf_size, &g_lcd_fd, yolo_pic_buf);
        yolo_pic_buf = NULL;
    }
    pthread_mutex_unlock(&pic_buf_mutex);

    // Free CMA double-buffers
    {
        size_t cam_buf_size = (size_t)g_yolo_model_w * g_yolo_model_h * 3;
        for (int i = 0; i < 2; i++) {
            if (g_cam_buf_virt[i]) {
                dma_buf_free(cam_buf_size, &g_cam_buf_fd[i], g_cam_buf_virt[i]);
                g_cam_buf_fd[i] = -1; g_cam_buf_virt[i] = NULL;
            }
        }
    }

    pthread_mutex_lock(&running_mutex);
    ai_camera_running.store(false);
    ai_camera_stop.store(false);
    pthread_mutex_unlock(&running_mutex);

    set_ai_camera_consumer(nullptr);
    
    return 0;
}

// ── 人脸检测模型生命周期管理 ─────────────────
extern "C" int load_face_detect_model(const char* model_path) {
    pthread_mutex_lock(&face_model_mutex);
    if (face_model_loaded) {
        printf("[AI Camera] face model already loaded, releasing old.\n");
        release_face_detect_model(&face_detect_ctx);
        face_model_loaded = false;
    }
    memset(&face_detect_ctx, 0, sizeof(face_detect_ctx));
    int ret = init_face_detect_model(model_path, &face_detect_ctx);
    if (ret != 0) {
        printf("[AI Camera] failed to load face model: %s\n", model_path);
        pthread_mutex_unlock(&face_model_mutex);
        return -1;
    }

    // Register with arena + adopt (late registration, arena already allocated)
    npu_arena_t* arena = npu_get_global_arena();
    if (arena) {
        npu_arena_register(arena,
                           face_detect_ctx.rknn_ctx,
                           face_detect_ctx.input_attrs,  face_detect_ctx.io_num.n_input,
                           face_detect_ctx.output_attrs, face_detect_ctx.io_num.n_output);
        npu_arena_adopt_yolov5(arena, &face_detect_ctx);
    }

    face_model_loaded = true;
    printf("[AI Camera] face detection model loaded: %s\n", model_path);
    pthread_mutex_unlock(&face_model_mutex);
    return 0;
}

extern "C" int unload_face_detect_model() {
    pthread_mutex_lock(&face_model_mutex);
    if (!face_model_loaded) {
        pthread_mutex_unlock(&face_model_mutex);
        return 0;
    }
    // Arena owns mems — NULL them so release skips rknn_destroy_mem
    npu_arena_t* arena = npu_get_global_arena();
    if (arena) {
        face_detect_ctx.input_mems[0]  = NULL;
        face_detect_ctx.output_mems[0] = NULL;
        face_detect_ctx.output_mems[1] = NULL;
        face_detect_ctx.output_mems[2] = NULL;
    }
    release_face_detect_model(&face_detect_ctx);
    memset(&face_detect_ctx, 0, sizeof(face_detect_ctx));
    face_model_loaded = false;
    printf("[AI Camera] face detection model released.\n");
    pthread_mutex_unlock(&face_model_mutex);
    return 0;
}

void get_buf_data(uint8_t* buffer)
{
    pthread_mutex_lock(&pic_buf_mutex);
    if (yolo_pic_buf == NULL) {
        pthread_mutex_unlock(&pic_buf_mutex);
        printf("Error: yolo_pic_buf is not initialized.\n");
        return;
    }
    dma_sync_device_to_cpu(g_lcd_fd);
    memcpy(buffer, yolo_pic_buf, yolo_pic_buf_size);
    pthread_mutex_unlock(&pic_buf_mutex);
}

void mapCoordinates(cv::Mat input, cv::Mat output, int *x, int *y) {    
    float scaleX = (float)output.cols / (float)input.cols; 
    float scaleY = (float)output.rows / (float)input.rows;
    
    *x = (int)((float)*x / scaleX);
    *y = (int)((float)*y / scaleY);
}
