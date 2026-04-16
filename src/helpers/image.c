/*
 * image.c — Image → ESC/POS raster conversion.
 *
 * Direct port of Helpers/EscPosImageHelper.cs. See image.h for the pipeline.
 *
 * Uses stb_image for decode and a compact bilinear resizer (we don't need
 * stb_image_resize for POS-quality output).
 */
#include "image.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO                       /* we give it memory buffers */
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "../../vendor/stb_image.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── Utilities ────────────────────────────────────────────────────── */

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

int image_pixel_width_for(int paper_char_width) {
    return paper_char_width <= 32 ? PRINTER_PIX_WIDTH_58MM : PRINTER_PIX_WIDTH_80MM;
}

/* ── Resize with white-background alpha flatten (bilinear) ────────── */

/*
 * Input:  src = RGBA8888, sw x sh
 * Output: dst = RGB8  (alpha flattened onto white), dw x dh
 */
static void flatten_and_resize(const uint8_t *src, int sw, int sh,
                               uint8_t *dst, int dw, int dh) {
    /* Bilinear sampling. Pre-multiply alpha onto white background so PNG
     * transparency prints as paper color, not black. */
    for (int y = 0; y < dh; y++) {
        float fy = (sh > 1) ? ((float)y * (sh - 1) / (dh - 1)) : 0.0f;
        int y0 = (int)fy;
        int y1 = y0 + 1 < sh ? y0 + 1 : y0;
        float dy = fy - y0;

        for (int x = 0; x < dw; x++) {
            float fx = (sw > 1) ? ((float)x * (sw - 1) / (dw - 1)) : 0.0f;
            int x0 = (int)fx;
            int x1 = x0 + 1 < sw ? x0 + 1 : x0;
            float dx = fx - x0;

            const uint8_t *p00 = src + (y0 * sw + x0) * 4;
            const uint8_t *p10 = src + (y0 * sw + x1) * 4;
            const uint8_t *p01 = src + (y1 * sw + x0) * 4;
            const uint8_t *p11 = src + (y1 * sw + x1) * 4;

            float w00 = (1 - dx) * (1 - dy);
            float w10 = dx * (1 - dy);
            float w01 = (1 - dx) * dy;
            float w11 = dx * dy;

            for (int ch = 0; ch < 3; ch++) {
                /* Compose over white: out = src.rgb * a + 255 * (1 - a), all in [0..255]. */
                float c = 0.0f;

                float cc = p00[ch], a = p00[3] / 255.0f;
                c += w00 * (cc * a + 255.0f * (1.0f - a));

                cc = p10[ch]; a = p10[3] / 255.0f;
                c += w10 * (cc * a + 255.0f * (1.0f - a));

                cc = p01[ch]; a = p01[3] / 255.0f;
                c += w01 * (cc * a + 255.0f * (1.0f - a));

                cc = p11[ch]; a = p11[3] / 255.0f;
                c += w11 * (cc * a + 255.0f * (1.0f - a));

                dst[(y * dw + x) * 3 + ch] = (uint8_t)clampf(c, 0.0f, 255.0f);
            }
        }
    }
}

/* ── Grayscale conversion (BT.601 weights) ────────────────────────── */

static void to_grayscale(const uint8_t *rgb, int w, int h, float *out) {
    for (int i = 0, n = w * h; i < n; i++) {
        const uint8_t *p = rgb + i * 3;
        out[i] = p[0] * 0.299f + p[1] * 0.587f + p[2] * 0.114f;
    }
}

/* ── Dithering algorithms ─────────────────────────────────────────── */

/* Floyd–Steinberg with serpentine scanning. Writes a row-major bool array. */
static void floyd_steinberg(const float *gray_in, int w, int h, uint8_t *bits) {
    float *px = (float *)malloc(sizeof(float) * w * h);
    if (!px) return;
    memcpy(px, gray_in, sizeof(float) * w * h);

    for (int y = 0; y < h; y++) {
        int left_to_right = (y % 2 == 0);
        int start_x = left_to_right ? 0 : w - 1;
        int end_x   = left_to_right ? w : -1;
        int step    = left_to_right ? 1 : -1;

        for (int x = start_x; x != end_x; x += step) {
            float old_p = px[y * w + x];
            float new_p = old_p < 128.0f ? 0.0f : 255.0f;
            bits[y * w + x] = (new_p == 0.0f) ? 1 : 0;   /* 1 = black dot */
            float err = old_p - new_p;

            if (x + step >= 0 && x + step < w)
                px[y * w + (x + step)] += err * 7.0f / 16.0f;

            if (y + 1 < h) {
                if (x - step >= 0 && x - step < w)
                    px[(y + 1) * w + (x - step)] += err * 3.0f / 16.0f;
                px[(y + 1) * w + x] += err * 5.0f / 16.0f;
                if (x + step >= 0 && x + step < w)
                    px[(y + 1) * w + (x + step)] += err * 1.0f / 16.0f;
            }
        }
    }
    free(px);
}

/* Atkinson dither. Diffuses only 6/8 of error → higher contrast. */
static void atkinson(const float *gray_in, int w, int h, uint8_t *bits) {
    float *px = (float *)malloc(sizeof(float) * w * h);
    if (!px) return;
    memcpy(px, gray_in, sizeof(float) * w * h);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float old_p = px[y * w + x];
            float new_p = old_p < 128.0f ? 0.0f : 255.0f;
            bits[y * w + x] = (new_p == 0.0f) ? 1 : 0;
            float e = (old_p - new_p) / 8.0f;

            #define ADD_ERR(px_, py_, err_) do {                   \
                if ((px_) >= 0 && (px_) < w &&                     \
                    (py_) >= 0 && (py_) < h)                       \
                    px[(py_) * w + (px_)] += (err_);                \
            } while (0)
            ADD_ERR(x + 1, y,     e);
            ADD_ERR(x + 2, y,     e);
            ADD_ERR(x - 1, y + 1, e);
            ADD_ERR(x,     y + 1, e);
            ADD_ERR(x + 1, y + 1, e);
            ADD_ERR(x,     y + 2, e);
            #undef ADD_ERR
        }
    }
    free(px);
}

