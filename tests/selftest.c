/* Self-test: generates sample images in various formats, converts them with
 * the same core code the CLI uses, then reads the .jxl files back with the
 * vendored libjxl decoder and compares pixel-by-pixel.  JPEG inputs are
 * verified byte-for-byte through JPEG XL reconstruction.  Exits 0 when
 * everything matches. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#endif

#include <setjmp.h>
#include <jpeglib.h>
#include <gif_lib.h>
#include <qoi.h>
#include <webp/encode.h>
#include <tiffio.h>
#include <png.h>
#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/codestream_header.h>
#include <jxl/color_encoding.h>
#include <jxl/types.h>

#include "image.h"
#include "dec/decode.h"
#include "enc/jxlenc.h"
#include "convert.h"
#include "util/filetime.h"

static int g_failures = 0;
static uint8_t *wp_blob = NULL;

#define CHECK(cond, name)                                                  \
    do {                                                                   \
        if (cond) printf("  PASS  %s\n", name);                            \
        else     { printf("  FAIL  %s\n", name); g_failures++; }           \
    } while (0)

static void fill_gradient_rgba(uint8_t *d, int w, int h, int alpha)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            d[(size_t)(y * w + x) * 4 + 0] = (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1));
            d[(size_t)(y * w + x) * 4 + 1] = (uint8_t)(y * 255 / (h > 1 ? h - 1 : 1));
            d[(size_t)(y * w + x) * 4 + 2] = (uint8_t)((x ^ y) & 0xFF);
            d[(size_t)(y * w + x) * 4 + 3] = (uint8_t)alpha;
        }
}

static void fill_gradient_rgb(uint8_t *d, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            d[(size_t)(y * w + x) * 3 + 0] = (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1));
            d[(size_t)(y * w + x) * 3 + 1] = (uint8_t)(y * 255 / (h > 1 ? h - 1 : 1));
            d[(size_t)(y * w + x) * 3 + 2] = (uint8_t)((x ^ y) & 0xFF);
        }
}

static void fill_gradient_gray(uint8_t *d, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            d[y * w + x] = (uint8_t)((x * 7 + y * 13) & 0xFF);
}

/* --- generators, all bottom-up (the common BMP layout) --- */

static int gen_bmp24(const char *path, int w, int h)
{
    long row = ((long)w * 3 + 3) / 4 * 4;
    long pix = row * h;
    uint8_t *px = (uint8_t *)calloc(1, (size_t)pix);
    uint8_t hdr[54] = {'B','M'};
    uint32_t fsz = 54 + (uint32_t)pix;
    hdr[2] = (uint8_t)fsz; hdr[3] = fsz >> 8; hdr[4] = fsz >> 16; hdr[5] = fsz >> 24;
    hdr[10] = 54;
    uint32_t hsz = 40; memcpy(hdr + 14, &hsz, 4);
    int32_t w32 = w, h32 = h; memcpy(hdr + 18, &w32, 4); memcpy(hdr + 22, &h32, 4);
    uint16_t planes = 1, bpp = 24; memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *d = px + (size_t)(h - 1 - y) * row + (size_t)x * 3;
            d[0] = (uint8_t)((x ^ y) & 0xFF);
            d[1] = (uint8_t)(y * 255 / (h > 1 ? h - 1 : 1));
            d[2] = (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1));
        }

    FILE *f = img_fopen_write(path);
    int ok = f && fwrite(hdr, 1, 54, f) == 54 && fwrite(px, 1, (size_t)pix, f) == (size_t)pix;
    if (f) fclose(f);
    free(px);
    return ok ? 0 : -1;
}

static int gen_bmp32_alpha(const char *path, int w, int h)
{
    /* BITMAPV4HEADER with alpha mask so the alpha channel is real */
    uint8_t hdr[14 + 108];
    memset(hdr, 0, sizeof(hdr));
    long row = (long)w * 4;
    long pix = row * h;
    hdr[0] = 'B'; hdr[1] = 'M';
    uint32_t fsz = (uint32_t)(14 + 108 + pix);
    hdr[2] = (uint8_t)fsz; hdr[3] = fsz >> 8; hdr[4] = fsz >> 16; hdr[5] = fsz >> 24;
    hdr[10] = 14 + 108;
    uint32_t hsz = 108; memcpy(hdr + 14, &hsz, 4);
    int32_t w32 = w, h32 = h; memcpy(hdr + 18, &w32, 4); memcpy(hdr + 22, &h32, 4);
    uint16_t planes = 1, bpp = 32; memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    uint32_t comp = 3 /* BI_BITFIELDS */; memcpy(hdr + 30, &comp, 4);
    uint32_t rm = 0x00FF0000, gm = 0x0000FF00, bm = 0x000000FF, am = 0xFF000000;
    memcpy(hdr + 54, &rm, 4); memcpy(hdr + 58, &gm, 4);
    memcpy(hdr + 62, &bm, 4); memcpy(hdr + 66, &am, 4);

    uint8_t *px = (uint8_t *)malloc((size_t)pix);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *d = px + (size_t)(h - 1 - y) * row + (size_t)x * 4;
            d[0] = (uint8_t)((x ^ y) & 0xFF);            /* B */
            d[1] = (uint8_t)(y * 255 / (h > 1 ? h - 1 : 1));  /* G */
            d[2] = (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1));  /* R */
            d[3] = (uint8_t)((x < w / 2) ? 0 : 255);     /* A */
        }

    FILE *f = img_fopen_write(path);
    int ok = f && fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
             fwrite(px, 1, (size_t)pix, f) == (size_t)pix;
    if (f) fclose(f);
    free(px);
    return ok ? 0 : -1;
}

static int gen_bmp8_pal(const char *path, int w, int h, int npal)
{
    long row = ((long)w + 3) / 4 * 4;
    long pix = row * h;
    uint8_t hdr[14 + 40 + 256 * 4];
    memset(hdr, 0, sizeof(hdr));
    uint32_t fsz = (uint32_t)(14 + 40 + npal * 4 + pix);
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (uint8_t)fsz; hdr[3] = fsz >> 8; hdr[4] = fsz >> 16; hdr[5] = fsz >> 24;
    uint32_t off = 14 + 40 + (uint32_t)npal * 4;
    hdr[10] = (uint8_t)off; hdr[11] = off >> 8; hdr[12] = off >> 16; hdr[13] = off >> 24;
    uint32_t hsz = 40; memcpy(hdr + 14, &hsz, 4);
    int32_t w32 = w, h32 = h; memcpy(hdr + 18, &w32, 4); memcpy(hdr + 22, &h32, 4);
    uint16_t planes = 1, bpp = 8; memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    uint32_t cu = (uint32_t)npal; memcpy(hdr + 46, &cu, 4);
    for (int i = 0; i < npal; i++) {
        uint8_t *e = hdr + 54 + i * 4;
        e[0] = (uint8_t)(i * 255 / (npal > 1 ? npal - 1 : 1));
        e[1] = (uint8_t)(255 - i * 255 / (npal > 1 ? npal - 1 : 1));
        e[2] = (uint8_t)(i * 128 / (npal > 1 ? npal - 1 : 1) + 60);
        e[3] = 0;
    }

    uint8_t *px = (uint8_t *)malloc((size_t)pix);
    memset(px, 0, (size_t)pix);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            px[(size_t)(h - 1 - y) * row + x] = (uint8_t)((x + y) % npal);

    FILE *f = img_fopen_write(path);
    int ok = f && fwrite(hdr, 1, 14 + 40 + (size_t)npal * 4, f) == 14 + 40 + (size_t)npal * 4 &&
             fwrite(px, 1, (size_t)pix, f) == (size_t)pix;
    if (f) fclose(f);
    free(px);
    return ok ? 0 : -1;
}

static int gen_tga32(const char *path, int w, int h, int top_down)
{
    uint8_t hdr[18] = {0};
    hdr[2] = 2;     /* uncompressed truecolor */
    hdr[12] = (uint8_t)(w & 0xFF); hdr[13] = (uint8_t)(w >> 8);
    hdr[14] = (uint8_t)(h & 0xFF); hdr[15] = (uint8_t)(h >> 8);
    hdr[16] = 32;
    hdr[17] = (uint8_t)(top_down ? 0x28 : 0x08);   /* alpha 8 bits; bit5 = top-down */

    FILE *f = img_fopen_write(path);
    int ok = f && fwrite(hdr, 1, 18, f) == 18;
    for (int y = 0; y < h && ok; y++) {
        int iy = top_down ? y : h - 1 - y;   /* image row this file row holds */
        for (int x = 0; x < w && ok; x++) {
            uint8_t px[4] = { (uint8_t)((x ^ iy) & 0xFF),               /* B */
                              (uint8_t)(iy * 255 / (h > 1 ? h - 1 : 1)),/* G */
                              (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1)), /* R */
                              (uint8_t)((x + iy) & 1 ? 128 : 255) };    /* A */
            ok = fwrite(px, 1, 4, f) == 4;
        }
    }
    if (f) fclose(f);
    return ok ? 0 : -1;
}

static int gen_pnm(const char *path, int w, int h, int type, unsigned maxval)
{
    FILE *f = img_fopen_write(path);
    if (!f)
        return -1;
    int ok = 0;
    if (type == 6) {
        ok = fprintf(f, "P6\n%d %d\n%u\n", w, h, maxval) > 0;
        int wide = maxval > 255;
        for (int y = 0; y < h && ok; y++)
            for (int x = 0; x < w && ok; x++) {
                uint8_t rgb[3] = { (uint8_t)(x * 255 / (w - 1)),
                                   (uint8_t)(y * 255 / (h - 1)),
                                   (uint8_t)((x ^ y) & 0xFF) };
                for (int c = 0; c < 3; c++) {
                    unsigned s = (unsigned)rgb[c] * maxval / 255u;
                    if (wide) {
                        uint8_t b2[2] = { (uint8_t)(s >> 8), (uint8_t)s };
                        ok = fwrite(b2, 1, 2, f) == 2;
                    } else {
                        uint8_t b1 = (uint8_t)s;
                        ok = fwrite(&b1, 1, 1, f) == 1;
                    }
                }
            }
    } else if (type == 5) {
        ok = fprintf(f, "P5\n%d %d\n%u\n", w, h, maxval) > 0;
        int wide = maxval > 255;
        for (int y = 0; y < h && ok; y++)
            for (int x = 0; x < w && ok; x++) {
                unsigned g = (unsigned)((x * 7 + y * 13) & 0xFF) * maxval / 255u;
                if (wide) {
                    uint8_t b2[2] = { (uint8_t)(g >> 8), (uint8_t)g };
                    ok = fwrite(b2, 1, 2, f) == 2;
                } else {
                    uint8_t b1 = (uint8_t)g;
                    ok = fwrite(&b1, 1, 1, f) == 1;
                }
            }
    }
    fclose(f);
    return ok ? 0 : -1;
}

/* generate a small JPEG with libjpeg (17x13 RGB gradient) */
static int gen_jpeg(const char *path, int w, int h)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    FILE *f = img_fopen_write(path);
    if (!f)
        return -1;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, f);
    cinfo.image_width = (JDIMENSION)w;
    cinfo.image_height = (JDIMENSION)h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 85, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    uint8_t *row = (uint8_t *)malloc((size_t)w * 3);
    while (cinfo.next_scanline < cinfo.image_height) {
        int y = (int)cinfo.next_scanline;
        for (int x = 0; x < w; x++) {
            row[(size_t)x * 3 + 0] = (uint8_t)(x * 255 / (w - 1));
            row[(size_t)x * 3 + 1] = (uint8_t)(y * 255 / (h - 1));
            row[(size_t)x * 3 + 2] = (uint8_t)((x ^ y) & 0xFF);
        }
        JSAMPROW rp = row;
        jpeg_write_scanlines(&cinfo, &rp, 1);
    }
    free(row);
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(f);
    return 0;
}

