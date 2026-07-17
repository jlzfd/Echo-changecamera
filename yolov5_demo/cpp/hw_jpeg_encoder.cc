#include "hw_jpeg_encoder.h"

#include "image_utils.h"
#include "dma_alloc.h"

#include "rockchip/rk_mpi.h"
#include "rockchip/mpp_frame.h"
#include "rockchip/mpp_packet.h"
#include "rockchip/mpp_buffer.h"
#include "rockchip/rk_venc_cfg.h"
#include "rockchip/rk_type.h"
#include "rockchip/mpp_err.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <mutex>

// ── Debug toggle ───────────────────────────────────────
#define HW_JPEG_DEBUG 1
#if HW_JPEG_DEBUG
#define HW_LOG(fmt, ...) fprintf(stderr, "[HW JPEG] " fmt "\n", ##__VA_ARGS__)
#else
#define HW_LOG(fmt, ...) do {} while(0)
#endif

// ── Static state ───────────────────────────────────────
static MppCtx    g_mpp_ctx = NULL;
static MppApi*   g_mpp_mpi = NULL;
static MppEncCfg g_enc_cfg = NULL;
static bool      g_inited  = false;
static std::mutex g_mutex;

static int g_src_w = 0, g_src_h = 0;
static int g_dst_w = 0, g_dst_h = 0;

// Intermediate NV12 CMA buffer (RGA BGR→NV12 destination)
static int             g_nv12_fd   = -1;
static void*           g_nv12_virt = NULL;
static size_t          g_nv12_size = 0;
static image_buffer_t  g_rga_nv12_dst;

// JPEG output staging buffer
static std::vector<uint8_t> g_jpeg_buf;

// ── Helpers ────────────────────────────────────────────

static MPP_RET build_frame_from_dma(int fd, void* virt, size_t size,
                                     int w, int h, MppFrame* out) {
    MppBufferInfo info;
    memset(&info, 0, sizeof(info));
    info.type = MPP_BUFFER_TYPE_EXT_DMA;
    info.fd   = fd;
    info.ptr  = virt;
    info.size = size;

    MppBuffer buf = NULL;
    MPP_RET ret = mpp_buffer_import(&buf, &info);
    if (ret || !buf) {
        HW_LOG("mpp_buffer_import FAIL ret=%d fd=%d size=%zu", ret, fd, size);
        return ret;
    }
    HW_LOG("mpp_buffer_import OK buf=%p", (void*)buf);

    MppFrame frame = NULL;
    ret = mpp_frame_init(&frame);
    if (ret || !frame) {
        HW_LOG("mpp_frame_init FAIL ret=%d", ret);
        mpp_buffer_put(buf);
        return ret;
    }

    mpp_frame_set_buffer(frame, buf);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_width(frame, w);
    mpp_frame_set_height(frame, h);
    mpp_frame_set_hor_stride(frame, w);
    mpp_frame_set_ver_stride(frame, h);
    mpp_frame_set_eos(frame, 0);

    *out = frame;
    return MPP_OK;
}

static void release_frame(MppFrame frame) {
    if (!frame) return;
    MppBuffer buf = mpp_frame_get_buffer(frame);
    if (buf) mpp_buffer_put(buf);
    mpp_frame_deinit(&frame);
}

// ── Public API ─────────────────────────────────────────

