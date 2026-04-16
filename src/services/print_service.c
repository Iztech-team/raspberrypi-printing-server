/*
 * print_service.c — high-level print orchestration.
 *
 * Mirrors PrinterService.cs: validate printer exists, build ESC/POS bytes,
 * send via CUPS, populate a print_response_t with elapsed_ms + error_code.
 */
#include "print_service.h"
#include "cups_service.h"
#include "../helpers/escpos.h"
#include "../helpers/image.h"
#include "../helpers/response.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* Clear a print_response_t to "success=false, empty strings". */
static void rsp_init(print_response_t *r) {
    memset(r, 0, sizeof *r);
}

static void rsp_ok(print_response_t *r, const char *message, long elapsed_ms) {
    r->success = true;
    snprintf(r->message, sizeof r->message, "%s", message ? message : "");
    r->error[0] = r->error_code[0] = '\0';
    r->elapsed_ms = elapsed_ms;
}

static void rsp_fail(print_response_t *r, const char *error,
                     long elapsed_ms, const char *error_code) {
    r->success = false;
    snprintf(r->message, sizeof r->message, "Print failed");
    snprintf(r->error,   sizeof r->error,   "%s", error ? error : "");
    if (error_code && *error_code) {
        snprintf(r->error_code, sizeof r->error_code, "%s", error_code);
    } else {
        snprintf(r->error_code, sizeof r->error_code, "%s", classify_error(error));
    }
    r->elapsed_ms = elapsed_ms;
}

/* Validate printer name + existence; fills response on failure and returns false. */
static bool validate_printer(const char *name, print_response_t *r) {
    if (!name || !*name) {
        rsp_fail(r,
            "Printer name is required. Use GET /api/printers to see installed printers.",
            0, "PRINTER_NAME_MISSING");
        return false;
    }
    if (!cups_printer_exists(name)) {
        char msg[512];
        snprintf(msg, sizeof msg,
            "Printer '%s' not found in CUPS. "
            "Use GET /api/printers to see installed printers, "
            "or POST /api/printers/add to install a network printer.",
            name);
        rsp_fail(r, msg, 0, "PRINTER_NOT_FOUND");
        return false;
    }
    return true;
}

/* Core send: ship ESC/POS bytes through CUPS, timing around the call. */
static void send_and_fill(const char *printer,
                          const uint8_t *data, size_t len,
                          long t0,
                          const char *ok_fmt,
                          print_response_t *r) {
    char err[512] = {0};
    char err_code[64] = {0};
    int rc = cups_send_raw(printer, data, len, err, sizeof err, err_code, sizeof err_code);
    long dt = monotonic_ms() - t0;
    if (rc == 0) {
        char msg[512];
        snprintf(msg, sizeof msg, ok_fmt, (int)len, printer);
        rsp_ok(r, msg, dt);
    } else {
        rsp_fail(r, err, dt, err_code);
    }
}

/* ── PrintRaw ─────────────────────────────────────────────────────── */

void print_service_raw(const char *printer,
                       const uint8_t *data, size_t len,
                       print_response_t *out) {
    rsp_init(out);
    long t0 = monotonic_ms();

    if (!validate_printer(printer, out)) return;

    if (!data || len == 0) {
        rsp_fail(out,
            "No print data provided. Use 'base64Data' or 'rawBytes'.",
            monotonic_ms() - t0, "NO_DATA");
        return;
    }

    send_and_fill(printer, data, len, t0, "Printed %d bytes to '%s'.", out);
}

/* ── PrintText ────────────────────────────────────────────────────── */

void print_service_text(const char *printer,
                        const char *text,
                        int paper_width,
                        bool cut_paper,
                        int feed_lines,
                        print_response_t *out) {
    rsp_init(out);
    long t0 = monotonic_ms();

    if (!validate_printer(printer, out)) return;
    if (!text || !*text) {
        rsp_fail(out, "Text content is required.", monotonic_ms() - t0, "NO_DATA");
        return;
    }

    /* Replicate the C# "\\n" → "\n" substitution + trailing-newline rule. */
    size_t tl = strlen(text);
    char *expanded = (char *)malloc(tl + 2);
    if (!expanded) {
        rsp_fail(out, "Out of memory", monotonic_ms() - t0, "SERVER_ERROR");
        return;
    }
    size_t w = 0;
    for (size_t i = 0; i < tl; i++) {
        if (text[i] == '\\' && i + 1 < tl && text[i + 1] == 'n') {
            expanded[w++] = '\n';
            i++;
        } else {
            expanded[w++] = text[i];
        }
    }
    if (w == 0 || expanded[w - 1] != '\n') expanded[w++] = '\n';
    expanded[w] = '\0';

    escpos_t *b = escpos_new();
    escpos_initialize(b);
    escpos_align(b, ALIGN_LEFT);
    escpos_text(b, expanded);
    if (cut_paper) escpos_feed_and_cut(b, feed_lines);

    size_t dlen = 0;
    uint8_t *data = escpos_build(b, &dlen);
    escpos_free(b);
    free(expanded);

    send_and_fill(printer, data, dlen, t0, "Printed text (%d bytes) to '%s'.", out);
    free(data);
    (void)paper_width;  /* currently advisory — clients still pass it for future layout */
}

