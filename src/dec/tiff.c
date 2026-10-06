#include "decode.h"
#include <stdlib.h>
#include <string.h>
#include <tiffio.h>

static int fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
    return -1;
}

/* libtiff client callbacks operating on our FILE* (portable, avoids
 * fd/stdio buffering conflicts) */
static tsize_t tiff_read_proc(thandle_t h, tdata_t buf, tsize_t n)
{
    return (tsize_t)fread(buf, 1, (size_t)n, (FILE *)h);
}
static tsize_t tiff_write_proc(thandle_t h, tdata_t buf, tsize_t n)
{
    (void)h; (void)buf; (void)n;
    return 0;                       /* read-only */
}
static toff_t tiff_seek_proc(thandle_t h, toff_t off, int whence)
{
    if (fseek((FILE *)h, (long)off, whence) != 0)
        return (toff_t)-1;
    return (toff_t)ftell((FILE *)h);
}
static int tiff_close_proc(thandle_t h)
{
    (void)h;
    return 0;                       /* do not close our FILE* */
}
static toff_t tiff_size_proc(thandle_t h)
{
    long cur = ftell((FILE *)h);
    fseek((FILE *)h, 0, SEEK_END);
    long size = ftell((FILE *)h);
    fseek((FILE *)h, cur, SEEK_SET);
    return (toff_t)size;
}
static int tiff_map_proc(thandle_t h, tdata_t *b, toff_t *s)
{
    (void)h; (void)b; (void)s;
    return 0;
}
static void tiff_unmap_proc(thandle_t h, tdata_t b, toff_t s)
{
    (void)h; (void)b; (void)s;
}

/* Bit-depth-matching TIFF reader: unlike the RGBA interface (which always
 * downconverts to 8-bit RGBA), this keeps the source's photometric
 * interpretation and bit depth:
 *   MINISWHITE/MINISBLACK 8/16-bit  -> GRAY 8/16 (MINISWHITE is inverted to
 *                                   the 0=black convention)
 *   MINISBLACK 1/2/4-bit            -> GRAY sub-byte, packed MSB-first
 *   RGB 8/16-bit                    -> RGB 8/16
 *   RGB + extra sample (alpha)      -> RGBA 8/16
 *   PALETTE 1/2/4/8-bit             -> PALETTE with 8-bit RGB entries
 * Only chunky (PlanarConfiguration=1) unsigned-integer samples are read;
 * other layouts (planar, CMYK, float) are rejected with a clear message
 * rather than silently downconverted. */

typedef struct {
    int gray;           /* photometric is gray-like */
    int invert;         /* MINISWHITE: invert samples */
    int has_alpha;
} tiff_layout_t;

static int layout_from_tags(uint16_t photometric, uint16_t spp,
                            const uint16_t *extras, uint16_t nextras,
                            tiff_layout_t *lo)
{
    memset(lo, 0, sizeof(*lo));
    switch (photometric) {
    case PHOTOMETRIC_MINISWHITE:
        lo->invert = 1;
        /* fall through */
    case PHOTOMETRIC_MINISBLACK:
        lo->gray = 1;
        break;
    case PHOTOMETRIC_RGB:
        break;
    case PHOTOMETRIC_PALETTE:
        break;
    default:
        return -1;
    }
    /* an extra sample used as alpha?  RGB+alpha has 4 samples, gray+alpha 2 */
    int alpha_sample = extras && nextras >= 1 &&
        (extras[0] == EXTRASAMPLE_ASSOCALPHA || extras[0] == EXTRASAMPLE_UNASSALPHA);
    if (alpha_sample) {
        if (photometric == PHOTOMETRIC_RGB && spp >= 4)
            lo->has_alpha = 1;
        else if (lo->gray && spp >= 2)
            lo->has_alpha = 1;
    }
    return 0;
}

/* Collect the tags we can carry into the .jxl output: physical resolution
 * (which becomes an Exif resolution blob when the source has no Exif of its
 * own), the ICC profile, XMP, and the descriptive text tags. */