/* generate a 16-bit gray PNG carrying a tRNS transparent value */
static int gen_png16_gray_trns(const char *path, int w, int h, unsigned trns_value)
{
    FILE *f = img_fopen_write(path);
    if (!f)
        return -1;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info) {
        if (png) png_destroy_write_struct(&png, NULL);
        fclose(f);
        return -1;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(f);
        return -1;
    }
    png_init_io(png, f);
    png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 16,
                 PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_color_16 t;
    memset(&t, 0, sizeof(t));
    t.gray = (png_uint_16)trns_value;
    png_set_tRNS(png, info, NULL, 0, &t);
    png_write_info(png, info);

    uint8_t *row = (uint8_t *)malloc((size_t)w * 2);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned v = (x == 1 && y == 0) ? trns_value
                                            : (unsigned)((x * 4096 + y * 1000) & 0xFFFF);
            img_st16be(row + (size_t)x * 2, v);
        }
        png_write_row(png, row);
    }
    free(row);
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(f);
    return 0;
}

/* Build a minimal big-endian TIFF/Exif blob (Make + resolution tags). */
static void put_be16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, unsigned v)
{ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static size_t make_exif_blob(uint8_t *out, size_t cap)
{
    enum { BLOB = 84 };
    if (cap < BLOB)
        return 0;
    memset(out, 0, BLOB);
    memcpy(out, "MM", 2);
    put_be16(out + 2, 42);          /* big-endian TIFF magic */
    put_be32(out + 4, 8);           /* IFD0 offset */
    put_be16(out + 8, 4);           /* entry count */
    uint8_t *e = out + 10;          /* entries: tag(2) type(2) count(4) value(4) */
    put_be16(e +  0, 0x010F); put_be16(e +  2, 2); put_be32(e +  4, 5); put_be32(e +  8, 62); /* Make */
    put_be16(e + 12, 0x011A); put_be16(e + 14, 5); put_be32(e + 16, 1); put_be32(e + 20, 68); /* XRes */
    put_be16(e + 24, 0x011B); put_be16(e + 26, 5); put_be32(e + 28, 1); put_be32(e + 32, 76); /* YRes */
    put_be16(e + 36, 0x0128); put_be16(e + 38, 3); put_be32(e + 40, 1); put_be16(e + 44, 2);  /* unit=inch */
    /* next IFD offset stays 0 */
    memcpy(out + 62, "ACME", 4);    /* Make value */
    put_be32(out + 68, 72); put_be32(out + 72, 1);   /* XResolution = 72/1 */
    put_be32(out + 76, 72); put_be32(out + 80, 1);   /* YResolution = 72/1 */
    return BLOB;
}

/* Insert an APP1 Exif segment right after the SOI marker of a JPEG file. */
static int jpeg_inject_exif(const char *path, const uint8_t *tiff, size_t tlen)
{
    FILE *f = img_fopen_read(path);
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *d = (uint8_t *)malloc((size_t)n);
    int ok = d && fread(d, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok || n < 2 || d[0] != 0xFF || d[1] != 0xD8) {
        free(d);
        return -1;
    }
    size_t seg = 2 + 6 + tlen;                 /* length field + "Exif\0\0" + TIFF */
    FILE *o = img_fopen_write(path);
    int rc = -1;
    if (o) {
        uint8_t m[4] = {0xFF, 0xE1, (uint8_t)(seg >> 8), (uint8_t)seg};
        rc = (fwrite(d, 1, 2, o) == 2 &&
              fwrite(m, 1, 4, o) == 4 &&
              fwrite("Exif\0\0", 1, 6, o) == 6 &&
              fwrite(tiff, 1, tlen, o) == tlen &&
              fwrite(d + 2, 1, (size_t)n - 2, o) == (size_t)n - 2) ? 0 : -1;
        fclose(o);
    }
    free(d);
    return rc;
}

/* Write a small JXL whose color encoding is linear sRGB: unlike plain sRGB
 * this makes libjxl store an ICC profile, which we can read back and reuse
 * as a genuine, valid ICC payload for the metadata tests. */
static int gen_linear_jxl(const char *path)
{
    enum { LW = 4, LH = 2 };
    uint8_t px[LW * LH * 3];
    for (int i = 0; i < LW * LH * 3; i++)
        px[i] = (uint8_t)(i * 7);
    JxlEncoder *enc = JxlEncoderCreate(NULL);
    JxlEncoderFrameSettings *fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_EFFORT, 3);
    JxlEncoderSetFrameLossless(fs, JXL_TRUE);
    JxlBasicInfo bi;
    JxlEncoderInitBasicInfo(&bi);
    bi.xsize = LW;
    bi.ysize = LH;
    bi.bits_per_sample = 8;
    bi.num_color_channels = 3;
    bi.uses_original_profile = JXL_TRUE;
    int ok = JxlEncoderSetBasicInfo(enc, &bi) == JXL_ENC_SUCCESS;
    JxlColorEncoding ce;
    JxlColorEncodingSetToLinearSRGB(&ce, JXL_FALSE);
    ok = ok && JxlEncoderSetColorEncoding(enc, &ce) == JXL_ENC_SUCCESS;
    JxlPixelFormat pf;
    memset(&pf, 0, sizeof(pf));
    pf.num_channels = 3;
    pf.data_type = JXL_TYPE_UINT8;
    pf.endianness = JXL_NATIVE_ENDIAN;
    ok = ok && JxlEncoderAddImageFrame(fs, &pf, px, sizeof(px)) == JXL_ENC_SUCCESS;
    JxlEncoderCloseInput(enc);
    uint8_t *out = (uint8_t *)malloc(1 << 16);
    size_t cap = 1 << 16;
    uint8_t *np = out;
    while (ok) {
        JxlEncoderStatus st = JxlEncoderProcessOutput(enc, &np, &cap);
        if (st == JXL_ENC_SUCCESS)
            break;
        if (st != JXL_ENC_NEED_MORE_OUTPUT) { ok = 0; break; }
    }
    if (ok) {
        FILE *f = img_fopen_write(path);
        ok = f && fwrite(out, 1, (size_t)(np - out), f) == (size_t)(np - out);
        if (f)
            fclose(f);
    }
    free(out);
    JxlEncoderDestroy(enc);
    return ok ? 0 : -1;
}

/* A PNG carrying pHYs (72 dpi), an ICC profile and two text chunks. */
static int gen_png_meta(const char *path, const uint8_t *icc, size_t icc_len)
{
    FILE *f = img_fopen_write(path);
    if (!f)
        return -1;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info) {
        if (png) png_destroy_write_struct(&png, NULL);
        fclose(f);
        return -1;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(f);
        return -1;
    }
    png_init_io(png, f);
    png_set_IHDR(png, info, 8, 6, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_pHYs(png, info, 2835, 2835, PNG_RESOLUTION_METER);  /* 2835 px/m ~ 72 dpi */
    if (icc && icc_len)
        png_set_iCCP(png, info, "selftest", PNG_COMPRESSION_TYPE_BASE,
                     (png_const_bytep)icc, (png_uint_32)icc_len);
    png_text txt[2];
    memset(txt, 0, sizeof(txt));
    txt[0].compression = PNG_TEXT_COMPRESSION_NONE;
    txt[0].key = (png_charp)"Title";
    txt[0].text = (png_charp)"Metadata test";
    txt[1].compression = PNG_TEXT_COMPRESSION_NONE;
    txt[1].key = (png_charp)"Software";
    txt[1].text = (png_charp)"img2jxl selftest";
    png_set_text(png, info, txt, 2);
    png_write_info(png, info);
    uint8_t row[8 * 3];
    for (int y = 0; y < 6; y++) {
        for (int x = 0; x < 8; x++) {
            row[x * 3 + 0] = (uint8_t)(x * 30);
            row[x * 3 + 1] = (uint8_t)(y * 40);
            row[x * 3 + 2] = 128;
        }
        png_write_row(png, row);
    }
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(f);
    return 0;
}

/* Read a whole file; returns a malloc'ed buffer (caller frees) or NULL. */
static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = img_fopen_read(path);
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *b = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
    int ok = b && fread(b, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) {
        free(b);
        return NULL;
    }
    *len = (size_t)n;
    return b;
}

/* Parse a big-endian Exif IFD0 for the physical resolution. */
static int exif_resolution(const uint8_t *e, size_t len, int *unit, double *xres)
{
    if (len < 14 || e[0] != 'M' || e[1] != 'M')
        return -1;
    size_t ifd = ((size_t)e[4] << 24) | ((size_t)e[5] << 16) |
                 ((size_t)e[6] << 8) | e[7];
    if (ifd + 2 > len)
        return -1;
    int n = (e[ifd] << 8) | e[ifd + 1];
    *unit = 0;
    *xres = 0.0;
    for (int i = 0; i < n; i++) {
        size_t p = ifd + 2 + (size_t)i * 12;
        if (p + 12 > len)
            break;
        unsigned tag = ((unsigned)e[p] << 8) | e[p + 1];
        unsigned type = ((unsigned)e[p + 2] << 8) | e[p + 3];
        if (tag == 0x011A && type == 5) {
            size_t o = ((size_t)e[p + 8] << 24) | ((size_t)e[p + 9] << 16) |
                       ((size_t)e[p + 10] << 8) | e[p + 11];
            if (o + 8 <= len) {
                unsigned num = ((unsigned)e[o] << 24) | ((unsigned)e[o + 1] << 16) |
                               ((unsigned)e[o + 2] << 8) | e[o + 3];
                unsigned den = ((unsigned)e[o + 4] << 24) | ((unsigned)e[o + 5] << 16) |
                               ((unsigned)e[o + 6] << 8) | e[o + 7];
                if (den)
                    *xres = (double)num / (double)den;
            }
        } else if (tag == 0x0128 && type == 3) {
            *unit = ((int)e[p + 8] << 8) | e[p + 9];
        }
    }
    return 0;
}

/* --- verification helpers --- */

/* decode a .jxl file with the same decoder the CLI uses for .jxl inputs */
static int read_jxl_img(const char *path, img_image_t *img)
{
    FILE *f = img_fopen_read(path);
    if (!f)
        return -1;
    int is_anim = 0;
    char err[256];
    int rc = jxl_decode_file(f, img, &is_anim, err, sizeof(err));
    fclose(f);
    if (rc != 0)
        return -1;
    return img_channels(img->color);
}

/* Build the pixel form the encoder produces for *img (palette expanded to
 * RGB(A), sub-byte gray scaled to 8-bit, 16-bit rows kept big-endian) so
 * decoded .jxl pixels can be compared directly. */
