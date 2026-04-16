/*
 * escpos.h — Fluent builder for ESC/POS thermal-printer commands.
 *
 * Direct port of Helpers/EscPosBuilder.cs. Produces the same byte sequences.
 * Usage:
 *     escpos_t *b = escpos_new();
 *     escpos_initialize(b);
 *     escpos_align(b, ALIGN_CENTER);
 *     escpos_bold(b, true);
 *     escpos_line(b, "HELLO");
 *     escpos_feed_and_cut(b, 3);
 *     size_t n;
 *     uint8_t *data = escpos_build(b, &n);
 *     // send data to printer...
 *     free(data);
 *     escpos_free(b);
 */
#ifndef PRINTER_ESCPOS_H
#define PRINTER_ESCPOS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "../types.h"

typedef struct escpos_s escpos_t;

/* Lifecycle */
escpos_t *escpos_new(void);
void      escpos_free(escpos_t *b);

/* Printer control */
void escpos_initialize(escpos_t *b);
void escpos_reset(escpos_t *b);

/* Text output */
void escpos_text(escpos_t *b, const char *text);
void escpos_line(escpos_t *b, const char *text);
void escpos_separator(escpos_t *b, int width, char ch);
void escpos_left_right(escpos_t *b, const char *left, const char *right, int width);
void escpos_centered_text(escpos_t *b, const char *text, int width);

/* Alignment */
void escpos_align(escpos_t *b, escpos_alignment_t a);

/* Font style */
void escpos_bold(escpos_t *b, bool on);
void escpos_underline(escpos_t *b, bool on);
void escpos_double_size(escpos_t *b, bool width, bool height);
void escpos_normal_size(escpos_t *b);
void escpos_font(escpos_t *b, uint8_t font);   /* 0=Font A, 1=Font B */

/* Feed & cut */
void escpos_feed_lines(escpos_t *b, int lines);
void escpos_full_cut(escpos_t *b);
void escpos_partial_cut(escpos_t *b);
void escpos_feed_and_cut(escpos_t *b, int feed_lines);

/* Buzzer */
void escpos_beep(escpos_t *b, int count, int duration);
void escpos_bel(escpos_t *b);

/* Cash drawer (kick pin 2) */
void escpos_open_cash_drawer(escpos_t *b);

/* Barcode / QR */
void escpos_barcode128(escpos_t *b, const char *data, uint8_t height, uint8_t width);
void escpos_qr(escpos_t *b, const char *data, uint8_t module_size, uint8_t error_correction);

/* Raw passthrough */
void escpos_raw(escpos_t *b, const uint8_t *bytes, size_t len);

/* Build: returns a malloc'd byte buffer + length. Caller frees. */
uint8_t *escpos_build(escpos_t *b, size_t *out_len);

#endif /* PRINTER_ESCPOS_H */