static void tiff_read_metadata(TIFF *tif, img_image_t *img)
{
    float xres = 0.0f, yres = 0.0f;
    uint16_t unit = 0;
    if (TIFFGetField(tif, TIFFTAG_XRESOLUTION, &xres) &&
        TIFFGetField(tif, TIFFTAG_YRESOLUTION, &yres) &&
        xres > 0.0f && yres > 0.0f) {
        TIFFGetFieldDefaulted(tif, TIFFTAG_RESOLUTIONUNIT, &unit);
        /* libtiff and Exif agree on the codes: 1 none, 2 inch, 3 cm */
        if (unit == RESUNIT_INCH || unit == RESUNIT_CENTIMETER) {
            img->meta.res_unit = (int)unit;
            img->meta.xres = (double)xres;
            img->meta.yres = (double)yres;
        }
    }

    uint32_t count = 0;
    void *data = NULL;
    if (TIFFGetField(tif, TIFFTAG_ICCPROFILE, &count, &data) && count > 0)
        img_meta_set_icc(&img->meta, (const uint8_t *)data, count);
    count = 0; data = NULL;
    if (TIFFGetField(tif, TIFFTAG_XMLPACKET, &count, &data) && count > 0)
        img_meta_set_xmp(&img->meta, (const uint8_t *)data, count);

    static const struct { uint32_t tag; const char *key; } text_tags[] = {
        { TIFFTAG_IMAGEDESCRIPTION, "Description" },
        { TIFFTAG_SOFTWARE,         "Software" },
        { TIFFTAG_ARTIST,           "Artist" },
        { TIFFTAG_COPYRIGHT,        "Copyright" },
        { TIFFTAG_DATETIME,         "DateTime" },
    };
    for (size_t i = 0; i < sizeof(text_tags) / sizeof(text_tags[0]); i++) {
        char *v = NULL;
        if (TIFFGetField(tif, text_tags[i].tag, &v) && v && v[0])
            img_meta_add_text(&img->meta, text_tags[i].key, NULL, v);
    }
}

