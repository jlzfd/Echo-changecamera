// V4L2 DMA-BUF capture implementation for RV1106 (MPLANE).
// See v4l2_capture.h for API documentation.

#include "v4l2_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>

// ── Helpers ──────────────────────────────────────────

static int xioctl(int fd, unsigned long request, void* arg) {
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

static const char* pixelformat_str(uint32_t fmt) {
    static char buf[5];
    buf[0] = (char)(fmt & 0xff);
    buf[1] = (char)((fmt >> 8) & 0xff);
    buf[2] = (char)((fmt >> 16) & 0xff);
    buf[3] = (char)((fmt >> 24) & 0xff);
    buf[4] = '\0';
    return buf;
}

// ── ISP configuration ────────────────────────────────

static void v4l2_configure_isp(int fd) {
    struct v4l2_control ctrl;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = V4L2_CID_EXPOSURE_AUTO;
    ctrl.value = V4L2_EXPOSURE_AUTO;
    if (xioctl(fd, VIDIOC_S_CTRL, &ctrl) == 0) {
        printf("[V4L2] Auto-exposure enabled\n");
    } else {
        printf("[V4L2] AE not supported on this device (errno=%d), using ISP defaults\n", errno);
    }

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = V4L2_CID_AUTO_WHITE_BALANCE;
    ctrl.value = 1;
    if (xioctl(fd, VIDIOC_S_CTRL, &ctrl) == 0) {
        printf("[V4L2] Auto-white-balance enabled\n");
    } else {
        printf("[V4L2] AWB not supported on this device (errno=%d), using ISP defaults\n", errno);
    }
}

// ── Init ─────────────────────────────────────────────

int v4l2_capture_init(v4l2_capture_t* cap, int dev_num, int width, int height) {
    if (!cap) return -1;
    memset(cap, 0, sizeof(*cap));
    cap->dev_fd = -1;

    // 1. Open device
    char dev_path[32];
    snprintf(dev_path, sizeof(dev_path), "/dev/video%d", dev_num);
    cap->dev_fd = open(dev_path, O_RDWR | O_NONBLOCK);
    if (cap->dev_fd < 0) {
        fprintf(stderr, "[V4L2] Cannot open %s: %s\n", dev_path, strerror(errno));
        return -1;
    }

    // 2. Query capabilities (accept both single-plane and multi-plane)
    struct v4l2_capability vcap;
    memset(&vcap, 0, sizeof(vcap));
    if (xioctl(cap->dev_fd, VIDIOC_QUERYCAP, &vcap) < 0) {
        fprintf(stderr, "[V4L2] VIDIOC_QUERYCAP failed: %s\n", strerror(errno));
        close(cap->dev_fd); cap->dev_fd = -1;
        return -1;
    }
    uint32_t dcaps = vcap.device_caps ? vcap.device_caps : vcap.capabilities;
    if (!(dcaps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE))) {
        fprintf(stderr, "[V4L2] %s is not a video capture device (caps=0x%x dcaps=0x%x)\n",
                dev_path, vcap.capabilities, vcap.device_caps);
        close(cap->dev_fd); cap->dev_fd = -1;
        return -1;
    }
    if (!(dcaps & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "[V4L2] %s does not support streaming I/O\n", dev_path);
        close(cap->dev_fd); cap->dev_fd = -1;
        return -1;
    }
    cap->is_mplane = !!(dcaps & V4L2_CAP_VIDEO_CAPTURE_MPLANE);
    uint32_t buf_type = cap->is_mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                       : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    printf("[V4L2] %s: driver='%s' card='%s' caps=0x%x mplane=%d\n",
           dev_path, vcap.driver, vcap.card, vcap.capabilities, cap->is_mplane);

    // 3. Set format: NV12 at requested resolution
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = buf_type;
    if (cap->is_mplane) {
        fmt.fmt.pix_mp.width       = (uint32_t)width;
        fmt.fmt.pix_mp.height      = (uint32_t)height;
        fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
        fmt.fmt.pix_mp.field       = V4L2_FIELD_ANY;
    } else {
        fmt.fmt.pix.width       = (uint32_t)width;
        fmt.fmt.pix.height      = (uint32_t)height;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
        fmt.fmt.pix.field       = V4L2_FIELD_ANY;
    }

    if (xioctl(cap->dev_fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "[V4L2] S_FMT NV12 %dx%d failed: %s\n",
                width, height, strerror(errno));
        close(cap->dev_fd); cap->dev_fd = -1;
        return -1;
    }

    // Read back what the driver actually configured
    if (xioctl(cap->dev_fd, VIDIOC_G_FMT, &fmt) < 0) {
        fprintf(stderr, "[V4L2] G_FMT failed: %s\n", strerror(errno));
        close(cap->dev_fd); cap->dev_fd = -1;
        return -1;
    }

    if (cap->is_mplane) {
        cap->width       = (int)fmt.fmt.pix_mp.width;
        cap->height      = (int)fmt.fmt.pix_mp.height;
        cap->pixelformat = fmt.fmt.pix_mp.pixelformat;
        cap->num_planes  = (int)fmt.fmt.pix_mp.num_planes;
        printf("[V4L2] Format: %s %dx%d stride=%d size=%d planes=%d\n",
               pixelformat_str(cap->pixelformat),
               cap->width, cap->height,
               fmt.fmt.pix_mp.plane_fmt[0].bytesperline,
               fmt.fmt.pix_mp.plane_fmt[0].sizeimage,
               cap->num_planes);
    } else {
        cap->width       = (int)fmt.fmt.pix.width;
        cap->height      = (int)fmt.fmt.pix.height;
        cap->pixelformat = fmt.fmt.pix.pixelformat;
        cap->num_planes  = 1;
        printf("[V4L2] Format: %s %dx%d stride=%d size=%d\n",
               pixelformat_str(cap->pixelformat),
               cap->width, cap->height,
               fmt.fmt.pix.bytesperline,
               fmt.fmt.pix.sizeimage);
    }

    if (cap->pixelformat != V4L2_PIX_FMT_NV12) {
        fprintf(stderr, "[V4L2] WARNING: driver returned %s, expected NV12\n",
                pixelformat_str(cap->pixelformat));
    }

    if (cap->width != width || cap->height != height) {
        printf("[V4L2] Driver adjusted resolution: %dx%d -> %dx%d\n",
               width, height, cap->width, cap->height);
    }

    // 4. Request MMAP buffers
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = 4;
    req.type   = buf_type;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(cap->dev_fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "[V4L2] REQBUFS(4, MMAP) failed: %s\n", strerror(errno));
        close(cap->dev_fd); cap->dev_fd = -1;
        return -1;
    }
    cap->num_buffers = (int)req.count;
    printf("[V4L2] Requested %d buffers, got %d\n", 3, cap->num_buffers);

    if (cap->num_buffers < 2) {
        fprintf(stderr, "[V4L2] Need at least 2 buffers, got %d\n", cap->num_buffers);
        close(cap->dev_fd); cap->dev_fd = -1;
        return -1;
    }

    // 5. For each buffer: QUERYBUF -> mmap -> EXPBUF -> QBUF
    struct v4l2_plane planes_buf[VIDEO_MAX_PLANES];
    for (int i = 0; i < cap->num_buffers; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        memset(planes_buf, 0, sizeof(planes_buf));
        buf.type   = buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = (uint32_t)i;
        buf.m.planes = planes_buf;
        buf.length   = (uint32_t)cap->num_planes;

        if (xioctl(cap->dev_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "[V4L2] QUERYBUF[%d] failed: %s\n", i, strerror(errno));
            v4l2_capture_deinit(cap);
            return -1;
        }

        // Use plane[0] offset + length for mmap (NV12 is contiguous)
        uint32_t offset = cap->is_mplane ? planes_buf[0].m.mem_offset : buf.m.offset;
        uint32_t length = cap->is_mplane ? planes_buf[0].length : buf.length;

        cap->buffers[i].length = length;
        cap->buffers[i].mmap_addr = mmap(NULL, length,
                                         PROT_READ | PROT_WRITE,
                                         MAP_SHARED,
                                         cap->dev_fd, offset);
        if (cap->buffers[i].mmap_addr == MAP_FAILED) {
            fprintf(stderr, "[V4L2] mmap[%d] failed: %s\n", i, strerror(errno));
            v4l2_capture_deinit(cap);
            return -1;
        }

        // Export as dma-buf fd
        struct v4l2_exportbuffer expbuf;
        memset(&expbuf, 0, sizeof(expbuf));
        expbuf.type  = buf_type;
        expbuf.index = (uint32_t)i;
        expbuf.flags = 0;

        if (xioctl(cap->dev_fd, VIDIOC_EXPBUF, &expbuf) < 0) {
            fprintf(stderr, "[V4L2] EXPBUF[%d] failed: %s\n", i, strerror(errno));
            fprintf(stderr, "[V4L2] Kernel may not support DMA-BUF export. "
                    "Check CONFIG_DMA_SHARED_BUFFER=y\n");
            v4l2_capture_deinit(cap);
            return -1;
        }
        cap->buffers[i].fd = expbuf.fd;

        // QBUF to enqueue for capture
        memset(&buf, 0, sizeof(buf));
        memset(planes_buf, 0, sizeof(planes_buf));
        buf.type   = buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = (uint32_t)i;
        buf.m.planes = planes_buf;
        buf.length   = (uint32_t)cap->num_planes;

        if (xioctl(cap->dev_fd, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "[V4L2] QBUF[%d] failed: %s\n", i, strerror(errno));
            v4l2_capture_deinit(cap);
            return -1;
        }

        printf("[V4L2] Buffer[%d]: mmap=%p len=%zu fd=%d\n",
               i, cap->buffers[i].mmap_addr,
               cap->buffers[i].length, cap->buffers[i].fd);
    }

    // 6. ISP configuration
    v4l2_configure_isp(cap->dev_fd);

    // 7. Start streaming
    if (xioctl(cap->dev_fd, VIDIOC_STREAMON, &buf_type) < 0) {
        fprintf(stderr, "[V4L2] STREAMON failed: %s\n", strerror(errno));
        v4l2_capture_deinit(cap);
        return -1;
    }
    cap->streaming = true;

    printf("[V4L2] Streaming started (%dx%d %s, %d buffers)\n",
           cap->width, cap->height, pixelformat_str(cap->pixelformat),
           cap->num_buffers);
    return 0;
}

