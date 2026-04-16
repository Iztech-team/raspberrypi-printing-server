/*
 * escpos.c — ESC/POS byte-stream builder.
 *
 * Direct port of Helpers/EscPosBuilder.cs. Byte sequences are identical.
 * Text is written UTF-8 directly (as the C# version does with Encoding.UTF8).
 */
#include "escpos.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct escpos_s {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
};

/* ── Buffer management ─────────────────────────────────────────────── */

static void ensure_cap(escpos_t *b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    size_t newcap = b->cap ? b->cap : 256;
    while (newcap < b->len + extra) newcap *= 2;
    uint8_t *nbuf = (uint8_t *)realloc(b->buf, newcap);
    if (!nbuf) {
        /* Allocation failure — leave buffer as-is. Build() returns a truncated
         * but still-valid buffer. This is a defensive path; in practice we
         * abort the program since this means we're out of memory. */
        fprintf(stderr, "escpos: out of memory\n");
        abort();
    }
    b->buf = nbuf;
    b->cap = newcap;
}

static void append_byte(escpos_t *b, uint8_t c) {
    ensure_cap(b, 1);
    b->buf[b->len++] = c;
}

static void append_bytes(escpos_t *b, const uint8_t *p, size_t n) {
    ensure_cap(b, n);
    memcpy(b->buf + b->len, p, n);
    b->len += n;
}

