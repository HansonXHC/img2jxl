#include "convert.h"
#include "image.h"
#include "dec/decode.h"
#include "enc/jxlenc.h"
#include "util/filetime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifdef _WIN32
#define img_stricmp _stricmp
#else
#define img_stricmp strcasecmp
#endif
#include "util/platform.h"

typedef enum { FMT_UNKNOWN = 0, FMT_BMP, FMT_TGA, FMT_PNM, FMT_ICO, FMT_JPEG,
               FMT_PNG, FMT_GIF, FMT_QOI, FMT_WEBP, FMT_TIFF, FMT_JXL } fmt_t;

static fmt_t fmt_from_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash1 = strrchr(path, '/');
    const char *slash2 = strrchr(path, '\\');
    const char *sep = slash1 > slash2 ? slash1 : slash2;
    if (!dot || (sep && dot < sep))
        return FMT_UNKNOWN;
    dot++;
    if (!img_stricmp(dot, "bmp") || !img_stricmp(dot, "dib")) return FMT_BMP;
    if (!img_stricmp(dot, "tga"))                          return FMT_TGA;
    if (!img_stricmp(dot, "pnm") || !img_stricmp(dot, "ppm") ||
        !img_stricmp(dot, "pgm") || !img_stricmp(dot, "pbm")) return FMT_PNM;
    if (!img_stricmp(dot, "ico"))                          return FMT_ICO;
    if (!img_stricmp(dot, "jpg") || !img_stricmp(dot, "jpeg") ||
        !img_stricmp(dot, "jfif"))                         return FMT_JPEG;
    if (!img_stricmp(dot, "png"))                          return FMT_PNG;
    if (!img_stricmp(dot, "gif"))                          return FMT_GIF;
    if (!img_stricmp(dot, "qoi"))                          return FMT_QOI;
    if (!img_stricmp(dot, "webp"))                         return FMT_WEBP;
    if (!img_stricmp(dot, "tif") || !img_stricmp(dot, "tiff")) return FMT_TIFF;
    if (!img_stricmp(dot, "jxl"))                          return FMT_JXL;
    return FMT_UNKNOWN;
}

static fmt_t sniff_format(const char *path)
{
    FILE *f = img_fopen_read(path);
    if (!f)
        return FMT_UNKNOWN;
    uint8_t b[12] = {0};
    size_t n = fread(b, 1, sizeof(b), f);
    fclose(f);
    if (n >= 2 && b[0] == 'B' && b[1] == 'M') return FMT_BMP;
    if (n >= 2 && b[0] == 0xFF && b[1] == 0xD8) return FMT_JPEG;
    if (n >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0) return FMT_ICO;
    if (n >= 2 && b[0] == 'P' && b[1] >= '1' && b[1] <= '6') return FMT_PNM;
    if (n >= 6 && !memcmp(b, "GIF8", 4)) return FMT_GIF;
    if (n >= 4 && !memcmp(b, "qoif", 4)) return FMT_QOI;
    if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4)) return FMT_WEBP;
    if (n >= 4 && !memcmp(b, "II*\0", 4)) return FMT_TIFF;
    if (n >= 4 && !memcmp(b, "MM\0*", 4)) return FMT_TIFF;
    /* JPEG XL: raw codestream FF 0A, or ISO-BMFF container */
    if (n >= 2 && b[0] == 0xFF && b[1] == 0x0A) return FMT_JXL;
    if (n >= 8 && !memcmp(b, "\x00\x00\x00\x0CJXL ", 7)) return FMT_JXL;
    return FMT_UNKNOWN;
}

const char *img2jxl_color_name(int color)
{
    static const char *names[] = { "GRAY", "GRAY_ALPHA", "PALETTE", "RGB", "RGBA" };
    if (color < 0 || color > 4)
        return "?";
    return names[color];
}

/* Extension-based test used by directory scanners (CLI batch and GUI drop). */
int img2jxl_is_supported_file(const char *path)
{
    return fmt_from_ext(path) != FMT_UNKNOWN;
}

