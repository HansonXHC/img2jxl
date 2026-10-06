#ifndef IMG2JXL_CONVERT_H
#define IMG2JXL_CONVERT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include "enc/jxlenc.h"

typedef struct {
    int ok;
    int out_w, out_h;
    int out_depth;
    int out_color;          /* img_color_t of the written JXL */
    int out_frames;         /* 1 for still images, >1 for animations */
    unsigned long long in_size, out_size;
    double secs;
    char err[256];
} img2jxl_result_t;

/* Decode one image file and write it out as JPEG XL (lossless).  Returns
 * 0 on success and fills *result; -1 on failure with result->err. */
int img2jxl_convert(const char *in_path, const char *out_path,
                    const jxl_opts_t *opts, int keep_time,
                    img2jxl_result_t *result);

const char *img2jxl_color_name(int color);

/* Extension-based check for BMP/TGA/PNM/ICO/JPEG/PNG/GIF/QOI/WebP/TIFF/JXL
 * (directory scanners). */
int img2jxl_is_supported_file(const char *path);


#ifdef __cplusplus
}
#endif

#endif
