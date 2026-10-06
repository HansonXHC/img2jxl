#include "decode.h"
#include <stdlib.h>
#include <string.h>
#include <jxl/decode.h>
#include <jxl/codestream_header.h>
#include <jxl/types.h>

static int fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
    return -1;
}

/* Decode a .jxl file into img_image_t (still images only).  Animated
 * inputs are rejected via is_animation so the caller can report them. */
int jxl_decode_file(FILE *f, img_image_t *img, int *is_animation,
                    char *err, size_t errlen)
{
    *is_animation = 0;

    long fsize = -1;
    if (fseek(f, 0, SEEK_END) != 0 || (fsize = ftell(f)) < 0)
        return fail(err, errlen, "jxl: cannot read input");
    rewind(f);
    size_t size = (size_t)fsize;
    uint8_t *buf = (uint8_t *)malloc(size ? size : 1);
    if (!buf || fread(buf, 1, size, f) != size) {
        free(buf);
        return fail(err, errlen, "jxl: cannot read input");
    }

    JxlDecoder *dec = JxlDecoderCreate(NULL);
    JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE);
    JxlDecoderSetInput(dec, buf, size);

    int rc = -1, channels = 0, sample_bits = 8, nominal = 8;
    uint8_t *pixels = NULL;
    size_t pixel_size = 0;
    uint32_t w = 0, h = 0;
    img_image_t out;
    memset(&out, 0, sizeof(out));

    for (;;) {
        JxlDecoderStatus st = JxlDecoderProcessInput(dec);
        if (st == JXL_DEC_ERROR) {
            fail(err, errlen, "jxl: not a valid JXL file");
            break;
        }
        if (st == JXL_DEC_NEED_MORE_INPUT) {
            fail(err, errlen, "jxl: truncated JXL file");
            break;
        }
        if (st == JXL_DEC_BASIC_INFO) {
            JxlBasicInfo info;
            JxlDecoderGetBasicInfo(dec, &info);
            if (info.have_animation) {
                *is_animation = 1;
                fail(err, errlen, "jxl: animated JXL input is not supported");
                break;
            }
            w = info.xsize;
            h = info.ysize;
            nominal = (int)info.bits_per_sample;
            sample_bits = nominal <= 8 ? 8 : 16;
            int alpha = info.num_extra_channels > 0;
            channels = (info.num_color_channels == 1 ? 1 : 3) + (alpha ? 1 : 0);

            JxlPixelFormat pf;
            memset(&pf, 0, sizeof(pf));
            pf.num_channels = (uint32_t)channels;
            pf.data_type = sample_bits == 16 ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8;
            pf.endianness = JXL_NATIVE_ENDIAN;
            if (JxlDecoderImageOutBufferSize(dec, &pf, &pixel_size) != JXL_DEC_SUCCESS) {
                fail(err, errlen, "jxl: cannot size output buffer");
                break;
            }
            pixels = (uint8_t *)malloc(pixel_size);
            if (!pixels) {
                fail(err, errlen, "jxl: out of memory");
                break;
            }
            if (JxlDecoderSetImageOutBuffer(dec, &pf, pixels, pixel_size)
                != JXL_DEC_SUCCESS) {
                fail(err, errlen, "jxl: cannot set output buffer");
                break;
            }
            /* take samples unscaled so a 10/12-bit source keeps its values
             * (libjxl's default would rescale them into 8/16-bit range) */
            {
                JxlBitDepth bd;
                bd.type = JXL_BIT_DEPTH_FROM_CODESTREAM;
                bd.bits_per_sample = (uint32_t)nominal;
                bd.exponent_bits_per_sample = 0;
                if (JxlDecoderSetImageOutBitDepth(dec, &bd) != JXL_DEC_SUCCESS) {
                    fail(err, errlen, "jxl: cannot set output bit depth");
                    break;
                }
            }
            continue;
        }
        if (st == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            fail(err, errlen, "jxl: decoder state error");
            break;
        }
        if (st == JXL_DEC_FULL_IMAGE) {
            /* convert the native-endian interleaved buffer into our
             * PNG-style representation (16-bit rows big-endian) */
            memset(&out, 0, sizeof(out));
            out.width = (int)w;
            out.height = (int)h;
            out.bit_depth = sample_bits;
            out.nominal_depth = (nominal != sample_bits) ? nominal : 0;
            switch (channels) {
            case 1: out.color = IMG_GRAY; break;
            case 2: out.color = IMG_GRAY_ALPHA; break;
            case 3: out.color = IMG_RGB; break;
            case 4: out.color = IMG_RGBA; break;
            default: fail(err, errlen, "jxl: unsupported channel count"); goto done;
            }
            out.rowstride = img_rowstride(out.width, out.bit_depth, channels);
            out.data = (uint8_t *)malloc((size_t)out.height * out.rowstride);
            if (!out.data) {
                fail(err, errlen, "jxl: out of memory");
                break;
            }
            if (sample_bits == 16) {
                for (int y = 0; y < out.height; y++) {
                    const uint16_t *s = (const uint16_t *)(pixels + (size_t)y * out.width * 2 * channels);
                    uint8_t *d = out.data + (size_t)y * out.rowstride;
                    for (int x = 0; x < out.width * channels; x++)
                        img_st16be(d + (size_t)x * 2, s[x]);
                }
            } else {
                for (int y = 0; y < out.height; y++)
                    memcpy(out.data + (size_t)y * out.rowstride,
                           pixels + (size_t)y * out.width * channels,
                           (size_t)out.width * channels);
            }
            continue;
        }
        if (st == JXL_DEC_SUCCESS) {
            if (!out.data) {
                fail(err, errlen, "jxl: no image found");
                break;
            }
            memcpy(img, &out, sizeof(out));
            rc = 0;
            break;
        }
    }

