/*
 * Example: RV1106 inference dispatch loop over UART.
 *
 * Integrate into DeskBot main.c or AIChat Client main.cc:
 *
 *   // --- init ---
 *   uart_bridge_init("/dev/ttyS1", 3000000);
 *   npu_model_init("model/yolov5.rknn");
 *
 *   // --- main loop ---
 *   char json[4096];
 *   int len = uart_bridge_recv(json, sizeof(json));
 *   if (len > 0) {
 *       dispatch(json);   // parse cmd, run inference, reply
 *   }
 *
 * Protocol commands from T113:
 *   {"cmd":"detect","ts":123456}
 *   {"cmd":"classify","ts":123457}
 *   {"cmd":"ping","ts":0}
 *   {"cmd":"shutdown"}
 *
 * Replies to T113:
 *   {"face":{"x":120,"y":80,"w":64,"h":64},"depth":0,"ts":123456}
 *   {"label":"person","confidence":0.95,"x":100,"y":50,"w":200,"h":300}
 *   {"pong":"ok"}
 *   {"status":"shutting_down"}
 */

#include "uart_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// #include "npu/rknn_api.h"   // Rockchip NPU SDK
// #include "jpeg_decoder.h"    // or use stb_image for JPEG decode

// --- placeholder: your actual NPU inference function ---
static int run_face_detect(const uint8_t *jpeg_data, size_t jpeg_size,
                           int *out_x, int *out_y, int *out_w, int *out_h)
{
    (void)jpeg_data; (void)jpeg_size;
    // TODO: JPEG decode → NPU preprocess → rknn_run → postprocess
    *out_x = 100; *out_y = 80; *out_w = 64; *out_h = 64;
    return 0;
}

static void handle_detect(const char *json)
{
    // parse JSON manually for lightweight embedded
    // {"cmd":"detect","ts":123456,"w":320,"h":240,"data":"<base64>"}
    long ts = 0;
    const char *p = strstr(json, "\"ts\":");
    if (p) ts = strtol(p + 5, NULL, 10);

    // TODO: extract base64 JPEG data, decode, run NPU
    int x = 0, y = 0, w = 0, h = 0;
    int ret = run_face_detect(NULL, 0, &x, &y, &w, &h);

    char reply[512];
    if (ret == 0) {
        snprintf(reply, sizeof(reply),
                 "{\"face\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},\"ts\":%ld}",
                 x, y, w, h, ts);
    } else {
        snprintf(reply, sizeof(reply),
                 "{\"error\":\"detect_failed\",\"ts\":%ld}", ts);
    }
    uart_bridge_send(reply);
}

static void handle_ping(void)
{
    uart_bridge_send("{\"pong\":\"ok\"}");
}

void dispatch(const char *json)
{
    if (!json) return;

    if (strstr(json, "\"detect\"")) {
        handle_detect(json);
    } else if (strstr(json, "\"ping\"")) {
        handle_ping();
    }
    // add more commands: classify, shutdown, status...
}
