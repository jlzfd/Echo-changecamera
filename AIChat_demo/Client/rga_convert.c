#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "image_utils.h"

#include "im2d.h"
#include "drmrga.h"

// RV1106 uses handle-based RGA
#define LIBRGA_IM2D_HANDLE

static int get_rga_fmt(image_format_t fmt) {
    switch (fmt) {
    case IMAGE_FORMAT_RGB888:
        return RK_FORMAT_RGB_888;
    case IMAGE_FORMAT_BGR888:
        return RK_FORMAT_BGR_888;
    case IMAGE_FORMAT_RGBA8888:
        return RK_FORMAT_RGBA_8888;
    case IMAGE_FORMAT_RGB565:
        return RK_FORMAT_RGB_565;
    case IMAGE_FORMAT_YUV420SP_NV12:
        return RK_FORMAT_YCbCr_420_SP;
    case IMAGE_FORMAT_YUV420SP_NV21:
        return RK_FORMAT_YCrCb_420_SP;
    default:
        return -1;
    }
}

static int rga_get_image_size(image_buffer_t* image) {
    if (image == NULL) return 0;
    switch (image->format) {
    case IMAGE_FORMAT_GRAY8:
        return image->width * image->height;
    case IMAGE_FORMAT_RGB888:
    case IMAGE_FORMAT_BGR888:
        return image->width * image->height * 3;
    case IMAGE_FORMAT_RGBA8888:
        return image->width * image->height * 4;
    case IMAGE_FORMAT_RGB565:
        return image->width * image->height * 2;
    case IMAGE_FORMAT_YUV420SP_NV12:
    case IMAGE_FORMAT_YUV420SP_NV21:
        return image->width * image->height * 3 / 2;
    default:
        return 0;
    }
}

int convert_image(image_buffer_t* src_img, image_buffer_t* dst_img,
                  image_rect_t* src_box, image_rect_t* dst_box, char color)
{
    int srcWidth  = src_img->width;
    int srcHeight = src_img->height;
    void *src     = src_img->virt_addr;
    int src_fd    = src_img->fd;
    int srcFmt    = get_rga_fmt(src_img->format);

    int dstWidth  = dst_img->width;
    int dstHeight = dst_img->height;
    void *dst     = dst_img->virt_addr;
    int dst_fd    = dst_img->fd;
    int dstFmt    = get_rga_fmt(dst_img->format);

    printf("src width=%d height=%d fmt=0x%x virAddr=0x%p fd=%d\n",
        srcWidth, srcHeight, srcFmt, src, src_fd);
    printf("dst width=%d height=%d fmt=0x%x virAddr=0x%p fd=%d\n",
        dstWidth, dstHeight, dstFmt, dst, dst_fd);

    // set rga usage
    int usage = 0;
    IM_STATUS ret_rga = IM_STATUS_NOERROR;

    // set rga rect
    im_rect srect, drect, prect;
    memset(&prect, 0, sizeof(im_rect));

    if (src_box != NULL) {
        srect.x = src_box->left;
        srect.y = src_box->top;
        srect.width  = src_box->right - src_box->left + 1;
        srect.height = src_box->bottom - src_box->top + 1;
    } else {
        srect.x = 0;
        srect.y = 0;
        srect.width  = srcWidth;
        srect.height = srcHeight;
    }

    if (dst_box != NULL) {
        drect.x = dst_box->left;
        drect.y = dst_box->top;
        drect.width  = dst_box->right - dst_box->left + 1;
        drect.height = dst_box->bottom - dst_box->top + 1;
    } else {
        drect.x = 0;
        drect.y = 0;
        drect.width  = dstWidth;
        drect.height = dstHeight;
    }

    // set rga buffer
    rga_buffer_t rga_buf_src, rga_buf_dst, pat;
    rga_buffer_handle_t rga_handle_src = 0;
    rga_buffer_handle_t rga_handle_dst = 0;
    memset(&pat, 0, sizeof(rga_buffer_t));

    im_handle_param_t in_param;
    in_param.width  = srcWidth;
    in_param.height = srcHeight;
    in_param.format = srcFmt;

    im_handle_param_t dst_param;
    dst_param.width  = dstWidth;
    dst_param.height = dstHeight;
    dst_param.format = dstFmt;

    if (src_fd > 0) {
        rga_handle_src = importbuffer_fd(src_fd, &in_param);
    } else if (src != NULL) {
        rga_handle_src = importbuffer_virtualaddr(src, &in_param);
    }
    if (rga_handle_src <= 0) {
        printf("src handle error %d\n", rga_handle_src);
        return -1;
    }
    rga_buf_src = wrapbuffer_handle(rga_handle_src, srcWidth, srcHeight, srcFmt,
                                    srcWidth, srcHeight);

    if (dst_fd > 0) {
        rga_handle_dst = importbuffer_fd(dst_fd, &dst_param);
    } else if (dst != NULL) {
        rga_handle_dst = importbuffer_virtualaddr(dst, &dst_param);
    }
    if (rga_handle_dst <= 0) {
        printf("dst handle error %d\n", rga_handle_dst);
        if (rga_handle_src > 0) releasebuffer_handle(rga_handle_src);
        return -1;
    }
    rga_buf_dst = wrapbuffer_handle(rga_handle_dst, dstWidth, dstHeight, dstFmt,
                                    dstWidth, dstHeight);

    // fill pad color if dst box is smaller than dst image
    if (drect.width != dstWidth || drect.height != dstHeight) {
        im_rect dst_whole_rect = {0, 0, dstWidth, dstHeight};
        int imcolor;
        char* p_imcolor = (char*)&imcolor;
        p_imcolor[0] = color;
        p_imcolor[1] = color;
        p_imcolor[2] = color;
        p_imcolor[3] = color;
        ret_rga = imfill(rga_buf_dst, dst_whole_rect, imcolor);
        if (ret_rga <= 0 && dst != NULL) {
            size_t dst_size = rga_get_image_size(dst_img);
            memset(dst, color, dst_size);
        }
    }

    // rga process
    ret_rga = improcess(rga_buf_src, rga_buf_dst, pat, srect, drect, prect, usage);
    if (ret_rga <= 0) {
        printf("Error on improcess STATUS=%d\n", ret_rga);
        printf("RGA error message: %s\n", imStrError((IM_STATUS)ret_rga));
    }

    if (rga_handle_src > 0) releasebuffer_handle(rga_handle_src);
    if (rga_handle_dst > 0) releasebuffer_handle(rga_handle_dst);

    return (ret_rga > 0) ? 0 : -1;
}
