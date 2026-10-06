#include "jxlenc.h"
#include <stdlib.h>
#include <string.h>
#include <jxl/encode.h>
#include <jxl/codestream_header.h>
#include <jxl/color_encoding.h>
#include <jxl/types.h>

static int fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
    return -1;
}

/* defined in the ancillary-metadata section below */
static int add_metadata(JxlEncoder *enc, const img_meta_t *m, int keep,
                        int *used_icc, char *err, size_t errlen);

/* ------------------------------------------------------------------ */
/* growing output buffer drained via JxlEncoderProcessOutput           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *data;
    size_t size, cap;
} outbuf_t;

static int drain(JxlEncoder *enc, outbuf_t *ob, char *err, size_t errlen)
{
    size_t chunk = (size_t)1 << 16;
    ob->data = (uint8_t *)malloc(chunk);
    if (!ob->data)
        return fail(err, errlen, "jxl: out of memory");
    ob->cap = chunk;
    ob->size = 0;

    for (;;) {
        uint8_t *next_out = ob->data + ob->size;
        size_t avail = ob->cap - ob->size;
        JxlEncoderStatus st = JxlEncoderProcessOutput(enc, &next_out, &avail);
        ob->size = (size_t)(next_out - ob->data);
        if (st == JXL_ENC_SUCCESS)
            return 0;
        if (st != JXL_ENC_NEED_MORE_OUTPUT)
            return fail(err, errlen, "jxl: encoder failed");
        ob->cap *= 2;
        uint8_t *grown = (uint8_t *)realloc(ob->data, ob->cap);
        if (!grown)
            return fail(err, errlen, "jxl: out of memory");
        ob->data = grown;
    }
}

static int write_out(const char *path, const outbuf_t *ob, char *err, size_t errlen)
{
    FILE *f = img_fopen_write(path);
    if (!f)
        return fail(err, errlen, "jxl: cannot open output file");
    if (fwrite(ob->data, 1, ob->size, f) != ob->size) {
        fclose(f);
        remove(path);
        return fail(err, errlen, "jxl: write failed");
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* shared encoder setup                                                */
/* ------------------------------------------------------------------ */

static int set_options(JxlEncoderFrameSettings *fs, const jxl_opts_t *opts)
{
    int effort = opts->effort;
    if (effort < 1 || effort > 10)
        effort = 10;
    if (JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_EFFORT,
                                         effort) != JXL_ENC_SUCCESS)
        return -1;
    if (JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_MODULAR,
                                         opts->modular ? 1 : 0) != JXL_ENC_SUCCESS)
        return -1;
    return 0;
}

/* Basic info for a pixel buffer with the given channel count (1=gray,
 * 2=gray+alpha, 3=rgb, 4=rgba).  `storage` is the container depth of the
 * buffer (8 or 16) and `nominal` the declared sample depth; they differ for
 * 10/12-bit sources, whose samples are passed unscaled (see
 * JXL_BIT_DEPTH_FROM_CODESTREAM). */
static int set_basic_info(JxlEncoder *enc, int w, int h, int channels,
                          int storage, int nominal, int animation,
                          uint32_t num_loops)
{
    JxlBasicInfo bi;
    JxlEncoderInitBasicInfo(&bi);
    bi.xsize = (uint32_t)w;
    bi.ysize = (uint32_t)h;
    bi.bits_per_sample = (uint32_t)nominal;
    bi.exponent_bits_per_sample = 0;
    bi.num_color_channels = (channels == 1 || channels == 2) ? 1 : 3;
    int has_alpha = (channels == 2 || channels == 4);
    bi.num_extra_channels = has_alpha ? 1 : 0;
    bi.alpha_bits = has_alpha ? (uint32_t)nominal : 0;
    bi.alpha_premultiplied = JXL_FALSE;
    bi.uses_original_profile = JXL_TRUE;    /* lossless: keep the original profile */
    if (animation) {
        bi.have_animation = JXL_TRUE;
        bi.animation.tps_numerator = 100;   /* 100 ticks/s: 1 tick = 1 GIF centisecond */
        bi.animation.tps_denominator = 1;
        bi.animation.num_loops = num_loops;
        bi.animation.have_timecodes = JXL_FALSE;
    }
    if (JxlEncoderSetBasicInfo(enc, &bi) != JXL_ENC_SUCCESS)
        return -1;
    (void)storage;

    if (has_alpha) {
        JxlExtraChannelInfo eci;
        JxlEncoderInitExtraChannelInfo(JXL_CHANNEL_ALPHA, &eci);
        eci.bits_per_sample = (uint32_t)nominal;
        eci.alpha_premultiplied = JXL_FALSE;
        if (JxlEncoderSetExtraChannelInfo(enc, 0, &eci) != JXL_ENC_SUCCESS)
            return -1;
    }
    return 0;
}