/* ── PrintTest ────────────────────────────────────────────────────── */

void print_service_test(const char *printer,
                        int paper_width,
                        print_response_t *out) {
    rsp_init(out);
    long t0 = monotonic_ms();

    if (!validate_printer(printer, out)) return;

    int w = paper_width > 0 ? paper_width : 48;

    time_t now = time(NULL);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    char ts[64];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm_local);

    char line_time[128], line_printer[128], line_width[128];
    snprintf(line_time,    sizeof line_time,    "Time:     %s", ts);
    snprintf(line_printer, sizeof line_printer, "Printer:  %s", printer);
    snprintf(line_width,   sizeof line_width,   "Width:    %d chars", w);

    escpos_t *b = escpos_new();
    escpos_initialize(b);
    escpos_align(b, ALIGN_CENTER);
    escpos_bold(b, true);
    escpos_double_size(b, true, true);
    escpos_line(b, "PRINT TEST");
    escpos_normal_size(b);
    escpos_bold(b, false);
    escpos_separator(b, w, '=');

    escpos_align(b, ALIGN_LEFT);
    escpos_line(b, "Server:   Raspberry Pi Print Server");
    escpos_line(b, line_time);
    escpos_line(b, line_printer);
    escpos_line(b, line_width);
    escpos_separator(b, w, '-');
    escpos_line(b, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    escpos_line(b, "abcdefghijklmnopqrstuvwxyz");
    escpos_line(b, "0123456789 !@#$%^&*()");
    escpos_separator(b, w, '-');
    escpos_bold(b, true);
    escpos_line(b, "This text is BOLD");
    escpos_bold(b, false);
    escpos_underline(b, true);
    escpos_line(b, "This text is UNDERLINED");
    escpos_underline(b, false);
    escpos_separator(b, w, '-');
    escpos_left_right(b, "Item 1", "$10.00", w);
    escpos_left_right(b, "Item 2", "$25.50", w);
    escpos_left_right(b, "Item 3", "$7.99", w);
    escpos_separator(b, w, '=');
    escpos_bold(b, true);
    escpos_left_right(b, "TOTAL", "$43.49", w);
    escpos_bold(b, false);
    escpos_separator(b, w, '=');
    escpos_align(b, ALIGN_CENTER);
    escpos_line(b, "");
    escpos_line(b, "** TEST COMPLETE **");
    escpos_line(b, "Printer is working correctly!");
    escpos_feed_and_cut(b, 4);

    size_t dlen = 0;
    uint8_t *data = escpos_build(b, &dlen);
    escpos_free(b);

    char err[512] = {0}, err_code[64] = {0};
    int rc = cups_send_raw(printer, data, dlen, err, sizeof err, err_code, sizeof err_code);
    long dt = monotonic_ms() - t0;

    if (rc == 0) {
        char msg[256];
        snprintf(msg, sizeof msg, "Test page printed to '%s'.", printer);
        rsp_ok(out, msg, dt);
    } else {
        rsp_fail(out, err, dt, err_code);
    }
    free(data);
}

/* ── Beep ─────────────────────────────────────────────────────────── */

void print_service_beep(const char *printer,
                        int count, int duration,
                        print_response_t *out) {
    rsp_init(out);
    long t0 = monotonic_ms();
    if (!validate_printer(printer, out)) return;

    escpos_t *b = escpos_new();
    escpos_beep(b, count, duration);
    size_t dlen = 0;
    uint8_t *data = escpos_build(b, &dlen);
    escpos_free(b);

    char err[512] = {0}, err_code[64] = {0};
    int rc = cups_send_raw(printer, data, dlen, err, sizeof err, err_code, sizeof err_code);
    long dt = monotonic_ms() - t0;

    if (rc == 0) {
        char msg[256];
        snprintf(msg, sizeof msg, "Beep sent to '%s' (%dx, %dms each).",
                 printer, count, duration * 100);
        rsp_ok(out, msg, dt);
    } else {
        rsp_fail(out, err, dt, err_code);
    }
    free(data);
}

/* ── Cut ──────────────────────────────────────────────────────────── */