int hw_jpeg_encoder_init(int src_w, int src_h, int dst_w, int dst_h, int quality) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_inited) return 0;

    g_src_w = src_w; g_src_h = src_h;
    g_dst_w = dst_w; g_dst_h = dst_h;

    HW_LOG("=== init start: BGR %dx%d → NV12 %dx%d → JPEG q=%d ===",
           src_w, src_h, dst_w, dst_h, quality);

    // 1. Intermediate NV12 CMA buffer (RGA output → MPP input)
    g_nv12_size = (size_t)dst_w * dst_h * 3 / 2;
    if (dma_buf_alloc(RV1106_CMA_HEAP_PATH, g_nv12_size,
                      &g_nv12_fd, &g_nv12_virt) != 0) {
        HW_LOG("FATAL: NV12 CMA alloc failed (size=%zu)", g_nv12_size);
        return -1;
    }
    HW_LOG("NV12 CMA: size=%zu fd=%d virt=%p", g_nv12_size, g_nv12_fd, g_nv12_virt);

    // 2. RGA dst descriptor (reused every encode)
    memset(&g_rga_nv12_dst, 0, sizeof(g_rga_nv12_dst));
    g_rga_nv12_dst.format = IMAGE_FORMAT_YUV420SP_NV12;
    g_rga_nv12_dst.width  = dst_w;
    g_rga_nv12_dst.height = dst_h;
    g_rga_nv12_dst.width_stride  = dst_w;
    g_rga_nv12_dst.height_stride = dst_h;
    g_rga_nv12_dst.virt_addr = (unsigned char*)g_nv12_virt;
    g_rga_nv12_dst.fd   = g_nv12_fd;
    g_rga_nv12_dst.size = g_nv12_size;

    // 3. MPP context: create + init for MJPEG encoding
    MPP_RET ret = mpp_create(&g_mpp_ctx, &g_mpp_mpi);
    if (ret || !g_mpp_ctx || !g_mpp_mpi) {
        HW_LOG("FATAL: mpp_create FAIL ret=%d ctx=%p mpi=%p", ret,
               (void*)g_mpp_ctx, (void*)g_mpp_mpi);
        dma_buf_free(g_nv12_size, &g_nv12_fd, g_nv12_virt);
        g_nv12_fd = -1; g_nv12_virt = NULL;
        return -2;
    }
    HW_LOG("mpp_create OK ctx=%p mpi=%p", (void*)g_mpp_ctx, (void*)g_mpp_mpi);

    ret = mpp_init(g_mpp_ctx, MPP_CTX_ENC, MPP_VIDEO_CodingMJPEG);
    if (ret) {
        HW_LOG("FATAL: mpp_init(MJPEG) FAIL ret=%d (check kernel driver)", ret);
        mpp_destroy(g_mpp_ctx);
        g_mpp_ctx = NULL; g_mpp_mpi = NULL;
        dma_buf_free(g_nv12_size, &g_nv12_fd, g_nv12_virt);
        g_nv12_fd = -1; g_nv12_virt = NULL;
        return -3;
    }
    HW_LOG("mpp_init(MJPEG) OK");

    // 4. Encoder config
    ret = mpp_enc_cfg_init(&g_enc_cfg);
    if (ret) {
        HW_LOG("FATAL: mpp_enc_cfg_init FAIL ret=%d", ret);
        mpp_destroy(g_mpp_ctx);
        g_mpp_ctx = NULL; g_mpp_mpi = NULL;
        dma_buf_free(g_nv12_size, &g_nv12_fd, g_nv12_virt);
        g_nv12_fd = -1; g_nv12_virt = NULL;
        return -4;
    }

    mpp_enc_cfg_set_s32(g_enc_cfg, "prep:width",  dst_w);
    mpp_enc_cfg_set_s32(g_enc_cfg, "prep:height", dst_h);
    mpp_enc_cfg_set_s32(g_enc_cfg, "prep:format", MPP_FMT_YUV420SP);
    mpp_enc_cfg_set_s32(g_enc_cfg, "rc:quality",  quality > 0 ? quality : 85);
    HW_LOG("config: prep=%dx%d fmt=%d quality=%d",
           dst_w, dst_h, MPP_FMT_YUV420SP, quality);

    ret = g_mpp_mpi->control(g_mpp_ctx, MPP_ENC_SET_CFG, g_enc_cfg);
    if (ret) {
        HW_LOG("FATAL: MPP_ENC_SET_CFG FAIL ret=%d", ret);
        mpp_enc_cfg_deinit(g_enc_cfg); g_enc_cfg = NULL;
        mpp_destroy(g_mpp_ctx); g_mpp_ctx = NULL; g_mpp_mpi = NULL;
        dma_buf_free(g_nv12_size, &g_nv12_fd, g_nv12_virt);
        g_nv12_fd = -1; g_nv12_virt = NULL;
        return -5;
    }
    HW_LOG("MPP_ENC_SET_CFG OK");

    g_inited = true;
    HW_LOG("=== init SUCCESS ===");
    return 0;
}

