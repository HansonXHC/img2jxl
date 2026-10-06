#ifndef IMG2JXL_JXLENC_H
#define IMG2JXL_JXLENC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stddef.h>
#include "image.h"

/* JPEG XL encoder options.  The tool is lossless-only: the codestream
 * distance is always 0. */
typedef struct {
    int effort;         /* 1-10 (1=lightning .. 10=glacier), default 10 */
    int modular;        /* enforce Modular mode (default 1; automatic
                         * predictor/transform selection is built into it) */
    int auto_optimize;  /* drop useless alpha / gray detection (applied by
                         * the caller on decoded pixels) */
} jxl_opts_t;

/* Encode *img as a .jxl file, preserving its color model and bit depth
 * (palette is expanded to RGB(A) and sub-byte gray to 8-bit, both lossless
 * value-wise; Modular's automatic palette transform recovers the savings).
 * Returns 0 on success, -1 with a reason in err. */
int jxl_write_file(const img_image_t *img, const jxl_opts_t *opts,
                   const char *path, char *err, size_t errlen);

/* Encode *anim (canvas-composited RGBA regions, as produced by
 * gif_decode_anim) as an animated .jxl file: per-frame duration in ticks
 * (100 ticks/second so delays_cs map 1:1), loop count, and per-frame
 * offsets.  GIF dispose-to-background is reproduced with zero-duration
 * clear frames; dispose-to-previous is approximated (see README).
 * Returns 0 on success, -1 with a reason in err. */
int jxl_write_anim(const img_animation_t *anim, const jxl_opts_t *opts,
                   const char *path, char *err, size_t errlen);

/* Losslessly re-compress a JPEG bitstream as a JPEG XL file (JxlEncoderAddJPEGFrame):
 * the DCT coefficients are transcoded and reconstruction data is stored so
 * djxl can restore the original JPEG byte-for-byte.  Returns 0 on success,
 * -1 with a reason in err (caller may fall back to the pixel path). */
int jxl_transcode_jpeg(const uint8_t *jpeg, size_t size, const jxl_opts_t *opts,
                       const char *path, char *err, size_t errlen);

/* Minimal JPEG SOF scan used to report dimensions/color without decoding. */
int jpeg_parse_dims(const uint8_t *buf, size_t size,
                    int *w, int *h, int *gray);

#ifdef __cplusplus
}
#endif

#endif