// ── Get frame ────────────────────────────────────────

int v4l2_capture_get_frame(v4l2_capture_t* cap, int* fd_out) {
    if (!cap || !cap->streaming || !fd_out) return -1;

    uint32_t buf_type = cap->is_mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                       : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    struct v4l2_buffer buf;
    struct v4l2_plane planes_buf[VIDEO_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes_buf, 0, sizeof(planes_buf));
    buf.type   = buf_type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = planes_buf;
    buf.length   = (uint32_t)cap->num_planes;

    // Non-blocking poll: retry on EAGAIN
    while (xioctl(cap->dev_fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) {
            usleep(5000);
            continue;
        }
        if (errno == EINTR) continue;
        fprintf(stderr, "[V4L2] DQBUF failed: %s\n", strerror(errno));
        return -1;
    }

    *fd_out = cap->buffers[buf.index].fd;
    return (int)buf.index;
}

// ── Put frame ────────────────────────────────────────

int v4l2_capture_put_frame(v4l2_capture_t* cap, int buffer_index) {
    if (!cap || !cap->streaming) return -1;
    if (buffer_index < 0 || buffer_index >= cap->num_buffers) return -1;

    uint32_t buf_type = cap->is_mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                       : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    struct v4l2_buffer buf;
    struct v4l2_plane planes_buf[VIDEO_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes_buf, 0, sizeof(planes_buf));
    buf.type   = buf_type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = (uint32_t)buffer_index;
    buf.m.planes = planes_buf;
    buf.length   = (uint32_t)cap->num_planes;

    if (xioctl(cap->dev_fd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "[V4L2] QBUF[%d] failed: %s\n", buffer_index, strerror(errno));
        return -1;
    }
    return 0;
}

// ── Deinit ───────────────────────────────────────────

void v4l2_capture_deinit(v4l2_capture_t* cap) {
    if (!cap) return;

    if (cap->streaming) {
        uint32_t buf_type = cap->is_mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                           : V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(cap->dev_fd, VIDIOC_STREAMOFF, &buf_type) < 0) {
            fprintf(stderr, "[V4L2] STREAMOFF failed: %s\n", strerror(errno));
        }
        cap->streaming = false;
    }

    for (int i = 0; i < cap->num_buffers; i++) {
        if (cap->buffers[i].mmap_addr && cap->buffers[i].mmap_addr != MAP_FAILED) {
            munmap(cap->buffers[i].mmap_addr, cap->buffers[i].length);
            cap->buffers[i].mmap_addr = NULL;
        }
        if (cap->buffers[i].fd >= 0) {
            close(cap->buffers[i].fd);
            cap->buffers[i].fd = -1;
        }
    }

    if (cap->dev_fd >= 0) {
        close(cap->dev_fd);
        cap->dev_fd = -1;
    }
    cap->num_buffers = 0;

    printf("[V4L2] Capture deinitialized\n");
}