int hw_jpeg_encode(int src_fd, uint8_t** jpeg_data, size_t* jpeg_size) {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (!g_inited || !g_mpp_ctx || !g_mpp_mpi) {
        HW_LOG("encode: not initialized");
        return -1;
    }
    if (!jpeg_data || !jpeg_size) return -2;

    HW_LOG("=== encode start: src_fd=%d ===", src_fd);

    // 1. RGA: BGR(fd) → NV12(fd), fd→fd with resize
    image_buffer_t rga_src;
    memset(&rga_src, 0, sizeof(rga_src));
    rga_src.format = IMAGE_FORMAT_BGR888;
    rga_src.width  = g_src_w;
    rga_src.height = g_src_h;
    rga_src.width_stride  = g_src_w;
    rga_src.height_stride = g_src_h;
    rga_src.virt_addr = NULL;
    rga_src.fd   = src_fd;
    rga_src.size = (size_t)g_src_w * g_src_h * 3;

    int rga_ret = convert_image(&rga_src, &g_rga_nv12_dst, NULL, NULL, 0);
    if (rga_ret != 0) {
        HW_LOG("RGA BGR→NV12 FAIL ret=%d", rga_ret);
        return -3;
    }
    HW_LOG("RGA BGR→NV12 OK");

    // 2. DMA sync: ensure MPP sees RGA output in CMA
    dma_sync_device_to_cpu(g_nv12_fd);

    // 3. Build MppFrame from NV12 DMA fd
    MppFrame frame = NULL;
    MPP_RET ret = build_frame_from_dma(g_nv12_fd, g_nv12_virt, g_nv12_size,
                                        g_dst_w, g_dst_h, &frame);
    if (ret || !frame) {
        HW_LOG("build_frame_from_dma FAIL ret=%d", ret);
        return -4;
    }

    // 4. Encode: try synchronous encode() first
    MppPacket packet = NULL;
    ret = g_mpp_mpi->encode(g_mpp_ctx, frame, &packet);

    // Fallback: async put_frame + get_packet
    if (ret || !packet) {
        HW_LOG("sync encode() returned ret=%d, trying async path...", ret);

        // Release old frame if encode() consumed it
        if (ret == 0 && !packet) {
            // frame was consumed, need new one
            release_frame(frame);
            ret = build_frame_from_dma(g_nv12_fd, g_nv12_virt, g_nv12_size,
                                        g_dst_w, g_dst_h, &frame);
            if (ret || !frame) {
                HW_LOG("build_frame_from_dma(2) FAIL ret=%d", ret);
                return -4;
            }
        }

        ret = g_mpp_mpi->encode_put_frame(g_mpp_ctx, frame);
        if (ret) {
            HW_LOG("encode_put_frame FAIL ret=%d", ret);
            release_frame(frame);
            return -5;
        }
        HW_LOG("encode_put_frame OK, waiting for packet...");

        // Poll with timeout (500ms max)
        MPP_RET poll_ret;
        int poll_count = 0;
        do {
            poll_ret = g_mpp_mpi->poll(g_mpp_ctx, MPP_PORT_OUTPUT, (MppPollType)50);
            poll_count++;
        } while (poll_ret == 0 && poll_count < 10);

        if (poll_ret < 0) {
            HW_LOG("poll FAIL ret=%d after %d tries", poll_ret, poll_count);
            return -5;
        }
        HW_LOG("poll OK after %d tries", poll_count);

        ret = g_mpp_mpi->encode_get_packet(g_mpp_ctx, &packet);
        if (ret || !packet) {
            HW_LOG("encode_get_packet FAIL ret=%d", ret);
            return -5;
        }
    }
    HW_LOG("encode OK, packet=%p", (void*)packet);

    // 5. Extract JPEG bitstream
    void*  pkt_data = mpp_packet_get_data(packet);
    size_t pkt_len  = mpp_packet_get_length(packet);

    if (!pkt_data || pkt_len == 0) {
        HW_LOG("empty packet: data=%p len=%zu", pkt_data, pkt_len);
        mpp_packet_deinit(&packet);
        release_frame(frame);
        return -6;
    }

    // Validate JPEG header (must start with SOI marker 0xFF 0xD8)
    uint8_t* raw = (uint8_t*)pkt_data;
    bool jpeg_valid = (pkt_len >= 2 && raw[0] == 0xFF && raw[1] == 0xD8);
    HW_LOG("JPEG: len=%zu soi=0x%02X%02X valid=%d",
           pkt_len, raw[0], raw[1], jpeg_valid);

    if (!jpeg_valid) {
        HW_LOG("WARNING: JPEG SOI marker missing — MPP may need jpeg:header_mode config");
        // Don't fail; some MPP versions strip the header on first frame
    }

    // 6. Copy to staging buffer
    g_jpeg_buf.assign(raw, raw + pkt_len);
    *jpeg_data = g_jpeg_buf.data();
    *jpeg_size = g_jpeg_buf.size();

    mpp_packet_deinit(&packet);
    release_frame(frame);

    HW_LOG("=== encode SUCCESS: %zu bytes ===", pkt_len);
    return (int)pkt_len;
}

void hw_jpeg_encoder_deinit(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_inited) return;

    HW_LOG("=== deinit ===");

    g_jpeg_buf.clear();

    if (g_enc_cfg)  { mpp_enc_cfg_deinit(g_enc_cfg); g_enc_cfg = NULL; }
    if (g_mpp_ctx)  { mpp_destroy(g_mpp_ctx); g_mpp_ctx = NULL; g_mpp_mpi = NULL; }
    if (g_nv12_virt) {
        dma_buf_free(g_nv12_size, &g_nv12_fd, g_nv12_virt);
        g_nv12_fd = -1; g_nv12_virt = NULL;
    }

    g_inited = false;
    HW_LOG("=== deinit DONE ===");
}
