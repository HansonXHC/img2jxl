#ifndef IMG2JXL_IMAGE_H
#define IMG2JXL_IMAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

/* Color types mirror the PNG model; every supported source format is
 * normalized into one of these while preserving its native bit depth. */
typedef enum {
    IMG_GRAY = 0,    /* 1/2/4/8/16-bit, 1 channel            */
    IMG_GRAY_ALPHA,  /* 8/16-bit, 2 channels                 */
    IMG_PALETTE,     /* 1/2/4/8-bit indices + palette        */
    IMG_RGB,         /* 8/16-bit, 3 channels                 */
    IMG_RGBA         /* 8/16-bit, 4 channels                 */
} img_color_t;

/* Ancillary metadata carried from the source container into the .jxl output.
 * Every field is optional: the encoder writes only what is present, so a
 * format without metadata simply yields a bare .jxl as before. */
typedef struct {
    char *key;              /* text pair, e.g. a PNG tEXt keyword */
    char *lang;             /* iTXt language tag, may be NULL */
    char *value;
} img_text_t;

typedef struct {
    uint8_t *exif;  size_t exif_len;    /* Exif blob, WITHOUT the "Exif\0\0" prefix */
    uint8_t *xmp;   size_t xmp_len;     /* XMP packet, UTF-8 */
    uint8_t *icc;   size_t icc_len;     /* ICC profile */
    double   xres, yres;                /* pixels per resolution unit */
    int      res_unit;                  /* 1 = none, 2 = inch, 3 = cm; 0 = unknown */
    img_text_t *texts;  int ntexts;     /* text chunks / comments */
} img_meta_t;

void img_meta_free(img_meta_t *m);
int  img_meta_set_exif(img_meta_t *m, const uint8_t *d, size_t n);
int  img_meta_set_xmp(img_meta_t *m, const uint8_t *d, size_t n);
int  img_meta_set_icc(img_meta_t *m, const uint8_t *d, size_t n);
int  img_meta_add_text(img_meta_t *m, const char *key, const char *lang,
                       const char *value);
/* True when there is nothing to write. */
int  img_meta_empty(const img_meta_t *m);

typedef struct {
    int width;
    int height;
    int bit_depth;          /* 1, 2, 4, 8 or 16: STORAGE depth of `data` */
    /* Declared sample depth when it differs from the storage depth, else 0.
     * A 10/12-bit JXL is stored unscaled in 16-bit slots with bit_depth 16
     * and nominal_depth 10/12, so re-encoding keeps the original depth
     * instead of rescaling (which would be lossy for 10/12-bit samples). */
    int nominal_depth;
    img_color_t color;

    /* Rows packed exactly like PNG scanlines: each row starts on a byte
     * boundary, sub-byte depths packed MSB-first, 16-bit samples stored
     * big-endian.  The encoder can write rows directly from this buffer. */
    uint8_t *data;
    size_t rowstride;       /* bytes per row */

    /* PALETTE only */
    uint8_t  palette[256 * 3];
    uint8_t  pal_alpha[256];    /* per-entry alpha (tRNS) */
    int      pal_ncolors;
    int      has_pal_alpha;

    /* GRAY only: optional single transparent-gray value (tRNS) */
    int      has_gray_trns;
    uint16_t gray_trns_value;   /* sample value at source depth */

    img_meta_t meta;            /* ancillary metadata; zeroed when none */
} img_image_t;

/* Bytes per output row for the given geometry. */
size_t img_rowstride(int width, int bit_depth, int channels);
int    img_channels(img_color_t color);

/* Declared sample depth: nominal_depth when set, otherwise bit_depth. */
static inline int img_nominal_depth(const img_image_t *img)
{
    return img->nominal_depth > 0 ? img->nominal_depth : img->bit_depth;
}

void   img_free(img_image_t *img);

/* Multi-frame image (animated GIF -> APNG).  Each frame is an RGBA
 * "region" image; x/y are its offset on the canvas.  delays_cs are GIF
 * native centiseconds; dispose is the raw GIF disposal mode (0..3);
 * loops: -1 = play once, 0 = infinite, n = n times. */
typedef struct {
    int width, height;      /* canvas size */
    int nframes;
    img_image_t *frames;    /* nframes entries, RGBA, region-sized */
    int *x, *y;
    int *delays_cs;
    int *dispose;
    int loops;
    img_meta_t meta;        /* canvas-level metadata (GIF comments etc.) */
} img_animation_t;

void   img_free_anim(img_animation_t *anim);

/* big-endian 16-bit sample access (matches PNG sample order) */
static inline void img_st16be(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline unsigned img_ld16be(const uint8_t *p)   { return ((unsigned)p[0] << 8) | p[1]; }

/* Open a file for reading/writing; paths come from the ANSI command line,
 * so they are widened with the ANSI codepage.  Returns NULL on failure. */
FILE *img_fopen_read(const char *path);
FILE *img_fopen_write(const char *path);
/* Win32 handles for GetFileTime/SetFileTime (wide, from ANSI path). */
void *img_open_read_attrs(const char *path);
void *img_open_write_attrs(const char *path);
void  img_close_handle(void *handle);


#ifdef __cplusplus
}
#endif

#endif