static uint8_t *expected_pixels(const img_image_t *img, int *ch, int *is16)
{
    int w = img->width, h = img->height;
    *is16 = (img->bit_depth == 16);
    size_t n = (size_t)w * h;

    if (img->color == IMG_PALETTE) {
        int c = img->has_pal_alpha ? 4 : 3;
        *ch = c; *is16 = 0;
        uint8_t *buf = (uint8_t *)malloc(n * (size_t)c);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int idx = img->data[(size_t)y * img->rowstride + x];
                if (idx >= img->pal_ncolors)
                    idx = 0;
                buf[((size_t)y * w + x) * c + 0] = img->palette[idx * 3 + 0];
                buf[((size_t)y * w + x) * c + 1] = img->palette[idx * 3 + 1];
                buf[((size_t)y * w + x) * c + 2] = img->palette[idx * 3 + 2];
                if (c == 4)
                    buf[((size_t)y * w + x) * c + 3] = img->has_pal_alpha ? img->pal_alpha[idx] : 255;
            }
        return buf;
    }
    if (img->color == IMG_GRAY && img->bit_depth <= 4) {
        *ch = 1; *is16 = 0;
        uint8_t *buf = (uint8_t *)malloc(n);
        unsigned mult = 255u / ((1u << img->bit_depth) - 1);
        for (int y = 0; y < h; y++) {
            const uint8_t *row = img->data + (size_t)y * img->rowstride;
            for (int x = 0; x < w; x++) {
                int per = 8 / img->bit_depth;
                unsigned byte = row[x / per];
                int shift = 8 - img->bit_depth - (x % per) * img->bit_depth;
                buf[(size_t)y * w + x] = (uint8_t)(((byte >> shift) & ((1u << img->bit_depth) - 1)) * mult);
            }
        }
        return buf;
    }
    /* GRAY / GRAY_ALPHA / RGB / RGBA at native depth: encoder copies rows */
    *ch = img_channels(img->color);
    size_t step = *is16 ? 2 : 1;
    uint8_t *buf = (uint8_t *)malloc(n * (size_t)*ch * step);
    for (int y = 0; y < h; y++)
        memcpy(buf + (size_t)y * w * *ch * step,
               img->data + (size_t)y * img->rowstride,
               (size_t)w * *ch * step);
    return buf;
}

static long compare_buf(const uint8_t *a, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return (long)i;
    return -1;
}

/* full round-trip check: encode *img to path, decode back, compare pixels.
 * Returns the number of channels of the encoded form, or -1. */
static int roundtrip_jxl(const img_image_t *img, const jxl_opts_t *opts,
                         const char *path, char *err, size_t errlen)
{
    if (jxl_write_file(img, opts, path, err, errlen) != 0)
        return -1;
    img_image_t got;
    int ch = read_jxl_img(path, &got);
    if (ch < 0) {
        snprintf(err, errlen, "encoded jxl does not decode");
        return -1;
    }
    int ech, eis16;
    uint8_t *exp = expected_pixels(img, &ech, &eis16);
    size_t step = eis16 ? 2 : 1;
    size_t n = (size_t)img->width * img->height * (size_t)ech * step;
    long diff = compare_buf(got.data, exp, n);
    int gch = img_channels(got.color);
    int gdepth = got.bit_depth;
    img_free(&got);
    free(exp);
    if (diff >= 0 || gch != ech || gdepth != (eis16 ? 16 : 8)) {
        snprintf(err, errlen, "pixel mismatch at %ld (got %d ch %d-bit)", diff, gch, gdepth);
        return -1;
    }
    return ch;
}

static uint8_t pal_gif_r(int i) { static const uint8_t v[4] = {255,0,0,10};  return v[i]; }
static uint8_t pal_gif_g(int i) { static const uint8_t v[4] = {0,255,0,20};  return v[i]; }
static uint8_t pal_gif_b(int i) { static const uint8_t v[4] = {0,0,255,30};  return v[i]; }

static uint64_t file_time_stamp(const char *path, int create)
{
    uint8_t c[16] = {0}, m[16] = {0};
    if (get_file_times(path, c, m) != 0)
        return 0;
    if (create) {
        uint64_t v;
        memcpy(&v, c, 8);
        return v;
    }
    uint64_t v;
    memcpy(&v, m, 8);
    return v;
}

/* decode an animated .jxl into coalesced RGBA8 canvas frames */
typedef struct {
    int w, h;
    int nframes;
    uint32_t *durations;
    uint8_t **frames;       /* nframes canvas-sized RGBA buffers */
    uint32_t num_loops;
} jxl_anim_t;

static void free_jxl_anim(jxl_anim_t *a)
{
    for (int i = 0; i < a->nframes; i++)
        free(a->frames[i]);
    free(a->frames);
    free(a->durations);
    memset(a, 0, sizeof(*a));
}