/* Plain sRGB color encoding, used when there is no ICC profile to carry. */
static int set_srgb(JxlEncoder *enc, int channels)
{
    JxlColorEncoding ce;
    JxlColorEncodingSetToSRGB(&ce, (channels == 1 || channels == 2) ? JXL_TRUE : JXL_FALSE);
    return JxlEncoderSetColorEncoding(enc, &ce) == JXL_ENC_SUCCESS ? 0 : -1;
}

/* Set up metadata + color encoding: the ICC profile (when present) replaces
 * the sRGB color encoding, and everything else becomes container boxes. */
static int setup_metadata_and_color(JxlEncoder *enc, const img_meta_t *m,
                                    const jxl_opts_t *opts, int channels,
                                    char *err, size_t errlen)
{
    int used_icc = 0;
    if (add_metadata(enc, m, opts->keep_metadata, &used_icc, err, errlen) != 0)
        return -1;
    if (!used_icc && set_srgb(enc, channels) != 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* ancillary metadata                                                  */
/* ------------------------------------------------------------------ */

static void put_be16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* A minimal big-endian Exif blob carrying just the physical resolution.
 * Used when the source states a DPI (PNG pHYs, TIFF resolution, JFIF
 * density) but has no Exif of its own to carry it. */
static uint8_t *build_res_exif(double xres, double yres, int unit, size_t *outlen)
{
    enum { BLOB = 8 + 2 + 3 * 12 + 4 + 8 + 8 };   /* values start at 50 */
    uint8_t *b = (uint8_t *)calloc(1, BLOB);
    if (!b)
        return NULL;
    memcpy(b, "MM", 2);
    put_be16(b + 2, 42);
    put_be32(b + 4, 8);                 /* IFD0 offset */
    put_be16(b + 8, 3);                 /* three entries */
    uint8_t *e = b + 10;
    put_be16(e +  0, 0x011A); put_be16(e +  2, 5); put_be32(e +  4, 1); put_be32(e +  8, 50);
    put_be16(e + 12, 0x011B); put_be16(e + 14, 5); put_be32(e + 16, 1); put_be32(e + 20, 58);
    put_be16(e + 24, 0x0128); put_be16(e + 26, 3); put_be32(e + 28, 1); put_be16(e + 32, (unsigned)unit);
    /* next IFD offset stays 0 */
    put_be32(b + 50, (unsigned)(xres * 100.0 + 0.5)); put_be32(b + 54, 100);
    put_be32(b + 58, (unsigned)(yres * 100.0 + 0.5)); put_be32(b + 62, 100);
    *outlen = BLOB;
    return b;
}

/* Serialize text pairs into the payload of the private "jxtx" box:
 * repeated { u32 keylen, key, u32 langlen, lang, u32 vallen, value }. */
static uint8_t *build_text_box(const img_meta_t *m, size_t *outlen)
{
    size_t need = 0;
    for (int i = 0; i < m->ntexts; i++)
        need += 12 + strlen(m->texts[i].key) +
                (m->texts[i].lang ? strlen(m->texts[i].lang) : 0) +
                strlen(m->texts[i].value);
    uint8_t *buf = (uint8_t *)malloc(need ? need : 1);
    if (!buf)
        return NULL;
    uint8_t *p = buf;
    for (int i = 0; i < m->ntexts; i++) {
        const img_text_t *t = &m->texts[i];
        size_t kl = strlen(t->key), ll = t->lang ? strlen(t->lang) : 0;
        size_t vl = strlen(t->value);
        put_be32(p, (unsigned)kl); p += 4;
        memcpy(p, t->key, kl); p += kl;
        put_be32(p, (unsigned)ll); p += 4;
        if (ll) { memcpy(p, t->lang, ll); p += ll; }
        put_be32(p, (unsigned)vl); p += 4;
        memcpy(p, t->value, vl); p += vl;
    }
    *outlen = (size_t)(p - buf);
    return buf;
}

/* Write ICC / Exif / XMP / text into the container.  Returns 0 on success;
 * *used_icc is set when the ICC profile was accepted (the caller then skips
 * the plain sRGB color encoding). */
static int add_metadata(JxlEncoder *enc, const img_meta_t *m, int keep,
                        int *used_icc, char *err, size_t errlen)
{
    *used_icc = 0;
    if (!keep || img_meta_empty(m))
        return 0;

    if (m->icc && m->icc_len &&
        JxlEncoderSetICCProfile(enc, m->icc, m->icc_len) == JXL_ENC_SUCCESS)
        *used_icc = 1;

    /* Exif: from the source, else synthesized from its resolution */
    uint8_t *exif = NULL, *exif_owned = NULL;
    size_t exif_len = 0;
    if (m->exif && m->exif_len) {
        exif = m->exif;
        exif_len = m->exif_len;
    } else if (m->xres > 0 && m->yres > 0) {
        exif_owned = build_res_exif(m->xres, m->yres,
                                    m->res_unit ? m->res_unit : 2, &exif_len);
        exif = exif_owned;
    }

    uint8_t *texts = NULL;
    size_t texts_len = 0;
    if (m->ntexts > 0)
        texts = build_text_box(m, &texts_len);

    if (exif_len || (m->xmp && m->xmp_len) || texts_len)
        JxlEncoderUseBoxes(enc);

    if (exif_len) {
        /* the Exif box content starts with a 4-byte offset to the TIFF header */
        uint8_t *blob = (uint8_t *)calloc(1, exif_len + 4);
        if (!blob) {
            free(exif_owned);
            free(texts);
            return fail(err, errlen, "jxl: out of memory");
        }
        memcpy(blob + 4, exif, exif_len);
        JxlEncoderStatus st = JxlEncoderAddBox(enc, "Exif", blob, exif_len + 4,
                                               JXL_FALSE);
        free(blob);
        if (st != JXL_ENC_SUCCESS) {
            free(exif_owned);
            free(texts);
            return fail(err, errlen, "jxl: cannot add Exif box");
        }
    }
    if (m->xmp && m->xmp_len) {
        if (JxlEncoderAddBox(enc, "xml ", m->xmp, m->xmp_len, JXL_FALSE)
            != JXL_ENC_SUCCESS) {
            free(exif_owned);
            free(texts);
            return fail(err, errlen, "jxl: cannot add XMP box");
        }
    }
    if (texts_len) {
        JxlEncoderStatus st = JxlEncoderAddBox(enc, "jxtx", texts, texts_len,
                                               JXL_FALSE);
        if (st != JXL_ENC_SUCCESS) {
            free(exif_owned);
            free(texts);
            return fail(err, errlen, "jxl: cannot add text box");
        }
    }
    if (exif_len || (m->xmp && m->xmp_len) || texts_len)
        JxlEncoderCloseBoxes(enc);

    free(exif_owned);
    free(texts);
    return 0;
}

/* ------------------------------------------------------------------ */
/* still-image pixel buffer construction                               */
/* ------------------------------------------------------------------ */

/* Expand a PNG-style sub-byte gray sample to 8-bit (exact scaling:
 * 255/(2^d - 1) is an integer for d in {1,2,4}). */
static uint8_t scale_subbyte(unsigned v, int depth)
{
    static const unsigned mult[8] = { 0, 255, 85, 0, 17, 0, 0, 0 };
    return (uint8_t)(v * mult[depth]);
}

static unsigned get_subbyte(const uint8_t *row, int x, int depth)
{
    int per = 8 / depth;                    /* samples per byte (1 for d=8) */
    unsigned byte = row[x / per];
    int shift = 8 - depth - (x % per) * depth;
    return (byte >> shift) & ((1u << depth) - 1);
}

/* Build an interleaved native-endian pixel buffer from img (rows are
 * PNG-style packed: sub-byte MSB-first, 16-bit big-endian).  Returns a
 * malloc'ed buffer (caller frees) and sets *channels/*is16/*sample_bits. */
static uint8_t *build_pixels(const img_image_t *img, int *channels,
                             int *is16, char *err, size_t errlen)
{
    int w = img->width, h = img->height;
    size_t n = (size_t)w * h;

    switch (img->color) {
    case IMG_GRAY: {
        if (img->bit_depth == 8 && !img->has_gray_trns) {
            *channels = 1; *is16 = 0;
            uint8_t *buf = (uint8_t *)malloc(n);
            if (!buf) break;
            for (int y = 0; y < h; y++)
                memcpy(buf + (size_t)y * w, img->data + (size_t)y * img->rowstride, (size_t)w);
            return buf;
        }
        if (img->bit_depth == 16 && !img->has_gray_trns) {
            *channels = 1; *is16 = 1;
            uint16_t *buf = (uint16_t *)malloc(n * 2);
            if (!buf) break;
            for (int y = 0; y < h; y++) {
                const uint8_t *s = img->data + (size_t)y * img->rowstride;
                for (int x = 0; x < w; x++)
                    buf[(size_t)y * w + x] = (uint16_t)img_ld16be(s + (size_t)x * 2);
            }
            return (uint8_t *)buf;
        }
        if (img->bit_depth == 16) {
            /* 16-bit gray + tRNS: transparent samples become alpha 0 */
            *channels = 2; *is16 = 1;
            uint16_t *buf = (uint16_t *)malloc(n * 4);
            if (!buf) break;
            for (int y = 0; y < h; y++) {
                const uint8_t *s = img->data + (size_t)y * img->rowstride;
                for (int x = 0; x < w; x++) {
                    unsigned v = img_ld16be(s + (size_t)x * 2);
                    buf[(size_t)y * w * 2 + (size_t)x * 2] = (uint16_t)v;
                    buf[(size_t)y * w * 2 + (size_t)x * 2 + 1] =
                        (img->has_gray_trns && v == img->gray_trns_value) ? 0 : 65535;
                }
            }
            return (uint8_t *)buf;
        }
        /* sub-byte or 8-bit gray, with an optional tRNS value */
        int ch = img->has_gray_trns ? 2 : 1;
        *channels = ch; *is16 = 0;
        uint8_t *buf = (uint8_t *)malloc(n * (size_t)ch);
        if (!buf) break;
        for (int y = 0; y < h; y++) {
            const uint8_t *s = img->data + (size_t)y * img->rowstride;
            uint8_t *d = buf + (size_t)y * w * ch;
            for (int x = 0; x < w; x++) {
                unsigned v = (img->bit_depth == 8)
                    ? s[x] : get_subbyte(s, x, img->bit_depth);
                d[(size_t)x * ch] = (img->bit_depth == 8)
                    ? (uint8_t)v : scale_subbyte(v, img->bit_depth);
                if (ch == 2)
                    d[(size_t)x * ch + 1] =
                        (img->has_gray_trns && v == img->gray_trns_value) ? 0 : 255;
            }
        }
        return buf;
    }
    case IMG_GRAY_ALPHA: {
        int ch = 2;
        *channels = ch; *is16 = (img->bit_depth == 16);
        size_t step = *is16 ? 2 : 1;
        uint8_t *buf = (uint8_t *)malloc(n * 2 * step);
        if (!buf) break;
        for (int y = 0; y < h; y++) {
            const uint8_t *s = img->data + (size_t)y * img->rowstride;
            uint8_t *d = buf + (size_t)y * w * 2 * step;
            if (*is16) {
                for (int x = 0; x < w; x++) {
                    uint16_t g = (uint16_t)img_ld16be(s + (size_t)x * 4);
                    uint16_t a = (uint16_t)img_ld16be(s + (size_t)x * 4 + 2);
                    ((uint16_t *)d)[(size_t)x * 2] = g;
                    ((uint16_t *)d)[(size_t)x * 2 + 1] = a;
                }
            } else {
                memcpy(d, s, (size_t)w * 2);
            }
        }
        return buf;
    }
    case IMG_PALETTE: {
        int ch = img->has_pal_alpha ? 4 : 3;
        *channels = ch; *is16 = 0;
        uint8_t *buf = (uint8_t *)malloc(n * (size_t)ch);
        if (!buf) break;
        for (int y = 0; y < h; y++) {
            const uint8_t *s = img->data + (size_t)y * img->rowstride;
            uint8_t *d = buf + (size_t)y * w * ch;
            for (int x = 0; x < w; x++) {
                unsigned idx = (img->bit_depth == 8)
                    ? s[x] : get_subbyte(s, x, img->bit_depth);
                if (idx >= (unsigned)img->pal_ncolors)
                    idx = 0;
                d[(size_t)x * ch + 0] = img->palette[idx * 3 + 0];
                d[(size_t)x * ch + 1] = img->palette[idx * 3 + 1];
                d[(size_t)x * ch + 2] = img->palette[idx * 3 + 2];
                if (ch == 4)
                    d[(size_t)x * ch + 3] = img->has_pal_alpha ? img->pal_alpha[idx] : 255;
            }
        }
        return buf;
    }
    case IMG_RGB:
    case IMG_RGBA: {
        int ch = (img->color == IMG_RGB) ? 3 : 4;
        *channels = ch; *is16 = (img->bit_depth == 16);
        size_t step = *is16 ? 2 : 1;
        uint8_t *buf = (uint8_t *)malloc(n * (size_t)ch * step);
        if (!buf) break;
        for (int y = 0; y < h; y++) {
            const uint8_t *s = img->data + (size_t)y * img->rowstride;
            uint8_t *d = buf + (size_t)y * w * ch * step;
            if (*is16) {
                for (int x = 0; x < w; x++)
                    for (int c = 0; c < ch; c++)
                        ((uint16_t *)d)[(size_t)x * ch + c] =
                            (uint16_t)img_ld16be(s + ((size_t)x * ch + c) * 2);
            } else {
                memcpy(d, s, (size_t)w * ch);
            }
        }
        return buf;
    }
    }
    *channels = 0; *is16 = 0;
    return NULL;
}

int jxl_write_file(const img_image_t *img, const jxl_opts_t *opts,
                   const char *path, char *err, size_t errlen)
{
    if (!img || !img->data)
        return fail(err, errlen, "jxl: no image data");

    int channels = 0, is16 = 0;
    uint8_t *pixels = build_pixels(img, &channels, &is16, err, errlen);
    if (!pixels)
        return channels ? fail(err, errlen, "jxl: out of memory")
                        : fail(err, errlen, "jxl: unsupported color model");

    int sample_bits = is16 ? 16 : 8;
    size_t size = (size_t)img->width * img->height * (size_t)channels * (is16 ? 2 : 1);

    JxlEncoder *enc = JxlEncoderCreate(NULL);
    JxlEncoderFrameSettings *fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    int rc = -1;
    outbuf_t ob = {0};

    if (set_options(fs, opts) != 0) {
        fail(err, errlen, "jxl: cannot set frame options");
        goto done;
    }
    if (JxlEncoderSetFrameLossless(fs, JXL_TRUE) != JXL_ENC_SUCCESS) {
        fail(err, errlen, "jxl: cannot set lossless mode");
        goto done;
    }
    int nominal_bits = img_nominal_depth(img);
    if (set_basic_info(enc, img->width, img->height, channels,
                       sample_bits, nominal_bits, 0, 0) != 0) {
        fail(err, errlen, "jxl: cannot set basic info");
        goto done;
    }
    if (setup_metadata_and_color(enc, &img->meta, opts, channels,
                                 err, errlen) != 0)
        goto done;

    JxlPixelFormat pf;
    memset(&pf, 0, sizeof(pf));
    pf.num_channels = (uint32_t)channels;
    pf.data_type = is16 ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8;
    pf.endianness = JXL_NATIVE_ENDIAN;
    pf.align = 0;

    /* when the declared depth is below the container depth (10/12-bit in
     * 16-bit slots) the samples must be taken unscaled */
    if (nominal_bits != sample_bits) {
        JxlBitDepth bd;
        bd.type = JXL_BIT_DEPTH_FROM_CODESTREAM;
        bd.bits_per_sample = (uint32_t)nominal_bits;
        bd.exponent_bits_per_sample = 0;
        if (JxlEncoderSetFrameBitDepth(fs, &bd) != JXL_ENC_SUCCESS) {
            fail(err, errlen, "jxl: cannot set frame bit depth");
            goto done;
        }
    }

    if (JxlEncoderAddImageFrame(fs, &pf, pixels, size) != JXL_ENC_SUCCESS) {
        fail(err, errlen, "jxl: cannot add image frame");
        goto done;
    }
    JxlEncoderCloseInput(enc);
    if (drain(enc, &ob, err, errlen) != 0)
        goto done;
    rc = write_out(path, &ob, err, errlen);

done:
    if (rc != 0)
        remove(path);
    JxlEncoderDestroy(enc);
    free(ob.data);
    free(pixels);
    return rc;
}

/* ------------------------------------------------------------------ */
/* animation                                                           */
/* ------------------------------------------------------------------ */

/* Encode one frame: a canvas-composited RGBA8 region drawn with
 * JXL_BLEND_REPLACE (the region already holds the exact canvas state for
 * its rect).  The blended result is saved to reference slot 1, which the
 * next frame uses as its base (slot 0 is only used by the first frame,
 * where it is the empty canvas). */
static int add_anim_frame(JxlEncoderFrameSettings *fs, const img_image_t *frame,
                          int x, int y, int duration, int source_slot)
{
    JxlFrameHeader fh;
    JxlEncoderInitFrameHeader(&fh);
    fh.duration = (uint32_t)duration;
    /* layer crop = the region rect */
    fh.layer_info.have_crop = JXL_TRUE;
    fh.layer_info.crop_x0 = x;
    fh.layer_info.crop_y0 = y;
    fh.layer_info.xsize = (uint32_t)frame->width;
    fh.layer_info.ysize = (uint32_t)frame->height;
    fh.layer_info.blend_info.blendmode = JXL_BLEND_REPLACE;
    fh.layer_info.blend_info.source = (uint32_t)source_slot;
    fh.layer_info.blend_info.clamp = JXL_TRUE;
    fh.layer_info.blend_info.alpha = 0;
    fh.layer_info.save_as_reference = 1u;
    if (JxlEncoderSetFrameHeader(fs, &fh) != JXL_ENC_SUCCESS)
        return -1;
    JxlBlendInfo eb = fh.layer_info.blend_info;
    eb.clamp = JXL_FALSE;
    JxlEncoderSetExtraChannelBlendInfo(fs, 0, &eb);

    JxlPixelFormat pf;
    memset(&pf, 0, sizeof(pf));
    pf.num_channels = 4;
    pf.data_type = JXL_TYPE_UINT8;
    pf.endianness = JXL_NATIVE_ENDIAN;
    pf.align = 0;
    size_t size = (size_t)frame->width * frame->height * 4;
    if (JxlEncoderAddImageFrame(fs, &pf, frame->data, size) != JXL_ENC_SUCCESS)
        return -1;
    return 0;
}

/* Zero-duration fully-transparent frame over the same rect: clears the
 * disposed area from the reference slot (GIF dispose-to-background). */
static int add_clear_frame(JxlEncoderFrameSettings *fs, const img_image_t *frame,
                           int x, int y)
{
    img_image_t blank = *frame;
    uint8_t *zeros = (uint8_t *)calloc((size_t)frame->width * frame->height, 4);
    if (!zeros)
        return -1;
    blank.data = zeros;
    int rc = add_anim_frame(fs, &blank, x, y, 0, 1);
    free(zeros);
    return rc;
}

int jxl_write_anim(const img_animation_t *anim, const jxl_opts_t *opts,
                   const char *path, char *err, size_t errlen)
{
    if (!anim || anim->nframes <= 0 || !anim->frames)
        return fail(err, errlen, "jxl: no animation frames");

    JxlEncoder *enc = JxlEncoderCreate(NULL);
    JxlEncoderFrameSettings *fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    int rc = -1;
    outbuf_t ob = {0};

    if (set_options(fs, opts) != 0) {
        fail(err, errlen, "jxl: cannot set frame options");
        goto done;
    }
    if (JxlEncoderSetFrameLossless(fs, JXL_TRUE) != JXL_ENC_SUCCESS) {
        fail(err, errlen, "jxl: cannot set lossless mode");
        goto done;
    }
    uint32_t num_loops = anim->loops < 0 ? 1u : (uint32_t)anim->loops;
    if (set_basic_info(enc, anim->width, anim->height, 4, 8, 8, 1, num_loops) != 0) {
        fail(err, errlen, "jxl: cannot set basic info");
        goto done;
    }
    if (setup_metadata_and_color(enc, &anim->meta, opts, 4, err, errlen) != 0)
        goto done;

    for (int i = 0; i < anim->nframes; i++) {
        if (add_anim_frame(fs, &anim->frames[i], anim->x[i], anim->y[i],
                           anim->delays_cs[i], i == 0 ? 0 : 1) != 0) {
            fail(err, errlen, "jxl: cannot add animation frame");
            goto done;
        }
        /* after dispose-to-background (2) or -to-previous (3, approximated),
         * clear the frame's rect from the reference before the next one */
        if (anim->dispose[i] == 2 || anim->dispose[i] == 3) {
            if (add_clear_frame(fs, &anim->frames[i], anim->x[i], anim->y[i]) != 0) {
                fail(err, errlen, "jxl: out of memory");
                goto done;
            }
        }
    }
    JxlEncoderCloseInput(enc);
    if (drain(enc, &ob, err, errlen) != 0)
        goto done;
    rc = write_out(path, &ob, err, errlen);

done:
    if (rc != 0)
        remove(path);
    JxlEncoderDestroy(enc);
    free(ob.data);
    return rc;
}

/* ------------------------------------------------------------------ */
/* JPEG bitstream transcoding                                          */
/* ------------------------------------------------------------------ */

int jxl_transcode_jpeg(const uint8_t *jpeg, size_t size, const jxl_opts_t *opts,
                       const char *path, char *err, size_t errlen)
{
    JxlEncoder *enc = JxlEncoderCreate(NULL);
    JxlEncoderFrameSettings *fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    int rc = -1;
    outbuf_t ob = {0};

    if (set_options(fs, opts) != 0) {
        fail(err, errlen, "jxl: cannot set frame options");
        goto done;
    }
    /* Keep the JPEG-derived metadata boxes (Exif/XMP/JUMBF) uncompressed.
     * libjxl defaults to brotli-compressing them into "brob" boxes, which
     * most viewers do not decompress — the camera metadata then looks lost
     * even though it is present in the file. */
    if (JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_JPEG_COMPRESS_BOXES, 0)
        != JXL_ENC_SUCCESS) {
        fail(err, errlen, "jxl: cannot disable metadata box compression");
        goto done;
    }
    if (JxlEncoderStoreJPEGMetadata(enc, JXL_TRUE) != JXL_ENC_SUCCESS) {
        fail(err, errlen, "jxl: cannot enable JPEG reconstruction");
        goto done;
    }
    if (JxlEncoderAddJPEGFrame(fs, jpeg, size) != JXL_ENC_SUCCESS) {
        fail(err, errlen, "jxl: JPEG bitstream cannot be transcoded");
        goto done;
    }
    JxlEncoderCloseInput(enc);
    if (drain(enc, &ob, err, errlen) != 0)
        goto done;
    rc = write_out(path, &ob, err, errlen);

done:
    if (rc != 0)
        remove(path);
    JxlEncoderDestroy(enc);
    free(ob.data);
    return rc;
}

int jpeg_parse_dims(const uint8_t *buf, size_t size, int *w, int *h, int *gray)
{
    *w = *h = 0;
    *gray = 0;
    size_t i = 2;   /* skip SOI */
    while (i + 4 <= size) {
        if (buf[i] != 0xFF) {
            i++;
            continue;
        }
        uint8_t marker = buf[i + 1];
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            i += 2;
            continue;
        }
        if (i + 4 > size)
            break;
        size_t seglen = ((size_t)buf[i + 2] << 8) | buf[i + 3];
        if ((marker >= 0xC0 && marker <= 0xCF) &&
            marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            if (i + 9 > size)
                return -1;
            *h = ((int)buf[i + 5] << 8) | buf[i + 6];
            *w = ((int)buf[i + 7] << 8) | buf[i + 8];
            *gray = (buf[i + 9] == 1);
            return (*w > 0 && *h > 0) ? 0 : -1;
        }
        i += 2 + seglen;
    }
    return -1;
}
