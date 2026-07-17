// V4L2 DMA-BUF capture for RV1106 zero-copy pipeline.
//
// Opens /dev/videoN, negotiates NV12 (single-plane or multi-plane),
// allocates MMAP buffers, exports each as dma-buf fd via VIDIOC_EXPBUF,
// configures ISP auto-exposure / auto-white-balance.
//
// Usage:
//   v4l2_capture_t cap;
//   v4l2_capture_init(&cap, 11, 640, 480);
//   int fd, idx;
//   while (...) {
//       idx = v4l2_capture_get_frame(&cap, &fd);
//       // use fd with RGA (zero CPU copy)
//       v4l2_capture_put_frame(&cap, idx);
//   }
//   v4l2_capture_deinit(&cap);

#ifndef V4L2_CAPTURE_H_
#define V4L2_CAPTURE_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define V4L2_CAPTURE_MAX_BUFFERS  4

typedef struct {
    int     fd;             // dma-buf fd from VIDIOC_EXPBUF (valid for buffer lifetime)
    void*   mmap_addr;      // mmap'd kernel buffer (for munmap, not pixel access)
    size_t  length;         // buffer length in bytes
} v4l2_buffer_info_t;

typedef struct {
    int     dev_fd;                       // open file descriptor for /dev/videoN
    int     num_buffers;                  // number of queued buffers (typically 3)
    v4l2_buffer_info_t buffers[V4L2_CAPTURE_MAX_BUFFERS];

    int     width;
    int     height;
    uint32_t pixelformat;                 // V4L2_PIX_FMT_NV12

    bool    streaming;                    // true after VIDIOC_STREAMON
    bool    is_mplane;                    // true for V4L2_CAP_VIDEO_CAPTURE_MPLANE
    int     num_planes;                   // number of planes (G_FMT readback)
} v4l2_capture_t;

// Open /dev/videoN, negotiate NV12 at w×h, allocate + mmap + EXPBUF + QBUF
// all buffers, configure ISP (AE/AWB), start streaming.
// Returns 0 on success, -1 on error (prints diagnostic to stderr).
int v4l2_capture_init(v4l2_capture_t* cap, int dev_num, int width, int height);

// Dequeue a frame. Returns buffer index (0..num_buffers-1) on success, -1 on error.
// On success, *fd_out receives the pre-exported dma-buf fd.
// Caller must call v4l2_capture_put_frame() to re-queue.
int v4l2_capture_get_frame(v4l2_capture_t* cap, int* fd_out);

// Re-queue a buffer back to V4L2. buffer_index must be the index returned by
// v4l2_capture_get_frame(). Returns 0 on success, -1 on error.
int v4l2_capture_put_frame(v4l2_capture_t* cap, int buffer_index);

// Stop streaming, unmap buffers, close exported fds, release device.
void v4l2_capture_deinit(v4l2_capture_t* cap);

#ifdef __cplusplus
}
#endif

#endif // V4L2_CAPTURE_H_