static int read_jxl_anim(const char *path, jxl_anim_t *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof(*out));
    FILE *f = img_fopen_read(path);
    if (!f) {
        snprintf(err, errlen, "cannot open %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)fsize);
    if (!buf || fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(buf);
        fclose(f);
        snprintf(err, errlen, "cannot read %s", path);
        return -1;
    }
    fclose(f);

    JxlDecoder *dec = JxlDecoderCreate(NULL);
    JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE);
    JxlDecoderSetInput(dec, buf, (size_t)fsize);

    int rc = -1, cap = 0;
    size_t canvas_bytes = 0;
    uint8_t *scratch = NULL;

    for (;;) {
        JxlDecoderStatus st = JxlDecoderProcessInput(dec);
        if (st == JXL_DEC_BASIC_INFO) {
            JxlBasicInfo info;
            JxlDecoderGetBasicInfo(dec, &info);
            out->w = (int)info.xsize;
            out->h = (int)info.ysize;
            out->num_loops = info.animation.num_loops;
            canvas_bytes = (size_t)out->w * out->h * 4;
            scratch = (uint8_t *)malloc(canvas_bytes);
            if (!scratch) {
                snprintf(err, errlen, "out of memory");
                break;
            }
            JxlPixelFormat pf;
            memset(&pf, 0, sizeof(pf));
            pf.num_channels = 4;
            pf.data_type = JXL_TYPE_UINT8;
            pf.endianness = JXL_NATIVE_ENDIAN;
            if (JxlDecoderSetImageOutBuffer(dec, &pf, scratch, canvas_bytes) != JXL_DEC_SUCCESS) {
                snprintf(err, errlen, "cannot set image out buffer");
                break;
            }
            continue;
        }
        if (st == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            /* the (re)set buffer request happens per coalesced frame */
            JxlPixelFormat pf;
            memset(&pf, 0, sizeof(pf));
            pf.num_channels = 4;
            pf.data_type = JXL_TYPE_UINT8;
            pf.endianness = JXL_NATIVE_ENDIAN;
            if (JxlDecoderSetImageOutBuffer(dec, &pf, scratch, canvas_bytes)
                != JXL_DEC_SUCCESS) {
                snprintf(err, errlen, "cannot set image out buffer");
                break;
            }
            continue;
        }
        if (st == JXL_DEC_FRAME) {
            JxlFrameHeader fh;
            JxlDecoderGetFrameHeader(dec, &fh);
            if (out->nframes == cap) {
                cap = cap ? cap * 2 : 8;
                out->durations = (uint32_t *)realloc(out->durations, (size_t)cap * sizeof(uint32_t));
                out->frames = (uint8_t **)realloc(out->frames, (size_t)cap * sizeof(uint8_t *));
            }
            out->durations[out->nframes] = fh.duration;
            out->frames[out->nframes] = (uint8_t *)malloc(canvas_bytes);
            out->nframes++;
            continue;
        }
        if (st == JXL_DEC_FULL_IMAGE) {
            memcpy(out->frames[out->nframes - 1], scratch, canvas_bytes);
            continue;
        }
        if (st == JXL_DEC_SUCCESS) {
            rc = 0;
            break;
        }
        snprintf(err, errlen, "jxl animation decode failed (status %d)", st);
        break;
    }

    free(scratch);
    JxlDecoderDestroy(dec);
    free(buf);
    if (rc != 0)
        free_jxl_anim(out);
    return rc;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* usage: img2jxl_selftest [file.jxl] — just verify the file decodes */
    if (argc > 1) {
        img_image_t img;
        int is_anim = 0;
        char err[256] = {0};
        FILE *f = img_fopen_read(argv[1]);
        int rc = f ? jxl_decode_file(f, &img, &is_anim, err, sizeof(err)) : -1;
        if (f) fclose(f);
        if (rc != 0) {
            printf("FAIL: cannot decode %s (%s)\n", argv[1], err);
            return 1;
        }
        printf("OK: %s decodes (%dx%d, %s %d-bit)\n", argv[1], img.width, img.height,
               img2jxl_color_name(img.color), img_nominal_depth(&img));
        img_free(&img);
        return 0;
    }

    printf("img2jxl self-test\n");
#ifdef _WIN32
    _mkdir("testout");
#else
    mkdir("testout", 0755);
#endif

    jxl_opts_t opts;
    opts.effort = 10;
    opts.modular = 1;
    opts.auto_optimize = 0;
    opts.keep_metadata = 1;

    int W = 33, H = 17;     /* odd sizes to exercise padding paths */
    char err[256];

    /* --- BMP 24-bit --- */
    printf("BMP 24-bit -> RGB8:\n");
    CHECK(gen_bmp24("testout/t24.bmp", W, H) == 0, "generate bmp24");
    {
        FILE *f = img_fopen_read("testout/t24.bmp");
        img_image_t img;
        int ok24 = f && bmp_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(ok24, "decode bmp24");
        if (f) fclose(f);
        if (!ok24) { printf("  SKIP  remaining bmp24 checks\n"); goto bmp24_done; }
        CHECK(img.color == IMG_RGB && img.bit_depth == 8, "format is RGB8");
        int ch = roundtrip_jxl(&img, &opts, "testout/t24.jxl", err, sizeof(err));
        CHECK(ch == 3, "jxl round-trip pixels identical (lossless)");
        img_free(&img);
bmp24_done:;
    }

    /* --- BMP 32-bit with alpha --- */
    printf("BMP 32-bit (V4, alpha mask) -> RGBA8:\n");
    CHECK(gen_bmp32_alpha("testout/t32.bmp", W, H) == 0, "generate bmp32");
    {
        FILE *f = img_fopen_read("testout/t32.bmp");
        img_image_t img;
        int ok32 = f && bmp_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(ok32, "decode bmp32");
        if (f) fclose(f);
        if (!ok32) { printf("  SKIP  remaining bmp32 checks\n"); goto bmp32_done; }
        CHECK(img.color == IMG_RGBA && img.bit_depth == 8, "format is RGBA8");
        int ch = roundtrip_jxl(&img, &opts, "testout/t32.jxl", err, sizeof(err));
        CHECK(ch == 4, "jxl round-trip pixels identical (lossless, incl. alpha)");
        img_free(&img);
bmp32_done:;
    }

    /* --- BMP 8-bit palette --- */
    printf("BMP 8-bit palette -> PALETTE8 (encoded expanded, modular re-palettizes):\n");
    CHECK(gen_bmp8_pal("testout/t8.bmp", W, H, 200) == 0, "generate bmp8");
    {
        FILE *f = img_fopen_read("testout/t8.bmp");
        img_image_t img;
        int ok8 = f && bmp_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(ok8, "decode bmp8");
        if (f) fclose(f);
        if (!ok8) { printf("  SKIP  remaining bmp8 checks\n"); goto bmp8_done; }
        CHECK(img.color == IMG_PALETTE && img.bit_depth == 8 && img.pal_ncolors == 200,
              "format is PALETTE8, 200 colors");
        int ch = roundtrip_jxl(&img, &opts, "testout/t8.jxl", err, sizeof(err));
        CHECK(ch == 3, "jxl round-trip: palette expanded to RGB, pixels identical");
        img_free(&img);
bmp8_done:;
    }

    /* --- TGA 32 top-down and bottom-up --- */
    printf("TGA 32-bit (both origins) -> RGBA8:\n");
    for (int td = 0; td < 2; td++) {
        char inp[64], outp[64];
        snprintf(inp, sizeof(inp), "testout/t32_%s.tga", td ? "td" : "bu");
        snprintf(outp, sizeof(outp), "testout/t32_%s.jxl", td ? "td" : "bu");
        CHECK(gen_tga32(inp, W, H, td) == 0, td ? "generate tga top-down" : "generate tga bottom-up");
        FILE *f = img_fopen_read(inp);
        img_image_t img;
        int oktga = f && tga_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(oktga, "decode tga");
        if (f) fclose(f);
        if (!oktga) { printf("  SKIP  remaining tga checks\n"); continue; }
        CHECK(img.color == IMG_RGBA, "format is RGBA8");
        int ch = roundtrip_jxl(&img, &opts, outp, err, sizeof(err));
        CHECK(ch == 4, td ? "jxl round-trip identical (top-down)" : "jxl round-trip identical (bottom-up)");
        img_free(&img);
    }

    /* --- PNM 16-bit --- */
    printf("PNM P6 maxval 1023 -> RGB16:\n");
    CHECK(gen_pnm("testout/t16.ppm", 9, 7, 6, 1023) == 0, "generate ppm16");
    {
        FILE *f = img_fopen_read("testout/t16.ppm");
        img_image_t img;
        int ok16 = f && pnm_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(ok16, "decode ppm16");
        if (f) fclose(f);
        if (!ok16) { printf("  SKIP  remaining ppm16 checks\n"); goto ppm_done; }
        CHECK(img.color == IMG_RGB && img.bit_depth == 16, "format is RGB16");
        int ch = roundtrip_jxl(&img, &opts, "testout/t16.jxl", err, sizeof(err));
        CHECK(ch == 3, "jxl round-trip: 16-bit values identical");
        img_free(&img);
ppm_done:;
    }

    /* --- PNM P5 8-bit gray --- */
    printf("PNM P5 gray -> GRAY8:\n");
    CHECK(gen_pnm("testout/tgray.pgm", 11, 5, 5, 255) == 0, "generate pgm8");
    {
        FILE *f = img_fopen_read("testout/tgray.pgm");
        img_image_t img;
        int okgray = f && pnm_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(okgray, "decode pgm8");
        if (f) fclose(f);
        if (!okgray) { printf("  SKIP  remaining pgm checks\n"); goto pgm_done; }
        CHECK(img.color == IMG_GRAY && img.bit_depth == 8, "format is GRAY8");
        int ch = roundtrip_jxl(&img, &opts, "testout/tgray.jxl", err, sizeof(err));
        CHECK(ch == 1, "jxl round-trip: gray pixels identical");
        img_free(&img);
pgm_done:;
    }

    /* --- JPEG bitstream transcoding (reconstruction must be byte-exact) --- */
    printf("JPEG -> JXL bitstream transcoding:\n");
    {
        CHECK(gen_jpeg("testout/t.jpg", 17, 13) == 0, "generate jpeg (libjpeg)");
        FILE *f = img_fopen_read("testout/t.jpg");
        uint8_t *jbuf = NULL;
        size_t jlen = 0;
        if (f) {
            fseek(f, 0, SEEK_END);
            jlen = (size_t)ftell(f);
            rewind(f);
            jbuf = (uint8_t *)malloc(jlen);
            if (fread(jbuf, 1, jlen, f) != jlen) { free(jbuf); jbuf = NULL; }
            fclose(f);
        }
        CHECK(jbuf != NULL, "read generated jpeg");

        int jw, jh, jgray;
        CHECK(jbuf && jpeg_parse_dims(jbuf, jlen, &jw, &jh, &jgray) == 0 &&
              jw == 17 && jh == 13 && !jgray, "jpeg_parse_dims (17x13 RGB)");

        jxl_opts_t topts = opts;
        int trc = jbuf ? jxl_transcode_jpeg(jbuf, jlen, &topts, "testout/t.jpg.jxl",
                                            err, sizeof(err)) : -1;
        CHECK(trc == 0, "jxl_transcode_jpeg");
        if (trc == 0) {
            FILE *jf = img_fopen_read("testout/t.jpg.jxl");
            uint8_t *rec = NULL;
            size_t reclen = 0;
            int rrc = jf ? jxl_reconstruct_jpeg(jf, &rec, &reclen, err, sizeof(err)) : -1;
            if (jf) fclose(jf);
            CHECK(rrc == 0, "jxl_reconstruct_jpeg");
            if (rrc == 0)
                printf("        original %zu bytes, reconstructed %zu bytes\n", jlen, reclen);
            CHECK(rrc == 0 && reclen == jlen && compare_buf(rec, jbuf, jlen) < 0,
                  "reconstructed JPEG is byte-identical to the original");
            free(rec);
        }

        /* pixel fallback path: decode -> re-encode still image */
        f = img_fopen_read("testout/t.jpg");
        img_image_t img;
        int pok = f && jpeg_decode(f, &img, err, sizeof(err)) == 0;
        if (f) fclose(f);
        CHECK(pok, "jpeg pixel decode");
        if (pok) {
            int ch = roundtrip_jxl(&img, &opts, "testout/tjpx.jxl", err, sizeof(err));
            CHECK(ch == 3, "jpeg pixel path jxl round-trip identical");
            img_free(&img);
        }
        free(jbuf);
    }

    /* --- JPEG Exif: metadata must be stored in a readable Exif box --- */
    printf("JPEG Exif metadata box:\n");
    {
        uint8_t tiff[84];
        size_t tlen = make_exif_blob(tiff, sizeof(tiff));
        CHECK(tlen == 84, "build Exif blob");
        CHECK(gen_jpeg("testout/exif.jpg", 17, 13) == 0, "generate jpeg for the Exif test");
        CHECK(jpeg_inject_exif("testout/exif.jpg", tiff, tlen) == 0, "inject Exif APP1");

        uint8_t *jb = NULL;
        size_t jl = 0;
        FILE *f = img_fopen_read("testout/exif.jpg");
        if (f) {
            fseek(f, 0, SEEK_END);
            jl = (size_t)ftell(f);
            rewind(f);
            jb = (uint8_t *)malloc(jl);
            if (fread(jb, 1, jl, f) != jl) { free(jb); jb = NULL; }
            fclose(f);
        }
        jxl_opts_t eo = opts;
        eo.effort = 3;
        CHECK(jb && jxl_transcode_jpeg(jb, jl, &eo, "testout/exif.jxl", err, sizeof(err)) == 0,
              "transcode jpeg carrying Exif");

        /* walk the JXL container boxes */
        uint8_t *jx = NULL;
        size_t jxlen = 0;
        FILE *xf = img_fopen_read("testout/exif.jxl");
        if (xf) {
            fseek(xf, 0, SEEK_END);
            jxlen = (size_t)ftell(xf);
            rewind(xf);
            jx = (uint8_t *)malloc(jxlen);
            if (fread(jx, 1, jxlen, xf) != jxlen) { free(jx); jx = NULL; }
            fclose(xf);
        }
        int exif_ok = 0, brob_seen = 0, jbrd_seen = 0;
        if (jx) {
            size_t off = 0;
            while (off + 8 <= jxlen) {
                uint32_t bsz = ((uint32_t)jx[off] << 24) | ((uint32_t)jx[off+1] << 16) |
                               ((uint32_t)jx[off+2] << 8) | jx[off+3];
                size_t hdr = 8;
                if (bsz == 1) {
                    if (off + 16 > jxlen) break;
                    bsz = 0;
                    for (int k = 0; k < 8; k++) bsz = (bsz << 8) | jx[off + 8 + k];
                    hdr = 16;
                } else if (bsz == 0) {
                    bsz = (uint32_t)(jxlen - off);
                }
                const uint8_t *type = jx + off + 4;
                if (!memcmp(type, "Exif", 4)) {
                    size_t clen = bsz - hdr;
                    exif_ok = (clen == tlen + 4) &&
                              !memcmp(jx + off + hdr, "\0\0\0\0", 4) &&
                              !memcmp(jx + off + hdr + 4, tiff, tlen);
                }
                if (!memcmp(type, "brob", 4)) brob_seen = 1;
                if (!memcmp(type, "jbrd", 4)) jbrd_seen = 1;
                if (bsz < hdr) break;
                off += bsz;
            }
        }
        CHECK(exif_ok, "plain Exif box present with the exact injected payload");
        CHECK(!brob_seen, "metadata is not brotli-wrapped in a brob box");
        CHECK(jbrd_seen, "jbrd reconstruction data still present");

        /* and the JPEG must still come back byte-for-byte */
        if (jb) {
            FILE *rf = img_fopen_read("testout/exif.jxl");
            uint8_t *rec = NULL;
            size_t reclen = 0;
            int rrc = rf ? jxl_reconstruct_jpeg(rf, &rec, &reclen, err, sizeof(err)) : -1;
            if (rf) fclose(rf);
            CHECK(rrc == 0 && reclen == jl && compare_buf(rec, jb, jl) < 0,
                  "Exif-bearing JPEG reconstructs byte-for-byte");
            free(rec);
        }
        free(jb);
        free(jx);
    }

    /* --- metadata carrying: DPI / ICC / Exif / text --- */
    printf("metadata carrying:\n");
    {
        /* a non-sRGB JXL gives us a genuine ICC profile to work with */
        CHECK(gen_linear_jxl("testout/linear.jxl") == 0, "generate linear-sRGB jxl");
        img_image_t lin;
        int chl = read_jxl_img("testout/linear.jxl", &lin);
        CHECK(chl == 3 && lin.meta.icc_len > 0, "non-sRGB jxl exposes its ICC profile");
        uint8_t icc_copy[4096];
        size_t icc_len = 0;
        if (chl == 3 && lin.meta.icc_len && lin.meta.icc_len <= sizeof(icc_copy)) {
            icc_len = lin.meta.icc_len;
            memcpy(icc_copy, lin.meta.icc, icc_len);
        }
        if (chl == 3)
            img_free(&lin);

        /* PNG with pHYs + ICC + text chunks */
        CHECK(icc_len > 0 && gen_png_meta("testout/meta.png", icc_copy, icc_len) == 0,
              "generate png with pHYs/ICC/text");
        size_t pblen = 0;
        uint8_t *pb = slurp("testout/meta.png", &pblen);
        img_image_t src;
        int okm = pb && png_decode_mem(pb, pblen, &src, err, sizeof(err)) == 0;
        free(pb);
        CHECK(okm, "decode png with metadata");
        if (okm) {
            CHECK(src.meta.xres > 28.0 && src.meta.xres < 28.5 && src.meta.res_unit == 3,
                  "pHYs read as 28.35 px/cm");
            CHECK(src.meta.icc_len == icc_len, "PNG iCCP read");
            CHECK(src.meta.ntexts == 2 &&
                  !strcmp(src.meta.texts[0].key, "Title") &&
                  !strcmp(src.meta.texts[0].value, "Metadata test") &&
                  !strcmp(src.meta.texts[1].key, "Software"),
                  "PNG text chunks read");

            jxl_opts_t mo = opts;
            mo.effort = 3;
            CHECK(jxl_write_file(&src, &mo, "testout/meta.jxl", err, sizeof(err)) == 0,
                  "encode png with metadata");
            img_free(&src);

            img_image_t back;
            int chb = read_jxl_img("testout/meta.jxl", &back);
            CHECK(chb == 3, "jxl carrying metadata decodes");
            if (chb == 3) {
                CHECK(back.meta.icc_len == icc_len &&
                      !memcmp(back.meta.icc, icc_copy, icc_len),
                      "ICC profile round-trips byte-exactly");
                CHECK(back.meta.ntexts == 2 &&
                      !strcmp(back.meta.texts[0].key, "Title") &&
                      !strcmp(back.meta.texts[0].value, "Metadata test") &&
                      !strcmp(back.meta.texts[1].key, "Software") &&
                      !strcmp(back.meta.texts[1].value, "img2jxl selftest"),
                      "text chunks round-trip");
                /* the resolution travelled in a synthesized Exif box */
                int unit = 0;
                double xres = 0.0;
                CHECK(exif_resolution(back.meta.exif, back.meta.exif_len,
                                      &unit, &xres) == 0 &&
                      unit == 3 && xres > 28.3 && xres < 28.4,
                      "Exif resolution box carries 72 dpi (28.35 px/cm)");
                img_free(&back);
            }

            /* keep_metadata = 0 must leave a bare file */
            size_t pblen2 = 0;
            uint8_t *pb2 = slurp("testout/meta.png", &pblen2);
            img_image_t src2;
            if (pb2 && png_decode_mem(pb2, pblen2, &src2, err, sizeof(err)) == 0) {
                jxl_opts_t nm = opts;
                nm.effort = 3;
                nm.keep_metadata = 0;
                CHECK(jxl_write_file(&src2, &nm, "testout/nometa.jxl", err, sizeof(err)) == 0,
                      "encode with metadata disabled");
                img_free(&src2);
                img_image_t b2;
                if (read_jxl_img("testout/nometa.jxl", &b2) == 3) {
                    CHECK(b2.meta.exif_len == 0 && b2.meta.icc_len == 0 &&
                          b2.meta.ntexts == 0,
                          "disabled metadata leaves a bare file");
                    img_free(&b2);
                }
            }
            free(pb2);
        }
    }

    /* --- metadata from TIFF (resolution + description) and GIF (comment) --- */
    printf("metadata from TIFF and GIF:\n");
    {
        TIFF *tf = TIFFOpen("testout/meta.tiff", "w");
        CHECK(tf != NULL, "open tiff for metadata test");
        if (tf) {
            TIFFSetField(tf, TIFFTAG_IMAGEWIDTH, 4);
            TIFFSetField(tf, TIFFTAG_IMAGELENGTH, 3);
            TIFFSetField(tf, TIFFTAG_BITSPERSAMPLE, 8);
            TIFFSetField(tf, TIFFTAG_SAMPLESPERPIXEL, 3);
            TIFFSetField(tf, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
            TIFFSetField(tf, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            TIFFSetField(tf, TIFFTAG_XRESOLUTION, 300.0f);
            TIFFSetField(tf, TIFFTAG_YRESOLUTION, 300.0f);
            TIFFSetField(tf, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
            TIFFSetField(tf, TIFFTAG_IMAGEDESCRIPTION, "TIFF metadata probe");
            uint8_t row[4 * 3];
            int wok = 1;
            for (int y = 0; y < 3 && wok; y++) {
                for (int x = 0; x < 4; x++) {
                    row[x * 3 + 0] = (uint8_t)(x * 50);
                    row[x * 3 + 1] = (uint8_t)(y * 70);
                    row[x * 3 + 2] = 200;
                }
                wok = TIFFWriteScanline(tf, row, (uint32_t)y, 0) >= 0;
            }
            CHECK(wok, "write tiff with metadata");
            TIFFClose(tf);
        }
        FILE *tfh = img_fopen_read("testout/meta.tiff");
        img_image_t ti;
        int okti = tfh && tiff_decode(tfh, &ti, err, sizeof(err)) == 0;
        if (tfh) fclose(tfh);
        CHECK(okti, "decode tiff with metadata");
        if (okti) {
            CHECK(ti.meta.res_unit == 2 && ti.meta.xres > 299.0 && ti.meta.xres < 301.0,
                  "TIFF resolution read (300 dpi)");
            CHECK(ti.meta.ntexts == 1 && !strcmp(ti.meta.texts[0].key, "Description") &&
                  !strcmp(ti.meta.texts[0].value, "TIFF metadata probe"),
                  "TIFF description read");
            jxl_opts_t to = opts;
            to.effort = 3;
            CHECK(jxl_write_file(&ti, &to, "testout/metatiff.jxl", err, sizeof(err)) == 0,
                  "encode tiff with metadata");
            img_free(&ti);
            img_image_t tb;
            if (read_jxl_img("testout/metatiff.jxl", &tb) == 3) {
                int unit = 0;
                double xres = 0.0;
                CHECK(exif_resolution(tb.meta.exif, tb.meta.exif_len, &unit, &xres) == 0 &&
                      unit == 2 && xres > 299.0 && xres < 301.0,
                      "300 dpi survives into the jxl Exif box");
                img_free(&tb);
            }
        }
    }
    {
        int ge = 0;
        GifFileType *gf = EGifOpenFileName("testout/meta.gif", 0, &ge);
        CHECK(gf != NULL, "open gif for metadata test");
        if (gf) {
            GifColorType pal[2] = {{0, 0, 0}, {255, 255, 255}};
            ColorMapObject *cm = GifMakeMapObject(2, pal);
            int okg = EGifPutScreenDesc(gf, 4, 3, 2, 0, cm) == GIF_OK;
            okg &= EGifPutComment(gf, "GIF metadata probe") == GIF_OK;
            okg &= EGifPutImageDesc(gf, 0, 0, 4, 3, 0, NULL) == GIF_OK;
            uint8_t raster[12];
            for (int i = 0; i < 12; i++)
                raster[i] = (uint8_t)(i & 1);
            okg &= EGifPutLine(gf, raster, 12) == GIF_OK;
            okg &= EGifCloseFile(gf, &ge) == GIF_OK;
            GifFreeMapObject(cm);
            CHECK(okg, "write gif with a comment");
        }
        FILE *gifh = img_fopen_read("testout/meta.gif");
        img_image_t gi;
        int okgi = gifh && gif_decode(gifh, &gi, err, sizeof(err)) == 0;
        if (gifh) fclose(gifh);
        CHECK(okgi, "decode gif with a comment");
        if (okgi) {
            CHECK(gi.meta.ntexts == 1 && !strcmp(gi.meta.texts[0].key, "Comment") &&
                  !strcmp(gi.meta.texts[0].value, "GIF metadata probe"),
                  "GIF comment read as a text pair");
            jxl_opts_t go = opts;
            go.effort = 3;
            CHECK(jxl_write_file(&gi, &go, "testout/metagif.jxl", err, sizeof(err)) == 0,
                  "encode gif with metadata");
            img_free(&gi);
            img_image_t gb;
            if (read_jxl_img("testout/metagif.jxl", &gb) == 3) {
                CHECK(gb.meta.ntexts == 1 &&
                      !strcmp(gb.meta.texts[0].value, "GIF metadata probe"),
                      "GIF comment survives into the jxl text box");
                img_free(&gb);
            }
        }
    }

    /* --- timestamp copy --- */
    printf("timestamp sync:\n");
    {
        FILE *f = img_fopen_write("testout/ts_in.bin");
        fwrite("x", 1, 1, f);
        fclose(f);
        FILE *g = img_fopen_write("testout/ts_out.bin");
        fwrite("y", 1, 1, g);
        fclose(g);
        /* make the input's times distinctly old (portable) */
        {
            /* raw payload 1e17: a very old timestamp on Windows (FILETIME),
             * harmless on POSIX where only mtime is applied */
            uint64_t old_v = 100000000000000000ULL;
            uint8_t c[16] = {0}, m[16] = {0};
            memcpy(c, &old_v, 8);
            memcpy(m, &old_v, 8);
            set_file_times("testout/ts_in.bin", c, m);
        }
        CHECK(copy_file_times("testout/ts_in.bin", "testout/ts_out.bin") == 0, "copy_file_times");
        uint64_t c_in = file_time_stamp("testout/ts_in.bin", 1);
        uint64_t c_out = file_time_stamp("testout/ts_out.bin", 1);
        uint64_t m_in = file_time_stamp("testout/ts_in.bin", 0);
        uint64_t m_out = file_time_stamp("testout/ts_out.bin", 0);
        CHECK(c_in == c_out && c_in != 0, "creation time matches");
        CHECK(m_in == m_out && m_in != 0, "modification time matches");
    }

    /* --- effort levels: 1 and 10 both work losslessly; 10 <= 1 in size --- */
    printf("effort levels:\n");
    {
        FILE *f = img_fopen_read("testout/t24.bmp");
        img_image_t img;
        if (!f || bmp_decode(f, &img, err, sizeof(err)) != 0) {
            if (f) fclose(f);
            printf("  SKIP  effort levels (source decode failed)\n");
        } else {
            fclose(f);
            jxl_opts_t o1 = opts; o1.effort = 1;
            jxl_opts_t o10 = opts; o10.effort = 10;
            CHECK(roundtrip_jxl(&img, &o1, "testout/e1.jxl", err, sizeof(err)) == 3,
                  "effort 1 round-trip lossless");
            CHECK(roundtrip_jxl(&img, &o10, "testout/e10.jxl", err, sizeof(err)) == 3,
                  "effort 10 round-trip lossless");
            struct stat st1 = {0}, st10 = {0};
            stat("testout/e1.jxl", &st1);
            stat("testout/e10.jxl", &st10);
            long long s1 = (long long)st1.st_size, s10 = (long long)st10.st_size;
            printf("        effort 1: %lld bytes, effort 10: %lld bytes\n", s1, s10);
            CHECK(s10 <= s1, "effort 10 compresses at least as well as effort 1");
            img_free(&img);
        }
    }

    /* --- modular off (encoder chooses) still lossless --- */
    printf("modular off:\n");
    {
        FILE *f = img_fopen_read("testout/t24.bmp");
        img_image_t img;
        if (!f || bmp_decode(f, &img, err, sizeof(err)) != 0) {
            if (f) fclose(f);
            printf("  SKIP  modular off (source decode failed)\n");
        } else {
            fclose(f);
            jxl_opts_t om = opts; om.modular = 0;
            CHECK(roundtrip_jxl(&img, &om, "testout/nomod.jxl", err, sizeof(err)) == 3,
                  "VarDCT-chosen round-trip lossless");
            img_free(&img);
        }
    }

    /* --- GIF: encode with giflib (palette + transparency), decode back --- */
    printf("GIF -> PALETTE8 with transparency:\n");
    {
        int ge = 0;
        GifFileType *gf = EGifOpenFileName("testout/t.gif", 0, &ge);
        CHECK(gf != NULL, "open gif for writing");
        if (gf) {
            GifColorType pal[4] = {{255,0,0},{0,255,0},{0,0,255},{10,20,30}};
            ColorMapObject *cm = GifMakeMapObject(4, pal);
            CHECK(EGifPutScreenDesc(gf, 4, 4, 2, 0, cm) == GIF_OK, "gif screen desc");
            /* the GCB extension must precede the image descriptor */
            GraphicsControlBlock gcb;
            memset(&gcb, 0, sizeof(gcb));
            gcb.DisposalMode = DISPOSE_DO_NOT;
            gcb.TransparentColor = 3;
            uint8_t gcb_buf[8];
            int gcb_len = EGifGCBToExtension(&gcb, gcb_buf);
            CHECK(EGifPutExtension(gf, GRAPHICS_EXT_FUNC_CODE, gcb_len, gcb_buf) == GIF_OK,
                  "gif transparency extension");
            CHECK(EGifPutImageDesc(gf, 0, 0, 4, 4, 0, NULL) == GIF_OK, "gif image desc");
            uint8_t raster[16];
            for (int i = 0; i < 16; i++) raster[i] = (uint8_t)(i % 4);
            CHECK(EGifPutLine(gf, raster, 16) == GIF_OK, "gif pixels");
            CHECK(EGifCloseFile(gf, &ge) == GIF_OK, "close gif");
            GifFreeMapObject(cm);
        }

        FILE *f = img_fopen_read("testout/t.gif");
        img_image_t img;
        int okgif = f && gif_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(okgif, "decode gif");
        if (f) fclose(f);
        if (okgif) {
            CHECK(img.color == IMG_PALETTE && img.bit_depth == 8, "format is PALETTE8");
            CHECK(img.has_pal_alpha && img.pal_alpha[3] == 0, "transparency preserved");
            int idx_ok = 1;
            for (int i = 0; i < 16; i++)
                if (img.data[i] != (uint8_t)(i % 4)) { idx_ok = 0; break; }
            CHECK(idx_ok, "indices identical");
            if (jxl_write_file(&img, &opts, "testout/tgif.jxl", err, sizeof(err)) != 0)
                CHECK(0, "encode jxl");
            img_free(&img);
            img_image_t got;
            int ch = read_jxl_img("testout/tgif.jxl", &got);
            CHECK(ch == 4, "jxl decodes as RGBA (palette expanded, tRNS kept)");
            if (ch == 4) {
                uint8_t *exp = (uint8_t *)malloc((size_t)got.width * got.height * 4);
                for (int i = 0; i < got.width * got.height; i++) {
                    int idx = i % 4;
                    exp[i*4+0] = pal_gif_r(idx); exp[i*4+1] = pal_gif_g(idx); exp[i*4+2] = pal_gif_b(idx);
                    exp[i*4+3] = (idx == 3) ? 0 : 255;
                }
                CHECK(compare_buf(got.data, exp, (size_t)got.width * got.height * 4) < 0,
                      "pixels identical (lossless)");
                free(exp);
            }
            img_free(&got);
        }
    }

    /* --- QOI --- */
    printf("QOI -> RGBA8:\n");
    {
        qoi_desc desc;
        desc.width = 6; desc.height = 4; desc.channels = 4; desc.colorspace = QOI_SRGB;
        uint8_t px[6 * 4 * 4];
        for (int i = 0; i < 6 * 4; i++) {
            px[i*4+0] = (uint8_t)(i * 7); px[i*4+1] = (uint8_t)(i * 3);
            px[i*4+2] = (uint8_t)(i * 11); px[i*4+3] = (uint8_t)(i & 1 ? 128 : 255);
        }
        int enclen = 0;
        void *enc = qoi_encode(px, &desc, &enclen);
        CHECK(enc != NULL, "qoi_encode");
        if (enc) {
            FILE *f = img_fopen_write("testout/t.qoi");
            int wok = f && fwrite(enc, 1, (size_t)enclen, f) == (size_t)enclen;
            if (f) fclose(f);
            free(enc);
            CHECK(wok, "write qoi");
            f = img_fopen_read("testout/t.qoi");
            img_image_t img;
            int okq = f && qoi_decode_file(f, &img, err, sizeof(err)) == 0;
            CHECK(okq, "decode qoi");
            if (f) fclose(f);
            if (okq) {
                CHECK(img.color == IMG_RGBA && img.width == 6 && img.height == 4, "format is RGBA8 6x4");
                int diff = (int)compare_buf(img.data, px, 6 * 4 * 4);
                CHECK(diff < 0, "pixels identical (lossless)");
                CHECK(roundtrip_jxl(&img, &opts, "testout/tqoi.jxl", err, sizeof(err)) == 4,
                      "qoi jxl round-trip identical");
                img_free(&img);
            }
        }
    }

    /* --- WebP (encode sample, decode, verify round-trip readability) --- */
    printf("WebP -> RGBA8:\n");
    {
        uint8_t px[8 * 5 * 4];
        for (int i = 0; i < 8 * 5; i++) {
            px[i*4+0] = (uint8_t)(i * 5); px[i*4+1] = (uint8_t)(255 - i * 5);
            px[i*4+2] = (uint8_t)((i * 13) & 0xFF); px[i*4+3] = (uint8_t)(i & 1 ? 200 : 255);
        }
        int wpsize = WebPEncodeRGBA(px, 8, 5, 8 * 4, 70.0f, &wp_blob);
        CHECK(wpsize > 0, "WebPEncodeRGBA");
        if (wpsize > 0) {
            FILE *f = img_fopen_write("testout/t.webp");
            int wok = f && fwrite(wp_blob, 1, (size_t)wpsize, f) == (size_t)wpsize;
            if (f) fclose(f);
            free(wp_blob);
            wp_blob = NULL;
            CHECK(wok, "write webp");
            f = img_fopen_read("testout/t.webp");
            img_image_t img;
            int okw = f && webp_decode(f, &img, err, sizeof(err)) == 0;
            CHECK(okw, "decode webp");
            if (f) fclose(f);
            if (okw) {
                CHECK(img.color == IMG_RGBA && img.width == 8 && img.height == 5, "format is RGBA8 8x5");
                CHECK(roundtrip_jxl(&img, &opts, "testout/twebp.jxl", err, sizeof(err)) == 4,
                      "webp jxl round-trip identical");
                img_free(&img);
            }
        }
    }

    /* --- TIFF 8-bit RGB (written with libtiff) --- */
    printf("TIFF RGB8 -> RGB8:\n");
    {
        TIFF *tf = TIFFOpen("testout/t.tiff", "w");
        CHECK(tf != NULL, "open tiff for writing");
        if (tf) {
            TIFFSetField(tf, TIFFTAG_IMAGEWIDTH, 5);
            TIFFSetField(tf, TIFFTAG_IMAGELENGTH, 3);
            TIFFSetField(tf, TIFFTAG_BITSPERSAMPLE, 8);
            TIFFSetField(tf, TIFFTAG_SAMPLESPERPIXEL, 3);
            TIFFSetField(tf, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
            TIFFSetField(tf, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            uint8_t row[5 * 3];
            int wok = 1;
            for (int y = 0; y < 3 && wok; y++) {
                for (int x = 0; x < 5; x++) {
                    row[x*3+0] = (uint8_t)(x * 40); row[x*3+1] = (uint8_t)(y * 80); row[x*3+2] = 77;
                }
                wok = TIFFWriteScanline(tf, row, (uint32_t)y, 0) >= 0;
            }
            CHECK(wok, "write tiff scanlines");
            TIFFClose(tf);
        }
        FILE *f = img_fopen_read("testout/t.tiff");
        img_image_t img;
        int okt = f && tiff_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(okt, "decode tiff");
        if (f) fclose(f);
        if (okt) {
            CHECK(img.color == IMG_RGB && img.bit_depth == 8 && img.width == 5 && img.height == 3,
                  "format is RGB8 5x3 (bit depth matched)");
            int diff = -1;
            for (int y = 0; y < 3 && diff < 0; y++)
                for (int x = 0; x < 5 && diff < 0; x++) {
                    uint8_t *d = img.data + ((size_t)y * 5 + x) * 3;
                    if (d[0] != (uint8_t)(x * 40) || d[1] != (uint8_t)(y * 80) || d[2] != 77)
                        diff = y * 5 + x;
                }
            CHECK(diff < 0, "pixels identical (lossless)");
            CHECK(roundtrip_jxl(&img, &opts, "testout/ttiff.jxl", err, sizeof(err)) == 3,
                  "tiff rgb8 jxl round-trip identical");
            img_free(&img);
        }
    }

    /* --- TIFF 16-bit RGB: bit depth must be preserved --- */
    printf("TIFF RGB16 -> RGB16 (bit depth preserved):\n");
    {
        TIFF *tf = TIFFOpen("testout/t16.tiff", "w");
        CHECK(tf != NULL, "open tiff16 for writing");
        if (tf) {
            TIFFSetField(tf, TIFFTAG_IMAGEWIDTH, 5);
            TIFFSetField(tf, TIFFTAG_IMAGELENGTH, 3);
            TIFFSetField(tf, TIFFTAG_BITSPERSAMPLE, 16);
            TIFFSetField(tf, TIFFTAG_SAMPLESPERPIXEL, 3);
            TIFFSetField(tf, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
            TIFFSetField(tf, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            uint16_t row[5 * 3];
            int wok = 1;
            for (int y = 0; y < 3 && wok; y++) {
                for (int x = 0; x < 5; x++) {
                    row[(size_t)x*3+0] = (uint16_t)(x * 10000);
                    row[(size_t)x*3+1] = (uint16_t)(y * 20000);
                    row[(size_t)x*3+2] = 12345;
                }
                wok = TIFFWriteScanline(tf, (uint8_t *)row, (uint32_t)y, 0) >= 0;
            }
            CHECK(wok, "write tiff16 scanlines");
            TIFFClose(tf);
        }
        FILE *f = img_fopen_read("testout/t16.tiff");
        img_image_t img;
        int okt = f && tiff_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(okt, "decode tiff16");
        if (f) fclose(f);
        if (okt) {
            CHECK(img.color == IMG_RGB && img.bit_depth == 16,
                  "format is RGB16 (bit depth preserved)");
            int diff = -1;
            for (int y = 0; y < 3 && diff < 0; y++)
                for (int x = 0; x < 5 && diff < 0; x++) {
                    const uint8_t *d = img.data + ((size_t)y * 5 + x) * 6;
                    unsigned r = ((unsigned)d[0] << 8) | d[1];
                    unsigned g = ((unsigned)d[2] << 8) | d[3];
                    unsigned b = ((unsigned)d[4] << 8) | d[5];
                    if (r != (unsigned)(x * 10000) || g != (unsigned)(y * 20000) || b != 12345)
                        diff = y * 5 + x;
                }
            CHECK(diff < 0, "16-bit values identical (lossless)");
            CHECK(roundtrip_jxl(&img, &opts, "testout/ttiff16.jxl", err, sizeof(err)) == 3,
                  "tiff rgb16 jxl round-trip identical");
            img_free(&img);
        }
    }

    /* --- TIFF 16-bit gray: bit depth must be preserved --- */
    printf("TIFF GRAY16 -> GRAY16:\n");
    {
        TIFF *tf = TIFFOpen("testout/tg16.tiff", "w");
        CHECK(tf != NULL, "open tiff gray16 for writing");
        if (tf) {
            TIFFSetField(tf, TIFFTAG_IMAGEWIDTH, 7);
            TIFFSetField(tf, TIFFTAG_IMAGELENGTH, 2);
            TIFFSetField(tf, TIFFTAG_BITSPERSAMPLE, 16);
            TIFFSetField(tf, TIFFTAG_SAMPLESPERPIXEL, 1);
            TIFFSetField(tf, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
            TIFFSetField(tf, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            uint16_t row[7];
            int wok = 1;
            for (int y = 0; y < 2 && wok; y++) {
                for (int x = 0; x < 7; x++)
                    row[x] = (uint16_t)(x * 9000 + y);
                wok = TIFFWriteScanline(tf, (uint8_t *)row, (uint32_t)y, 0) >= 0;
            }
            CHECK(wok, "write tiff gray16 scanlines");
            TIFFClose(tf);
        }
        FILE *f = img_fopen_read("testout/tg16.tiff");
        img_image_t img;
        int okt = f && tiff_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(okt, "decode tiff gray16");
        if (f) fclose(f);
        if (okt) {
            CHECK(img.color == IMG_GRAY && img.bit_depth == 16, "format is GRAY16");
            CHECK(roundtrip_jxl(&img, &opts, "testout/ttg16.jxl", err, sizeof(err)) == 1,
                  "tiff gray16 jxl round-trip identical");
            img_free(&img);
        }
    }

    /* --- TIFF 8-bit palette --- */
    printf("TIFF PALETTE8 -> PALETTE8:\n");
    {
        TIFF *tf = TIFFOpen("testout/tp.tiff", "w");
        CHECK(tf != NULL, "open tiff palette for writing");
        if (tf) {
            TIFFSetField(tf, TIFFTAG_IMAGEWIDTH, 4);
            TIFFSetField(tf, TIFFTAG_IMAGELENGTH, 2);
            TIFFSetField(tf, TIFFTAG_BITSPERSAMPLE, 8);
            TIFFSetField(tf, TIFFTAG_SAMPLESPERPIXEL, 1);
            TIFFSetField(tf, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_PALETTE);
            TIFFSetField(tf, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            uint16_t cmap[3][256];
            for (int i = 0; i < 256; i++) {
                cmap[0][i] = (uint16_t)(i * 257);
                cmap[1][i] = (uint16_t)(255 * 257 - i * 257);
                cmap[2][i] = (uint16_t)(i * 128 + 4000);
            }
            TIFFSetField(tf, TIFFTAG_COLORMAP, cmap[0], cmap[1], cmap[2]);
            uint8_t row[4];
            int wok = 1;
            for (int y = 0; y < 2 && wok; y++) {
                for (int x = 0; x < 4; x++)
                    row[x] = (uint8_t)((x + y * 2) % 256);
                wok = TIFFWriteScanline(tf, row, (uint32_t)y, 0) >= 0;
            }
            CHECK(wok, "write tiff palette scanlines");
            TIFFClose(tf);
        }
        FILE *f = img_fopen_read("testout/tp.tiff");
        img_image_t img;
        int okt = f && tiff_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(okt, "decode tiff palette");
        if (f) fclose(f);
        if (okt) {
            CHECK(img.color == IMG_PALETTE && img.bit_depth == 8 && img.pal_ncolors == 256,
                  "format is PALETTE8 256 colors");
            int idx_ok = 1;
            for (int y = 0; y < 2 && idx_ok; y++)
                for (int x = 0; x < 4; x++)
                    if (img.data[(size_t)y * img.rowstride + x] != (uint8_t)((x + y * 2) % 256))
                        { idx_ok = 0; break; }
            CHECK(idx_ok, "palette indices identical");
            CHECK(roundtrip_jxl(&img, &opts, "testout/ttp.jxl", err, sizeof(err)) == 3,
                  "tiff palette jxl round-trip identical");
            img_free(&img);
        }
    }

    /* --- animated GIF -> JXL animation (3 frames, offsets, disposal, loop) --- */
    printf("animated GIF -> JXL animation:\n");
    {
        /* write a 6x4 3-frame GIF: frame0 full canvas (opaque), frame1
         * 4x2 sub-rect at (1,1) with disposal BACKGROUND, frame2 full
         * canvas with transparency index 3; NETSCAPE loop = 0 (infinite) */
        int ge = 0;
        GifFileType *gf = EGifOpenFileName("testout/anim.gif", 0, &ge);
        CHECK(gf != NULL, "open animated gif for writing");
        if (gf) {
            GifColorType pal[4] = {{255,0,0},{0,255,0},{0,0,255},{10,20,30}};
            ColorMapObject *cm = GifMakeMapObject(4, pal);
            CHECK(EGifPutScreenDesc(gf, 6, 4, 2, 0, cm) == GIF_OK, "gif screen desc");
            /* NETSCAPE loop = infinite */
            uint8_t ns_data[3] = {1, 0, 0};
            CHECK(EGifPutExtensionLeader(gf, APPLICATION_EXT_FUNC_CODE) == GIF_OK,
                  "netscape leader");
            CHECK(EGifPutExtensionBlock(gf, 11, "NETSCAPE2.0") == GIF_OK,
                  "netscape id");
            CHECK(EGifPutExtensionBlock(gf, 3, ns_data) == GIF_OK,
                  "netscape loop data");
            CHECK(EGifPutExtensionTrailer(gf) == GIF_OK, "netscape trailer");

            struct {
                int x, y, w, h, delay, dispose, trans;
                uint8_t fill;
            } frames[3] = {
                {0, 0, 6, 4, 10, 1, -1, 0},
                {1, 1, 4, 2, 20, 2,  3, 1},
                {0, 0, 6, 4,  5, 1,  3, 2},
            };
            int all_ok = 1;
            for (int i = 0; i < 3 && all_ok; i++) {
                GraphicsControlBlock gcb;
                memset(&gcb, 0, sizeof(gcb));
                gcb.DisposalMode = frames[i].dispose;
                gcb.UserInputFlag = 0;
                gcb.DelayTime = frames[i].delay;
                gcb.TransparentColor = frames[i].trans;
                uint8_t gcb_buf[8];
                int gcb_len = EGifGCBToExtension(&gcb, gcb_buf);
                all_ok &= EGifPutExtension(gf, GRAPHICS_EXT_FUNC_CODE, gcb_len, gcb_buf) == GIF_OK;
                all_ok &= EGifPutImageDesc(gf, frames[i].x, frames[i].y,
                                           frames[i].w, frames[i].h, 0, NULL) == GIF_OK;
                int npix = frames[i].w * frames[i].h;
                uint8_t raster[64];
                for (int j = 0; j < npix; j++)
                    raster[j] = (uint8_t)((frames[i].fill + j) % 4);
                all_ok &= EGifPutLine(gf, raster, npix) == GIF_OK;
            }
            CHECK(all_ok, "write 3 gif frames");
            CHECK(EGifCloseFile(gf, &ge) == GIF_OK, "close gif");
            GifFreeMapObject(cm);

            /* decode animation */
            FILE *f = img_fopen_read("testout/anim.gif");
            img_animation_t anim;
            int oka = f && gif_decode_anim(f, &anim, err, sizeof(err)) == 0;
            CHECK(oka, "gif_decode_anim");
            if (f) fclose(f);
            if (oka) {
                CHECK(anim.nframes == 3 && anim.width == 6 && anim.height == 4,
                      "3 frames, 6x4 canvas");
                CHECK(anim.delays_cs[0] == 10 && anim.delays_cs[1] == 20 &&
                      anim.delays_cs[2] == 5, "per-frame delays preserved");
                CHECK(anim.dispose[0] == 1 && anim.dispose[1] == 2 &&
                      anim.dispose[2] == 1, "disposal modes preserved");
                CHECK(anim.x[1] == 1 && anim.y[1] == 1 &&
                      anim.frames[1].width == 4 && anim.frames[1].height == 2,
                      "frame 1 sub-rect offset preserved");
                CHECK(anim.loops == 0, "NETSCAPE loop=infinite mapped to 0");
                CHECK(anim.frames[0].color == IMG_RGBA, "frames are RGBA");

                jxl_opts_t ao = opts;
                CHECK(jxl_write_anim(&anim, &ao, "testout/anim.jxl",
                                     err, sizeof(err)) == 0, "jxl_write_anim");

                /* build the expected displayed canvases the same way the
                 * GIF decoder composites (region paste + disposal) */
                int CW = anim.width, CH = anim.height;
                uint8_t *canvas = (uint8_t *)calloc((size_t)CW * CH, 4);
                uint8_t *expected[3];
                for (int i = 0; i < 3; i++) {
                    const img_image_t *fr = &anim.frames[i];
                    for (int yy = 0; yy < fr->height; yy++)
                        for (int xx = 0; xx < fr->width; xx++) {
                            const uint8_t *s = fr->data + (size_t)yy * fr->rowstride + (size_t)xx * 4;
                            uint8_t *d = canvas + (size_t)((anim.y[i] + yy) * CW + anim.x[i] + xx) * 4;
                            if (s[3] != 0)
                                memcpy(d, s, 4);
                        }
                    expected[i] = (uint8_t *)malloc((size_t)CW * CH * 4);
                    memcpy(expected[i], canvas, (size_t)CW * CH * 4);
                    if (anim.dispose[i] == 2)
                        for (int yy = 0; yy < fr->height; yy++)
                            memset(canvas + (size_t)((anim.y[i] + yy) * CW + anim.x[i]) * 4,
                                   0, (size_t)fr->width * 4);
                }

                jxl_anim_t ja;
                int okr = read_jxl_anim("testout/anim.jxl", &ja, err, sizeof(err)) == 0;
                CHECK(okr, "decode animated jxl");
                if (!okr)
                    printf("        (read_jxl_anim: %s)\n", err);
                if (okr) {
                    CHECK(ja.w == 6 && ja.h == 4, "canvas size 6x4");
                    CHECK(ja.num_loops == 0, "loop count 0 (infinite)");
                    /* collect displayed frames (duration > 0); zero-duration
                     * clear frames exist but are not displayed */
                    int shown[8], nshown = 0;
                    for (int i = 0; i < ja.nframes && nshown < 8; i++)
                        if (ja.durations[i] > 0)
                            shown[nshown++] = i;
                    CHECK(nshown == 3, "3 displayed frames (clear frames hidden)");
                    if (nshown == 3) {
                        CHECK(ja.durations[shown[0]] == 10 && ja.durations[shown[1]] == 20 &&
                              ja.durations[shown[2]] == 5, "durations 10/20/5 ticks");
                        int pix_ok = 1;
                        for (int k = 0; k < 3 && pix_ok; k++)
                            if (compare_buf(ja.frames[shown[k]], expected[k],
                                            (size_t)CW * CH * 4) >= 0)
                                pix_ok = 0;
                        CHECK(pix_ok, "frame canvases identical (composition lossless)");
                    }
                    free_jxl_anim(&ja);
                }
                for (int i = 0; i < 3; i++)
                    free(expected[i]);
                free(canvas);
                img_free_anim(&anim);
            }
        }
    }

    /* --- 16-bit gray PNG with tRNS: no crash, alpha produced, depth kept --- */
    printf("PNG 16-bit gray + tRNS:\n");
    {
        const unsigned TRNS = 33000;
        CHECK(gen_png16_gray_trns("testout/g16t.png", 4, 3, TRNS) == 0,
              "generate 16-bit gray + tRNS png");
        FILE *f = img_fopen_read("testout/g16t.png");
        uint8_t *png_buf = NULL;
        size_t png_len = 0;
        if (f) {
            fseek(f, 0, SEEK_END);
            png_len = (size_t)ftell(f);
            rewind(f);
            png_buf = (uint8_t *)malloc(png_len);
            if (fread(png_buf, 1, png_len, f) != png_len) { free(png_buf); png_buf = NULL; }
            fclose(f);
        }
        img_image_t img;
        int okp = png_buf && png_decode_mem(png_buf, png_len, &img, err, sizeof(err)) == 0;
        CHECK(okp, "decode 16-bit gray + tRNS");
        free(png_buf);
        if (okp) {
            CHECK(img.color == IMG_GRAY && img.bit_depth == 16 && img.has_gray_trns &&
                  img.gray_trns_value == TRNS, "GRAY16 with tRNS value kept");
            jxl_opts_t o16 = opts; o16.effort = 3;
            CHECK(jxl_write_file(&img, &o16, "testout/g16t.jxl", err, sizeof(err)) == 0,
                  "encode 16-bit gray + tRNS (no crash)");
            img_free(&img);
            img_image_t got;
            int ch = read_jxl_img("testout/g16t.jxl", &got);
            CHECK(ch == 2 && got.bit_depth == 16, "decodes as GRAY_ALPHA 16-bit");
            if (ch == 2) {
                int alpha_ok = 1, color_ok = 1;
                for (int y = 0; y < got.height; y++) {
                    const uint8_t *d = got.data + (size_t)y * got.rowstride;
                    for (int x = 0; x < got.width; x++) {
                        unsigned g = img_ld16be(d + (size_t)x * 4);
                        unsigned a = img_ld16be(d + (size_t)x * 4 + 2);
                        unsigned eg = (x == 1 && y == 0) ? TRNS
                                                         : (unsigned)((x * 4096 + y * 1000) & 0xFFFF);
                        if (g != eg) color_ok = 0;
                        unsigned ea = (x == 1 && y == 0) ? 0 : 65535;
                        if (a != ea) alpha_ok = 0;
                    }
                }
                CHECK(color_ok, "gray samples unchanged");
                CHECK(alpha_ok, "tRNS sample is transparent, others opaque");
                img_free(&got);
            }
        }
    }

    /* --- TIFF gray + alpha (spp=2): alpha must survive --- */
    printf("TIFF gray+alpha -> GRAY_ALPHA:\n");
    for (int bits = 8; bits <= 16; bits += 8) {
        char path[64];
        snprintf(path, sizeof(path), "testout/tga%d.tiff", bits);
        TIFF *tf = TIFFOpen(path, "w");
        CHECK(tf != NULL, bits == 8 ? "open tiff gray8+alpha for writing"
                                    : "open tiff gray16+alpha for writing");
        if (!tf)
            continue;
        uint16_t spp = 2, extra = EXTRASAMPLE_UNASSALPHA;
        TIFFSetField(tf, TIFFTAG_IMAGEWIDTH, 4);
        TIFFSetField(tf, TIFFTAG_IMAGELENGTH, 2);
        TIFFSetField(tf, TIFFTAG_BITSPERSAMPLE, (uint16_t)bits);
        TIFFSetField(tf, TIFFTAG_SAMPLESPERPIXEL, spp);
        TIFFSetField(tf, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
        TIFFSetField(tf, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tf, TIFFTAG_EXTRASAMPLES, 1, &extra);
        uint16_t row16[8];
        uint8_t row8[8];
        int wok = 1;
        for (int y = 0; y < 2 && wok; y++) {
            for (int x = 0; x < 4; x++) {
                unsigned g = (unsigned)(x * 50 + y);
                unsigned a = (x < 2) ? 0u : (unsigned)(bits == 16 ? 30000 : 200);
                if (bits == 16) {
                    row16[x * 2] = (uint16_t)g;
                    row16[x * 2 + 1] = (uint16_t)a;
                } else {
                    row8[x * 2] = (uint8_t)g;
                    row8[x * 2 + 1] = (uint8_t)a;
                }
            }
            wok = TIFFWriteScanline(tf, bits == 16 ? (void *)row16 : (void *)row8,
                                    (uint32_t)y, 0) >= 0;
        }
        CHECK(wok, "write gray+alpha scanlines");
        TIFFClose(tf);

        FILE *f = img_fopen_read(path);
        img_image_t img;
        int okt = f && tiff_decode(f, &img, err, sizeof(err)) == 0;
        if (f) fclose(f);
        CHECK(okt, "decode gray+alpha tiff");
        if (!okt)
            continue;
        CHECK(img.color == IMG_GRAY_ALPHA && img.bit_depth == bits,
              "format is GRAY_ALPHA at the source depth (alpha kept)");
        int px_ok = 1;
        for (int y = 0; y < 2 && px_ok; y++)
            for (int x = 0; x < 4; x++) {
                unsigned g, a;
                if (bits == 16) {
                    const uint8_t *d = img.data + (size_t)y * img.rowstride + (size_t)x * 4;
                    g = img_ld16be(d); a = img_ld16be(d + 2);
                } else {
                    const uint8_t *d = img.data + (size_t)y * img.rowstride + (size_t)x * 2;
                    g = d[0]; a = d[1];
                }
                unsigned eg = (unsigned)(x * 50 + y);
                unsigned ea = (x < 2) ? 0u : (unsigned)(bits == 16 ? 30000 : 200);
                if (g != eg || a != ea) { px_ok = 0; break; }
            }
        CHECK(px_ok, "gray and alpha samples identical (lossless)");
        img_free(&img);
    }

    /* --- JXL 12-bit: the declared depth must survive a re-encode --- */
    printf("JXL 12-bit declared depth:\n");
    {
        enum { W12 = 8, H12 = 4 };
        uint16_t px[W12 * H12 * 3];
        for (int y = 0; y < H12; y++)
            for (int x = 0; x < W12; x++) {
                uint16_t *p = px + ((size_t)y * W12 + x) * 3;
                p[0] = (uint16_t)(x * (4095 / (W12 - 1)));
                p[1] = (uint16_t)(y * (4095 / (H12 - 1)));
                p[2] = (uint16_t)((x * 137 + y * 311) & 4095);
            }
        /* write a 12-bit JXL with the libjxl encoder (unscaled samples) */
        JxlEncoder *enc = JxlEncoderCreate(NULL);
        JxlEncoderFrameSettings *fs = JxlEncoderFrameSettingsCreate(enc, NULL);
        JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_EFFORT, 3);
        JxlEncoderSetFrameLossless(fs, JXL_TRUE);
        JxlBasicInfo bi;
        JxlEncoderInitBasicInfo(&bi);
        bi.xsize = W12;
        bi.ysize = H12;
        bi.bits_per_sample = 12;
        bi.num_color_channels = 3;
        bi.uses_original_profile = JXL_TRUE;
        CHECK(JxlEncoderSetBasicInfo(enc, &bi) == JXL_ENC_SUCCESS, "encoder: 12-bit basic info");
        JxlColorEncoding ce;
        JxlColorEncodingSetToSRGB(&ce, JXL_FALSE);
        JxlEncoderSetColorEncoding(enc, &ce);
        JxlBitDepth bd;
        bd.type = JXL_BIT_DEPTH_FROM_CODESTREAM;
        bd.bits_per_sample = 12;
        bd.exponent_bits_per_sample = 0;
        JxlEncoderSetFrameBitDepth(fs, &bd);
        JxlPixelFormat pf;
        memset(&pf, 0, sizeof(pf));
        pf.num_channels = 3;
        pf.data_type = JXL_TYPE_UINT16;
        pf.endianness = JXL_NATIVE_ENDIAN;
        int enc_ok = JxlEncoderAddImageFrame(fs, &pf, px, sizeof(px)) == JXL_ENC_SUCCESS;
        JxlEncoderCloseInput(enc);
        uint8_t *out = (uint8_t *)malloc(1 << 16);
        size_t cap = 1 << 16;
        uint8_t *np = out;
        while (enc_ok) {
            JxlEncoderStatus st = JxlEncoderProcessOutput(enc, &np, &cap);
            if (st == JXL_ENC_SUCCESS)
                break;
            if (st != JXL_ENC_NEED_MORE_OUTPUT) { enc_ok = 0; break; }
        }
        if (enc_ok) {
            FILE *f = img_fopen_write("testout/g12.jxl");
            enc_ok = f && fwrite(out, 1, (size_t)(np - out), f) == (size_t)(np - out);
            if (f) fclose(f);
        }
        free(out);
        JxlEncoderDestroy(enc);
        CHECK(enc_ok, "write 12-bit jxl sample");

        /* img2jxl must read it unscaled and remember the declared depth */
        FILE *f = img_fopen_read("testout/g12.jxl");
        img_image_t img;
        int is_anim = 0;
        int rc12 = f ? jxl_decode_file(f, &img, &is_anim, err, sizeof(err)) : -1;
        if (f) fclose(f);
        CHECK(rc12 == 0, "decode 12-bit jxl");
        if (rc12 == 0) {
            CHECK(img.bit_depth == 16 && img.nominal_depth == 12 &&
                  img_nominal_depth(&img) == 12,
                  "storage 16-bit, declared depth 12");
            int same = 1;
            for (int y = 0; y < H12 && same; y++)
                for (int x = 0; x < W12; x++) {
                    const uint8_t *d = img.data + (size_t)y * img.rowstride + (size_t)x * 6;
                    for (int c = 0; c < 3; c++)
                        if (img_ld16be(d + c * 2) != px[((size_t)y * W12 + x) * 3 + c])
                            same = 0;
                }
            CHECK(same, "12-bit sample values unchanged (no rescaling)");

            jxl_opts_t o12 = opts;
            o12.effort = 3;
            CHECK(jxl_write_file(&img, &o12, "testout/g12_re.jxl", err, sizeof(err)) == 0,
                  "re-encode 12-bit jxl");
            img_free(&img);

            /* the re-encoded file must still declare 12 bits */
            FILE *rf = img_fopen_read("testout/g12_re.jxl");
            int declared = 0;
            if (rf) {
                fseek(rf, 0, SEEK_END);
                long sz = ftell(rf);
                rewind(rf);
                uint8_t *b = (uint8_t *)malloc((size_t)sz);
                if (b && fread(b, 1, (size_t)sz, rf) == (size_t)sz) {
                    JxlDecoder *d = JxlDecoderCreate(NULL);
                    JxlDecoderSubscribeEvents(d, JXL_DEC_BASIC_INFO);
                    JxlDecoderSetInput(d, b, (size_t)sz);
                    for (;;) {
                        JxlDecoderStatus st = JxlDecoderProcessInput(d);
                        if (st == JXL_DEC_BASIC_INFO) {
                            JxlBasicInfo ri;
                            if (JxlDecoderGetBasicInfo(d, &ri) == JXL_DEC_SUCCESS)
                                declared = (int)ri.bits_per_sample;
                            break;
                        }
                        if (st == JXL_DEC_SUCCESS || st == JXL_DEC_ERROR ||
                            st == JXL_DEC_NEED_MORE_INPUT)
                            break;
                    }
                    JxlDecoderDestroy(d);
                }
                free(b);
                fclose(rf);
            }
            CHECK(declared == 12, "re-encoded jxl still declares 12 bits");
        }
    }

    /* --- .jxl input rejects garbage with a clear error --- */
    printf("JXL error path:\n");
    {
        FILE *f = img_fopen_write("testout/fake.jxl");
        if (f) {
            fwrite("{garbage-not-a-jxl}", 1, 19, f);
            fclose(f);
        }
        f = img_fopen_read("testout/fake.jxl");
        img_image_t img;
        int is_anim = 0;
        err[0] = 0;
        int rc = f ? jxl_decode_file(f, &img, &is_anim, err, sizeof(err)) : -1;
        CHECK(rc != 0, "jxl rejects garbage");
        CHECK(err[0] != 0, "error message present");
        if (f) fclose(f);
    }

    printf("\n%s (%d failures)\n", g_failures ? "SELF-TEST FAILED" : "SELF-TEST PASSED", g_failures);
    return g_failures ? 1 : 0;
}