static void append_str(escpos_t *b, const char *s) {
    if (!s) return;
    size_t n = strlen(s);
    append_bytes(b, (const uint8_t *)s, n);
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

escpos_t *escpos_new(void) {
    escpos_t *b = (escpos_t *)calloc(1, sizeof(escpos_t));
    return b;
}

void escpos_free(escpos_t *b) {
    if (!b) return;
    free(b->buf);
    free(b);
}

/* ── Printer control ──────────────────────────────────────────────── */

void escpos_initialize(escpos_t *b) {
    static const uint8_t cmd[] = { 0x1B, 0x40 };  /* ESC @ */
    append_bytes(b, cmd, sizeof cmd);
}

void escpos_reset(escpos_t *b) {
    escpos_initialize(b);
}

/* ── Text output ──────────────────────────────────────────────────── */

void escpos_text(escpos_t *b, const char *text) {
    append_str(b, text);
}

void escpos_line(escpos_t *b, const char *text) {
    if (text) append_str(b, text);
    append_byte(b, '\n');
}

void escpos_separator(escpos_t *b, int width, char ch) {
    if (width < 1) width = 1;
    ensure_cap(b, (size_t)width + 1);
    for (int i = 0; i < width; i++) b->buf[b->len++] = (uint8_t)ch;
    b->buf[b->len++] = '\n';
}

void escpos_left_right(escpos_t *b, const char *left, const char *right, int width) {
    if (!left) left = "";
    if (!right) right = "";
    int ll = (int)strlen(left), rl = (int)strlen(right);
    int pad = width - ll - rl;
    if (pad < 1) pad = 1;
    append_str(b, left);
    for (int i = 0; i < pad; i++) append_byte(b, ' ');
    append_str(b, right);
    append_byte(b, '\n');
}

void escpos_centered_text(escpos_t *b, const char *text, int width) {
    if (!text) text = "";
    int tl = (int)strlen(text);
    int pad = (width - tl) / 2;
    if (pad < 0) pad = 0;
    for (int i = 0; i < pad; i++) append_byte(b, ' ');
    append_str(b, text);
    append_byte(b, '\n');
}

/* ── Alignment ────────────────────────────────────────────────────── */

void escpos_align(escpos_t *b, escpos_alignment_t a) {
    uint8_t cmd[] = { 0x1B, 0x61, (uint8_t)a };
    append_bytes(b, cmd, sizeof cmd);
}

/* ── Font style ───────────────────────────────────────────────────── */

void escpos_bold(escpos_t *b, bool on) {
    uint8_t cmd[] = { 0x1B, 0x45, (uint8_t)(on ? 1 : 0) };
    append_bytes(b, cmd, sizeof cmd);
}

void escpos_underline(escpos_t *b, bool on) {
    uint8_t cmd[] = { 0x1B, 0x2D, (uint8_t)(on ? 1 : 0) };
    append_bytes(b, cmd, sizeof cmd);
}

void escpos_double_size(escpos_t *b, bool width, bool height) {
    /* GS ! n: bits 4–7 = width multiplier, bits 0–3 = height multiplier. */
    uint8_t mode = 0;
    if (width)  mode |= 0x10;   /* (2 − 1) << 4 */
    if (height) mode |= 0x01;   /* (2 − 1) */
    uint8_t cmd[] = { 0x1D, 0x21, mode };
    append_bytes(b, cmd, sizeof cmd);
}

void escpos_normal_size(escpos_t *b) {
    uint8_t cmd[] = { 0x1D, 0x21, 0x00 };
    append_bytes(b, cmd, sizeof cmd);
}

void escpos_font(escpos_t *b, uint8_t font) {
    uint8_t cmd[] = { 0x1B, 0x4D, font };
    append_bytes(b, cmd, sizeof cmd);
}

/* ── Feed & cut ───────────────────────────────────────────────────── */

void escpos_feed_lines(escpos_t *b, int lines) {
    if (lines < 0) lines = 0;
    if (lines > 255) lines = 255;
    uint8_t cmd[] = { 0x1B, 0x64, (uint8_t)lines };
    append_bytes(b, cmd, sizeof cmd);
}

void escpos_full_cut(escpos_t *b) {
    static const uint8_t cmd[] = { 0x1D, 0x56, 0x00 };
    append_bytes(b, cmd, sizeof cmd);
}

void escpos_partial_cut(escpos_t *b) {
    static const uint8_t cmd[] = { 0x1D, 0x56, 0x01 };
    append_bytes(b, cmd, sizeof cmd);
}

void escpos_feed_and_cut(escpos_t *b, int feed_lines) {
    escpos_feed_lines(b, feed_lines);
    escpos_full_cut(b);
}

/* ── Buzzer / beep ────────────────────────────────────────────────── */

void escpos_beep(escpos_t *b, int count, int duration) {
    if (count < 1) count = 1;
    if (count > 9) count = 9;
    if (duration < 1) duration = 1;
    if (duration > 9) duration = 9;
    uint8_t cmd[] = { 0x1B, 0x42, (uint8_t)count, (uint8_t)duration };
    append_bytes(b, cmd, sizeof cmd);
}

void escpos_bel(escpos_t *b) {
    append_byte(b, 0x07);
}

/* ── Cash drawer ──────────────────────────────────────────────────── */

void escpos_open_cash_drawer(escpos_t *b) {
    /* ESC p m t1 t2 — pin 2, 25ms on, 250ms off */
    static const uint8_t cmd[] = { 0x1B, 0x70, 0x00, 0x19, 0xFA };
    append_bytes(b, cmd, sizeof cmd);
}

/* ── Barcode ─────────────────────────────────────────────────────── */

void escpos_barcode128(escpos_t *b, const char *data, uint8_t height, uint8_t width) {
    if (!data) return;
    size_t n = strlen(data);
    if (n == 0 || n > 255) return;

    uint8_t h[] = { 0x1D, 0x68, height };
    uint8_t w[] = { 0x1D, 0x77, width };
    uint8_t hri[] = { 0x1D, 0x48, 0x02 };       /* HRI below barcode */
    uint8_t start[] = { 0x1D, 0x6B, 0x49 };     /* Code128 prefix */

    append_bytes(b, h, sizeof h);
    append_bytes(b, w, sizeof w);
    append_bytes(b, hri, sizeof hri);
    append_bytes(b, start, sizeof start);
    append_byte(b, (uint8_t)n);
    append_bytes(b, (const uint8_t *)data, n);
}

/* ── QR Code ─────────────────────────────────────────────────────── */

void escpos_qr(escpos_t *b, const char *data, uint8_t module_size, uint8_t error_correction) {
    if (!data) return;
    size_t n = strlen(data);
    size_t store_len = n + 3;

    /* Model 2 */
    uint8_t model[]    = { 0x1D, 0x28, 0x6B, 0x04, 0x00, 0x31, 0x41, 0x32, 0x00 };
    uint8_t size_cmd[] = { 0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x43, module_size };
    uint8_t ec[]       = { 0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x45, error_correction };
    uint8_t store[]    = { 0x1D, 0x28, 0x6B,
                          (uint8_t)(store_len & 0xFF),
                          (uint8_t)((store_len >> 8) & 0xFF),
                          0x31, 0x50, 0x30 };
    uint8_t print[]    = { 0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x51, 0x30 };

    append_bytes(b, model, sizeof model);
    append_bytes(b, size_cmd, sizeof size_cmd);
    append_bytes(b, ec, sizeof ec);
    append_bytes(b, store, sizeof store);
    append_bytes(b, (const uint8_t *)data, n);
    append_bytes(b, print, sizeof print);
}

/* ── Raw passthrough ─────────────────────────────────────────────── */

void escpos_raw(escpos_t *b, const uint8_t *bytes, size_t len) {
    if (!bytes || len == 0) return;
    append_bytes(b, bytes, len);
}

/* ── Build ────────────────────────────────────────────────────────── */

uint8_t *escpos_build(escpos_t *b, size_t *out_len) {
    if (!b) { if (out_len) *out_len = 0; return NULL; }
    uint8_t *copy = (uint8_t *)malloc(b->len > 0 ? b->len : 1);
    if (copy && b->len > 0) memcpy(copy, b->buf, b->len);
    if (out_len) *out_len = b->len;
    return copy;
}