static unsigned long long file_size(const char *path)
{
#if defined(_WIN32) && !defined(IMG2JXL_FORCE_POSIX)
    HANDLE h = CreateFileA(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    LARGE_INTEGER sz = {0};
    GetFileSizeEx(h, &sz);
    CloseHandle(h);
    return (unsigned long long)sz.QuadPart;
#else
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return (unsigned long long)st.st_size;
#endif
}

/* Read the whole file into memory; returns 0 on success. */
static int read_whole(FILE *f, uint8_t **out, size_t *outlen,
                      char *err, size_t errlen)
{
    long fsize = -1;
    if (fseek(f, 0, SEEK_END) == 0) {
        fsize = ftell(f);
        rewind(f);
    }
    uint8_t *buf = (fsize > 0) ? (uint8_t *)malloc((size_t)fsize) : NULL;
    if (!buf || fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(buf);
        snprintf(err, errlen, "cannot read input file");
        return -1;
    }
    *out = buf;
    *outlen = (size_t)fsize;
    return 0;
}

/* fill timing/size info and report success */
static int finish_ok(img2jxl_result_t *result, const char *out_path,
                     int have_times, const uint8_t *ctime, const uint8_t *mtime,
                     double t0, unsigned long long in_size)
{
    if (have_times)
        set_file_times(out_path, ctime, mtime);
    result->secs = img_now_sec() - t0;
    result->in_size = in_size;
    result->out_size = file_size(out_path);
    result->ok = 1;
    return 0;
}

int img2jxl_convert(const char *in_path, const char *out_path,
                    const jxl_opts_t *opts, int keep_time,
                    img2jxl_result_t *result)
{
    memset(result, 0, sizeof(*result));

    fmt_t fmt = fmt_from_ext(in_path);
    if (fmt == FMT_UNKNOWN)
        fmt = sniff_format(in_path);
    if (fmt == FMT_UNKNOWN) {
        snprintf(result->err, sizeof(result->err), "unsupported input format");
        return -1;
    }

    double t0 = img_now_sec();

    /* save the source's times before any (possibly in-place) write */
    uint8_t ctime[8] = {0}, mtime[8] = {0};
    int have_times = keep_time && get_file_times(in_path, ctime, mtime) == 0;
    unsigned long long in_size = file_size(in_path);

    FILE *f = img_fopen_read(in_path);
    if (!f) {
        snprintf(result->err, sizeof(result->err), "cannot open input file");
        return -1;
    }

    int rc;

    /* ---------------- animated GIF -> JXL animation ---------------- */
    if (fmt == FMT_GIF) {
        img_animation_t anim;
        if (gif_decode_anim(f, &anim, result->err, sizeof(result->err)) == 0 &&
            anim.nframes > 1) {
            result->out_w = anim.width;
            result->out_h = anim.height;
            result->out_depth = 8;
            result->out_color = IMG_RGBA;
            result->out_frames = anim.nframes;
            rc = jxl_write_anim(&anim, opts, out_path,
                                result->err, sizeof(result->err));
            img_free_anim(&anim);
            fclose(f);
            if (rc != 0)
                return -1;
            return finish_ok(result, out_path, have_times, ctime, mtime,
                             t0, in_size);
        }
        /* single-frame GIF (or animation decode failure): rewind and use
         * the still-image path, which keeps PALETTE output */
        img_free_anim(&anim);   /* safe: anim is zeroed on entry/failure */
        rewind(f);
    }

    /* ---------------- JPEG -> JXL bitstream transcoding ---------------- */
    if (fmt == FMT_JPEG) {
        uint8_t *buf = NULL;
        size_t buflen = 0;
        if (read_whole(f, &buf, &buflen, result->err, sizeof(result->err)) != 0) {
            fclose(f);
            return -1;
        }
        fclose(f);

        int jw, jh, jgray;
        if (jpeg_parse_dims(buf, buflen, &jw, &jh, &jgray) != 0) {
            free(buf);
            snprintf(result->err, sizeof(result->err), "jpeg: not a JPEG file");
            return -1;
        }
        result->out_w = jw;
        result->out_h = jh;
        result->out_depth = 8;
        result->out_color = jgray ? IMG_GRAY : IMG_RGB;
        result->out_frames = 1;

        rc = jxl_transcode_jpeg(buf, buflen, opts, out_path,
                                result->err, sizeof(result->err));
        free(buf);
        if (rc != 0) {
            /* fall back to the pixel path: decode and re-encode losslessly */
            f = img_fopen_read(in_path);
            if (!f) {
                snprintf(result->err, sizeof(result->err), "cannot open input file");
                return -1;
            }
            img_image_t img;
            if (jpeg_decode(f, &img, result->err, sizeof(result->err)) != 0) {
                fclose(f);
                return -1;
            }
            fclose(f);
            result->out_w = img.width;
            result->out_h = img.height;
            result->out_depth = img_nominal_depth(&img);
            result->out_color = img.color;
            rc = jxl_write_file(&img, opts, out_path, result->err, sizeof(result->err));
            img_free(&img);
            if (rc != 0)
                return -1;
        }
        return finish_ok(result, out_path, have_times, ctime, mtime, t0, in_size);
    }

    /* ---------------- .jxl input: re-encode with current settings ------- */
    if (fmt == FMT_JXL) {
        /* verify the signature first: a ".jxl"-named file may be something
         * else; reject it when no known format is detectable either */
        uint8_t sig[12] = {0};
        size_t got = fread(sig, 1, sizeof(sig), f);
        rewind(f);
        int looks_jxl = (got >= 2 && sig[0] == 0xFF && sig[1] == 0x0A) ||
                        (got >= 8 && !memcmp(sig, "\x00\x00\x00\x0CJXL ", 7));
        if (!looks_jxl) {
            if (sig[0] == 0xFF && sig[1] == 0xD8)
                fmt = FMT_JPEG;
            else if (sig[0] == 'B' && sig[1] == 'M')
                fmt = FMT_BMP;
            else if (sig[0] == 0 && sig[1] == 0 && sig[2] == 1 && sig[3] == 0)
                fmt = FMT_ICO;
            else if (sig[0] == 'P' && sig[1] >= '1' && sig[1] <= '6')
                fmt = FMT_PNM;
            else if (got >= 8 && !memcmp(sig, "\x89PNG", 4))
                fmt = FMT_PNG;
            else if (got >= 6 && !memcmp(sig, "GIF8", 4))
                fmt = FMT_GIF;
            else if (got >= 4 && !memcmp(sig, "qoif", 4))
                fmt = FMT_QOI;
            else if (got >= 12 && !memcmp(sig, "RIFF", 4) && !memcmp(sig + 8, "WEBP", 4))
                fmt = FMT_WEBP;
            else if (got >= 4 && (!memcmp(sig, "II*\0", 4) || !memcmp(sig, "MM\0*", 4)))
                fmt = FMT_TIFF;
            else {
                fclose(f);
                snprintf(result->err, sizeof(result->err),
                         "file has a .jxl name but is not a JXL file");
                return -1;
            }
            rewind(f);
        }
    }

    if (fmt == FMT_JXL) {
        img_image_t img;
        int is_anim = 0;
        rc = jxl_decode_file(f, &img, &is_anim, result->err, sizeof(result->err));
        fclose(f);
        if (rc != 0)
            return -1;
        if (opts->auto_optimize)
            img_auto_optimize(&img);
        result->out_w = img.width;
        result->out_h = img.height;
        result->out_depth = img_nominal_depth(&img);
        result->out_color = img.color;
        result->out_frames = 1;
        rc = jxl_write_file(&img, opts, out_path, result->err, sizeof(result->err));
        img_free(&img);
        if (rc != 0)
            return -1;
        return finish_ok(result, out_path, have_times, ctime, mtime, t0, in_size);
    }

    /* ---------------- everything else: decode pixels ---------------- */
    img_image_t img;
    if (fmt == FMT_PNG) {
        /* verify the magic first: a ".png"-named file may actually be some
         * other format; sniff and convert that instead when recognizable */
        static const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        uint8_t sig[8] = {0};
        size_t got = fread(sig, 1, 8, f);
        rewind(f);
        if (got < 8 || memcmp(sig, png_sig, 8) != 0) {
            if (sig[0] == 0xFF && sig[1] == 0xD8)
                fmt = FMT_JPEG;
            else if (sig[0] == 'B' && sig[1] == 'M')
                fmt = FMT_BMP;
            else if (sig[0] == 0 && sig[1] == 0 && sig[2] == 1 && sig[3] == 0)
                fmt = FMT_ICO;
            else if (sig[0] == 'P' && sig[1] >= '1' && sig[1] <= '6')
                fmt = FMT_PNM;
            else {
                fclose(f);
                snprintf(result->err, sizeof(result->err),
                         "file has a .png name but is not a PNG (and no other known format detected)");
                return -1;
            }
        }
    }

    switch (fmt) {
    case FMT_BMP:  rc = bmp_decode(f, &img, result->err, sizeof(result->err)); break;
    case FMT_TGA:  rc = tga_decode(f, &img, result->err, sizeof(result->err)); break;
    case FMT_PNM:  rc = pnm_decode(f, &img, result->err, sizeof(result->err)); break;
    case FMT_ICO:  rc = ico_decode(f, &img, result->err, sizeof(result->err)); break;
    case FMT_JPEG: rc = jpeg_decode(f, &img, result->err, sizeof(result->err)); break;
    case FMT_GIF:  rc = gif_decode(f, &img, result->err, sizeof(result->err)); break;
    case FMT_QOI:  rc = qoi_decode_file(f, &img, result->err, sizeof(result->err)); break;
    case FMT_WEBP: rc = webp_decode(f, &img, result->err, sizeof(result->err)); break;
    case FMT_TIFF: rc = tiff_decode(f, &img, result->err, sizeof(result->err)); break;
    case FMT_PNG: {
        uint8_t *buf = NULL;
        size_t buflen = 0;
        if (read_whole(f, &buf, &buflen, result->err, sizeof(result->err)) != 0) {
            fclose(f);
            return -1;
        }
        rc = png_decode_mem(buf, buflen, &img, result->err, sizeof(result->err));
        free(buf);
        break;
    }
    default:
        snprintf(result->err, sizeof(result->err), "unsupported input format");
        rc = -1;
        break;
    }
    fclose(f);
    if (rc != 0)
        return -1;

    if (opts->auto_optimize)
        img_auto_optimize(&img);

    result->out_w = img.width;
    result->out_h = img.height;
    result->out_depth = img_nominal_depth(&img);
    result->out_color = img.color;
    result->out_frames = 1;

    rc = jxl_write_file(&img, opts, out_path, result->err, sizeof(result->err));
    img_free(&img);
    if (rc != 0)
        return -1;

    return finish_ok(result, out_path, have_times, ctime, mtime, t0, in_size);
}