/* Simple threshold. */
static void threshold_convert(const float *gray, int w, int h, int thr, uint8_t *bits) {
    for (int i = 0, n = w * h; i < n; i++)
        bits[i] = gray[i] < thr ? 1 : 0;
}

/* ── ESC/POS GS v 0 packer with 256-row band splitting ────────────── */

static uint8_t *pack_to_raster(const uint8_t *bits, int w, int h, size_t *out_len) {
    const int MAX_BAND = 256;
    int byte_w = (w + 7) / 8;
    int num_bands = (h + MAX_BAND - 1) / MAX_BAND;
    size_t cap = (size_t)byte_w * h + (size_t)num_bands * 8 + 16;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) return NULL;
    size_t len = 0;

    for (int band_start = 0; band_start < h; band_start += MAX_BAND) {
        int band_h = (h - band_start) < MAX_BAND ? (h - band_start) : MAX_BAND;

        /* GS v 0 header */
        buf[len++] = 0x1D;
        buf[len++] = 0x76;
        buf[len++] = 0x30;
        buf[len++] = 0x00;
        buf[len++] = (uint8_t)(byte_w & 0xFF);
        buf[len++] = (uint8_t)((byte_w >> 8) & 0xFF);
        buf[len++] = (uint8_t)(band_h & 0xFF);
        buf[len++] = (uint8_t)((band_h >> 8) & 0xFF);

        for (int y = band_start; y < band_start + band_h; y++) {
            for (int x = 0; x < byte_w; x++) {
                uint8_t b = 0;
                for (int bit = 0; bit < 8; bit++) {
                    int px = x * 8 + bit;
                    if (px < w && bits[y * w + px])
                        b |= (uint8_t)(0x80 >> bit);
                }
                buf[len++] = b;
            }
        }
    }

    *out_len = len;
    return buf;
}

/* ── Main entry point ─────────────────────────────────────────────── */

uint8_t *image_to_escpos_raster(const uint8_t *image_data, size_t image_len,
                                int max_width_px,
                                int brightness, double gamma,
                                const char *dithering, int threshold,
                                size_t *out_len,
                                char *err_buf, size_t err_len) {
    if (out_len) *out_len = 0;

    if (!image_data || image_len == 0) {
        if (err_buf) snprintf(err_buf, err_len, "No image data provided.");
        return NULL;
    }

    int sw = 0, sh = 0, ch = 0;
    uint8_t *rgba = stbi_load_from_memory(image_data, (int)image_len,
                                          &sw, &sh, &ch, 4);
    if (!rgba) {
        if (err_buf) snprintf(err_buf, err_len,
            "Image decode failed: %s. Make sure the file is a valid PNG, JPG, or BMP.",
            stbi_failure_reason());
        return NULL;
    }

    /* Resize: keep original width if already ≤ target, else scale. */
    int target_w = sw < max_width_px ? sw : max_width_px;
    int target_h = sh * target_w / (sw > 0 ? sw : 1);
    if (target_h < 1) target_h = 1;

    uint8_t *rgb = (uint8_t *)malloc((size_t)target_w * target_h * 3);
    if (!rgb) {
        stbi_image_free(rgba);
        if (err_buf) snprintf(err_buf, err_len, "Out of memory during resize.");
        return NULL;
    }
    flatten_and_resize(rgba, sw, sh, rgb, target_w, target_h);
    stbi_image_free(rgba);

    /* Grayscale */
    size_t npix = (size_t)target_w * target_h;
    float *gray = (float *)malloc(sizeof(float) * npix);
    if (!gray) {
        free(rgb);
        if (err_buf) snprintf(err_buf, err_len, "Out of memory during grayscale.");
        return NULL;
    }
    to_grayscale(rgb, target_w, target_h, gray);
    free(rgb);

    /* Brightness */
    brightness = clampi(brightness, 50, 200);
    if (brightness != 100) {
        float factor = brightness / 100.0f;
        for (size_t i = 0; i < npix; i++)
            gray[i] = clampf(gray[i] * factor, 0.0f, 255.0f);
    }

    /* Gamma */
    gamma = clampd(gamma, 0.5, 3.0);
    if (fabs(gamma - 1.0) > 0.01) {
        double inv = 1.0 / gamma;
        for (size_t i = 0; i < npix; i++)
            gray[i] = (float)(255.0 * pow(gray[i] / 255.0, inv));
    }

    /* Dither to 1-bit */
    uint8_t *bits = (uint8_t *)calloc(npix, 1);
    if (!bits) {
        free(gray);
        if (err_buf) snprintf(err_buf, err_len, "Out of memory during dither.");
        return NULL;
    }
    const char *algo = dithering ? dithering : "floyd-steinberg";
    if (strcasecmp(algo, "atkinson") == 0) {
        atkinson(gray, target_w, target_h, bits);
    } else if (strcasecmp(algo, "threshold") == 0) {
        threshold_convert(gray, target_w, target_h, threshold, bits);
    } else {
        floyd_steinberg(gray, target_w, target_h, bits);
    }
    free(gray);

    /* Pack */
    uint8_t *raster = pack_to_raster(bits, target_w, target_h, out_len);
    free(bits);
    if (!raster) {
        if (err_buf) snprintf(err_buf, err_len, "Out of memory during raster pack.");
        return NULL;
    }
    return raster;
}