done:
    JxlDecoderDestroy(dec);
    free(pixels);
    free(buf);
    if (rc != 0)
        img_free(&out);
    return rc;
}

int jxl_reconstruct_jpeg(FILE *f, uint8_t **out, size_t *outlen,
                         char *err, size_t errlen)
{
    *out = NULL;
    *outlen = 0;

    long fsize = -1;
    if (fseek(f, 0, SEEK_END) != 0 || (fsize = ftell(f)) < 0)
        return fail(err, errlen, "jxl: cannot read input");
    rewind(f);
    size_t size = (size_t)fsize;
    uint8_t *buf = (uint8_t *)malloc(size ? size : 1);
    if (!buf || fread(buf, 1, size, f) != size) {
        free(buf);
        return fail(err, errlen, "jxl: cannot read input");
    }

    int rc = -1;
    JxlDecoder *dec = JxlDecoderCreate(NULL);
    /* FULL_IMAGE must be subscribed too: the reconstructed JPEG bytes are
     * written to the JPEG buffer during the frame-decoding stage */
    JxlDecoderSubscribeEvents(dec, JXL_DEC_JPEG_RECONSTRUCTION | JXL_DEC_FULL_IMAGE);
    JxlDecoderSetInput(dec, buf, size);

    size_t cap = 1 << 16;
    size_t used = 0;        /* bytes of *jpeg written so far */
    size_t buf_size = 0;    /* capacity handed to the last SetJPEGBuffer */
    int have_buf = 0;
    uint8_t *jpeg = (uint8_t *)malloc(cap);
    if (!jpeg) {
        fail(err, errlen, "jxl: out of memory");
        goto done2;
    }

    for (;;) {
        JxlDecoderStatus st = JxlDecoderProcessInput(dec);
        /* account for everything the decoder wrote since the last call */
        if (have_buf) {
            size_t remaining = JxlDecoderReleaseJPEGBuffer(dec);
            used += buf_size - remaining;
            have_buf = 0;
        }
        if (st == JXL_DEC_JPEG_RECONSTRUCTION) {
            buf_size = cap - used;
            if (JxlDecoderSetJPEGBuffer(dec, jpeg + used, buf_size) != JXL_DEC_SUCCESS) {
                fail(err, errlen, "jxl: cannot set JPEG buffer");
                break;
            }
            have_buf = 1;
            continue;
        }
        if (st == JXL_DEC_JPEG_NEED_MORE_OUTPUT) {
            if (used == cap) {
                cap *= 2;
                uint8_t *grown = (uint8_t *)realloc(jpeg, cap);
                if (!grown) {
                    fail(err, errlen, "jxl: out of memory");
                    break;
                }
                jpeg = grown;
            }
            buf_size = cap - used;
            if (JxlDecoderSetJPEGBuffer(dec, jpeg + used, buf_size) != JXL_DEC_SUCCESS) {
                fail(err, errlen, "jxl: cannot set JPEG buffer");
                break;
            }
            have_buf = 1;
            continue;
        }
        if (st == JXL_DEC_FULL_IMAGE) {
            /* only reached when no JPEG buffer is set: pixels were written
             * to no buffer of ours, which cannot happen here */
            continue;
        }
        if (st == JXL_DEC_ERROR) {
            fail(err, errlen, "jxl: not a JPEG-recompressed JXL file");
            break;
        }
        if (st == JXL_DEC_SUCCESS) {
            *out = jpeg;
            *outlen = used;
            rc = 0;
            break;
        }
    }

done2:
    if (rc != 0)
        free(jpeg);
    JxlDecoderDestroy(dec);
    free(buf);
    return rc;
}
