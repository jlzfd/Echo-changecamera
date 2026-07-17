#ifndef HW_JPEG_ENCODER_H
#define HW_JPEG_ENCODER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize HW JPEG encoder (MPP MJPEG + RGA BGR→NV12).
 * src_w/src_h: BGR888 source dimensions (e.g. 640x640)
 * dst_w/dst_h: JPEG output dimensions (e.g. 448x448)
 * quality: 1-100, default 85
 * Returns 0 on success, negative on error.
 */
int hw_jpeg_encoder_init(int src_w, int src_h, int dst_w, int dst_h, int quality);

/**
 * Encode BGR888 CMA buffer to JPEG via RGA→NV12→MPP pipeline.
 * src_fd: DMA-BUF fd of BGR888 source (src_w × src_h)
 * Returns JPEG size on success, 0 or negative on error.
 * *jpeg_data is valid until the next hw_jpeg_encode() or hw_jpeg_encoder_deinit().
 */
int hw_jpeg_encode(int src_fd, uint8_t** jpeg_data, size_t* jpeg_size);

void hw_jpeg_encoder_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // HW_JPEG_ENCODER_H