void print_service_cut(const char *printer,
                       const char *cut_type,
                       int feed_lines,
                       print_response_t *out) {
    rsp_init(out);
    long t0 = monotonic_ms();
    if (!validate_printer(printer, out)) return;

    escpos_t *b = escpos_new();
    if (feed_lines > 0) escpos_feed_lines(b, feed_lines);
    const char *t = (cut_type && *cut_type) ? cut_type : "full";
    if (strcasecmp(t, "partial") == 0) escpos_partial_cut(b);
    else                               escpos_full_cut(b);

    size_t dlen = 0;
    uint8_t *data = escpos_build(b, &dlen);
    escpos_free(b);

    char err[512] = {0}, err_code[64] = {0};
    int rc = cups_send_raw(printer, data, dlen, err, sizeof err, err_code, sizeof err_code);
    long dt = monotonic_ms() - t0;

    if (rc == 0) {
        char msg[256];
        snprintf(msg, sizeof msg, "%s cut sent to '%s'.", t, printer);
        rsp_ok(out, msg, dt);
    } else {
        rsp_fail(out, err, dt, err_code);
    }
    free(data);
}

/* ── OpenDrawer ───────────────────────────────────────────────────── */

void print_service_open_drawer(const char *printer, print_response_t *out) {
    rsp_init(out);
    long t0 = monotonic_ms();
    if (!validate_printer(printer, out)) return;

    escpos_t *b = escpos_new();
    escpos_open_cash_drawer(b);
    size_t dlen = 0;
    uint8_t *data = escpos_build(b, &dlen);
    escpos_free(b);

    char err[512] = {0}, err_code[64] = {0};
    int rc = cups_send_raw(printer, data, dlen, err, sizeof err, err_code, sizeof err_code);
    long dt = monotonic_ms() - t0;

    if (rc == 0) {
        char msg[256];
        snprintf(msg, sizeof msg, "Cash drawer opened on '%s'.", printer);
        rsp_ok(out, msg, dt);
    } else {
        rsp_fail(out, err, dt, err_code);
    }
    free(data);
}

/* ── Image ────────────────────────────────────────────────────────── */

void print_service_image(const char *printer,
                         const uint8_t *image_data, size_t image_len,
                         int paper_width,
                         bool cut_paper, int feed_lines,
                         int brightness, double gamma,
                         const char *dithering, int threshold,
                         print_response_t *out) {
    rsp_init(out);
    long t0 = monotonic_ms();

    if (!validate_printer(printer, out)) return;
    if (!image_data || image_len == 0) {
        rsp_fail(out, "No image data provided.", monotonic_ms() - t0, "NO_DATA");
        return;
    }

    int pix_w = image_pixel_width_for(paper_width > 0 ? paper_width : 42);

    char img_err[512] = {0};
    size_t raster_len = 0;
    uint8_t *raster = image_to_escpos_raster(
        image_data, image_len,
        pix_w,
        brightness, gamma,
        dithering, threshold,
        &raster_len,
        img_err, sizeof img_err);

    if (!raster) {
        rsp_fail(out, img_err[0] ? img_err : "Image conversion failed",
                 monotonic_ms() - t0, "IMAGE_ERROR");
        return;
    }

    escpos_t *b = escpos_new();
    escpos_initialize(b);
    /* Reset left margin to 0 (GS L) and set print area to full width (GS W)
     * so raster images start at the left edge without clipping. */
    {
        uint8_t margin_cmd[] = { 0x1D, 0x4C, 0x00, 0x00 };  /* GS L = 0 */
        escpos_raw(b, margin_cmd, sizeof margin_cmd);
        uint8_t width_cmd[] = { 0x1D, 0x57, (uint8_t)(pix_w & 0xFF), (uint8_t)((pix_w >> 8) & 0xFF) };  /* GS W */
        escpos_raw(b, width_cmd, sizeof width_cmd);
    }
    escpos_align(b, ALIGN_LEFT);
    escpos_raw(b, raster, raster_len);
    escpos_line(b, "");
    if (cut_paper) {
        int feed = feed_lines > 6 ? feed_lines : 6;
        escpos_feed_and_cut(b, feed);
    }

    free(raster);

    size_t dlen = 0;
    uint8_t *data = escpos_build(b, &dlen);
    escpos_free(b);

    char err[512] = {0}, err_code[64] = {0};
    int rc = cups_send_raw(printer, data, dlen, err, sizeof err, err_code, sizeof err_code);
    long dt = monotonic_ms() - t0;

    if (rc == 0) {
        char msg[256];
        snprintf(msg, sizeof msg,
                 "Image printed to '%s' (%zu bytes, %s dithering).",
                 printer, dlen,
                 dithering ? dithering : "floyd-steinberg");
        rsp_ok(out, msg, dt);
    } else {
        rsp_fail(out, err, dt, err_code);
    }
    free(data);
}