int tiff_decode(FILE *f, img_image_t *img, char *err, size_t errlen)
{
    TIFF *tif = TIFFClientOpen("memory", "r", (thandle_t)f,
                               tiff_read_proc, tiff_write_proc,
                               tiff_seek_proc, tiff_close_proc,
                               tiff_size_proc, tiff_map_proc, tiff_unmap_proc);
    if (!tif)
        return fail(err, errlen, "tiff: not a readable TIFF");

    uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    if (w == 0 || h == 0 || w > 1u << 20 || h > 1u << 20) {
        TIFFClose(tif);
        return fail(err, errlen, "tiff: bad dimensions");
    }

    uint16_t photometric = 0, spp = 0, planar = 0, sample_format = 1, bits = 0;
    uint16_t *extras = NULL;
    uint16_t nextras = 0;
    TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &photometric);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sample_format);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bits);
    TIFFGetField(tif, TIFFTAG_EXTRASAMPLES, &nextras, &extras);
    if (!spp || !bits) {
        TIFFClose(tif);
        return fail(err, errlen, "tiff: unsupported sample layout");
    }
    int subok = bits <= 4 && (photometric == PHOTOMETRIC_PALETTE ||
                              photometric == PHOTOMETRIC_MINISBLACK ||
                              photometric == PHOTOMETRIC_MINISWHITE);
    if (sample_format != SAMPLEFORMAT_UINT ||
        (bits != 8 && bits != 16 && !subok)) {
        TIFFClose(tif);
        return fail(err, errlen, "tiff: only unsigned 1/2/4/8/16-bit samples are supported");
    }
    if (planar != PLANARCONFIG_CONTIG) {
        TIFFClose(tif);
        return fail(err, errlen, "tiff: planar TIFF is not supported");
    }
    tiff_layout_t lo;
    if (layout_from_tags(photometric, spp, extras, nextras, &lo) != 0) {
        TIFFClose(tif);
        return fail(err, errlen, "tiff: unsupported photometric interpretation "
                                 "(only gray/RGB/palette)");
    }

    memset(img, 0, sizeof(*img));
    img->width = (int)w;
    img->height = (int)h;
    img->bit_depth = bits;
    size_t tiff_rowbytes = ((size_t)w * bits * spp + 7) / 8;

    /* ---------------- palette ---------------- */
    if (photometric == PHOTOMETRIC_PALETTE) {
        uint16_t *cmap_r = NULL, *cmap_g = NULL, *cmap_b = NULL;
        TIFFGetField(tif, TIFFTAG_COLORMAP, &cmap_r, &cmap_g, &cmap_b);
        if (!cmap_r || bits > 8) {
            TIFFClose(tif);
            return fail(err, errlen, "tiff: bad palette");
        }
        img->color = IMG_PALETTE;
        img->pal_ncolors = 1 << bits;
        for (int i = 0; i < img->pal_ncolors; i++) {
            img->palette[i * 3 + 0] = (uint8_t)(cmap_r[i] >> 8);
            img->palette[i * 3 + 1] = (uint8_t)(cmap_g[i] >> 8);
            img->palette[i * 3 + 2] = (uint8_t)(cmap_b[i] >> 8);
        }
    } else if (lo.gray) {
        img->color = lo.has_alpha ? IMG_GRAY_ALPHA : IMG_GRAY;
    } else {
        img->color = lo.has_alpha ? IMG_RGBA : IMG_RGB;
    }
    int ch = (img->color == IMG_GRAY) ? 1
           : (img->color == IMG_GRAY_ALPHA) ? 2
           : (img->color == IMG_RGB) ? 3
           : (img->color == IMG_RGBA) ? 4
           : 1;
    tiff_read_metadata(tif, img);
    img->rowstride = img_rowstride(img->width, img->bit_depth, ch);
    img->data = (uint8_t *)calloc((size_t)img->height, img->rowstride);
    if (!img->data) {
        TIFFClose(tif);
        return fail(err, errlen, "tiff: out of memory");
    }

    /* ---------------- read strips ---------------- */
    tmsize_t strip_size = TIFFStripSize(tif);
    (void)strip_size;
    uint32_t rows_per_strip = 0;
    TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rows_per_strip);
    if (rows_per_strip == 0 || rows_per_strip > h)
        rows_per_strip = h;
    size_t strip_buf_bytes = (size_t)rows_per_strip * tiff_rowbytes;
    uint8_t *strip = (uint8_t *)_TIFFmalloc(strip_buf_bytes);
    if (!strip) {
        img_free(img);
        TIFFClose(tif);
        return fail(err, errlen, "tiff: out of memory");
    }

    int nstrips = (int)TIFFNumberOfStrips(tif);
    int row = 0;
    int bad = 0;
    for (int s = 0; s < nstrips && !bad; s++) {
        tmsize_t got = TIFFReadEncodedStrip(tif, s, strip, (tmsize_t)strip_buf_bytes);
        if (got <= 0) {
            /* tolerate a truncated final strip: keep the zero-filled rows */
            if (s == nstrips - 1)
                break;
            bad = 1;
            break;
        }
        int nrows = (int)(got / (tmsize_t)tiff_rowbytes);
        if (nrows > (int)rows_per_strip)
            nrows = (int)rows_per_strip;
        for (int r = 0; r < nrows && row < (int)h; r++, row++) {
            const uint8_t *src = strip + (size_t)r * tiff_rowbytes;
            uint8_t *dst = img->data + (size_t)row * img->rowstride;
            if (lo.invert && img->bit_depth <= 4) {
                /* per-sample inversion of packed sub-byte gray rows */
                int per = 8 / img->bit_depth;
                unsigned mask = (1u << img->bit_depth) - 1;
                for (int x = 0; x < (int)w; x++) {
                    int shift = 8 - img->bit_depth - (x % per) * img->bit_depth;
                    unsigned v = (src[x / per] >> shift) & mask;
                    v = mask - v;
                    unsigned outb = dst[x / per];
                    outb &= ~(mask << shift);
                    dst[x / per] = (uint8_t)(outb | (v << shift));
                }
            } else if (img->bit_depth == 16) {
                /* libtiff decodes 16-bit samples in HOST byte order; our
                 * row layout is big-endian (PNG convention) */
                int ns = (ch < (int)spp) ? ch : (int)spp;
                for (int x = 0; x < (int)w; x++) {
                    for (int c = 0; c < ns; c++) {
                        unsigned v;
                        memcpy(&v, src + ((size_t)x * spp + c) * 2, 2);
                        if (lo.invert && c == 0)
                            v = 65535 - v;
                        img_st16be(dst + ((size_t)x * ch + c) * 2, v);
                    }
                }
            } else if (lo.invert) {
                /* MINISWHITE inverts the gray sample only; alpha is copied */
                int ns = (ch < (int)spp) ? ch : (int)spp;
                for (int x = 0; x < (int)w; x++) {
                    for (int c = 0; c < ns; c++) {
                        uint8_t v = src[(size_t)x * spp + c];
                        if (c == 0)
                            v = (uint8_t)(255 - v);
                        dst[(size_t)x * ch + c] = v;
                    }
                }
            } else if (ch == (int)spp) {
                /* direct copy: chunky rows, no padding */
                memcpy(dst, src, img->rowstride);
            } else {
                /* keep only the first ch samples of each pixel */
                for (int x = 0; x < (int)w; x++)
                    memcpy(dst + (size_t)x * ch, src + (size_t)x * spp, (size_t)ch);
            }
        }
    }
    _TIFFfree(strip);
    TIFFClose(tif);
    if (bad) {
        img_free(img);
        return fail(err, errlen, "tiff: decode failed (corrupt or unsupported variant)");
    }
    return 0;
}
