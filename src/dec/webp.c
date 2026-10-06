#include "decode.h"
#include <stdlib.h>
#include <string.h>
#include <webp/decode.h>

static int fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
    return -1;
}

static unsigned rd32le(const uint8_t *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* WebP keeps metadata in RIFF chunks: ICCP (ICC profile), EXIF (a bare TIFF
 * blob, matching our meta.exif) and XMP (an XMP packet). */
static void webp_read_metadata(const uint8_t *buf, size_t size, img_image_t *img)
{
    if (size < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WEBP", 4) != 0)
        return;
    size_t off = 12;
    while (off + 8 <= size) {
        const uint8_t *type = buf + off;
        unsigned clen = rd32le(buf + off + 4);
        const uint8_t *data = buf + off + 8;
        if (off + 8 + clen > size)
            break;                                  /* truncated chunk */
        if (!memcmp(type, "ICCP", 4))
            img_meta_set_icc(&img->meta, data, clen);
        else if (!memcmp(type, "EXIF", 4))
            img_meta_set_exif(&img->meta, data, clen);
        else if (!memcmp(type, "XMP ", 4))
            img_meta_set_xmp(&img->meta, data, clen);
        off += 8 + clen + (clen & 1);               /* chunks are even-padded */
    }
}

int webp_decode(FILE *f, img_image_t *img, char *err, size_t errlen)
{
    if (fseek(f, 0, SEEK_END) != 0)
        return fail(err, errlen, "webp: seek failed");
    long fsize = ftell(f);
    rewind(f);
    if (fsize < 16)
        return fail(err, errlen, "webp: file too small");
    uint8_t *buf = (uint8_t *)malloc((size_t)fsize);
    if (!buf)
        return fail(err, errlen, "webp: out of memory");
    if (fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(buf);
        return fail(err, errlen, "webp: read failed");
    }

    WebPBitstreamFeatures features;
    VP8StatusCode st = WebPGetFeatures(buf, (size_t)fsize, &features);
    if (st != VP8_STATUS_OK) {
        free(buf);
        return fail(err, errlen, "webp: invalid WebP data");
    }

    int w = 0, h = 0;
    int has_alpha = features.has_alpha;
    webp_read_metadata(buf, (size_t)fsize, img);
    uint8_t *pixels = has_alpha
        ? WebPDecodeRGBA(buf, (size_t)fsize, &w, &h)
        : WebPDecodeRGB(buf, (size_t)fsize, &w, &h);
    free(buf);
    if (!pixels) {
        return fail(err, errlen, "webp: decode failed");
    }

    /* metadata was filled into *img above; keep it across the re-init */
    img_meta_t meta = img->meta;
    memset(img, 0, sizeof(*img));
    img->meta = meta;
    img->width = w;
    img->height = h;
    img->bit_depth = 8;
    img->color = has_alpha ? IMG_RGBA : IMG_RGB;
    int ch = has_alpha ? 4 : 3;
    img->rowstride = img_rowstride(w, 8, ch);
    img->data = (uint8_t *)malloc((size_t)w * h * (size_t)ch);
    if (!img->data) {
        WebPFree(pixels);
        return fail(err, errlen, "webp: out of memory");
    }
    memcpy(img->data, pixels, (size_t)w * h * (size_t)ch);
    WebPFree(pixels);
    return 0;
}
