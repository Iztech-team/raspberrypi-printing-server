/*
 * routes/print.c — print operation endpoints.
 *
 * All handlers share the same shape: parse JSON (or multipart) → call
 * print_service → serialize print_response_t. The JSON shape matches the
 * Windows PrintResponse exactly.
 */
#include "print.h"
#include "../services/print_service.h"
#include "../services/printer_config.h"
#include "../helpers/response.h"
#include "../../vendor/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char *mg_str_to_cstr(struct mg_str s) {
    char *out = (char *)malloc(s.len + 1);
    if (!out) return NULL;
    memcpy(out, s.buf, s.len);
    out[s.len] = '\0';
    return out;
}

static const char *json_str_or(cJSON *obj, const char *key, const char *fallback) {
    cJSON *v = cJSON_GetObjectItem(obj, key);
    const char *s = cJSON_GetStringValue(v);
    return (s && *s) ? s : fallback;
}

static int json_int_or(cJSON *obj, const char *key, int fallback) {
    cJSON *v = cJSON_GetObjectItem(obj, key);
    if (!v || !cJSON_IsNumber(v)) return fallback;
    return (int)v->valuedouble;
}

static double json_double_or(cJSON *obj, const char *key, double fallback) {
    cJSON *v = cJSON_GetObjectItem(obj, key);
    if (!v || !cJSON_IsNumber(v)) return fallback;
    return v->valuedouble;
}

static bool json_bool_or(cJSON *obj, const char *key, bool fallback) {
    cJSON *v = cJSON_GetObjectItem(obj, key);
    if (!v) return fallback;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? true : false;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    if (cJSON_IsString(v)) return strcasecmp(v->valuestring, "true") == 0;
    return fallback;
}

/* Convert a print_response_t into the standard JSON object + send. */
static void send_print_response(struct mg_connection *c, const print_response_t *r) {
    int status = r->success ? 200 : 400;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject  (o, "success", r->success);
    cJSON_AddStringToObject(o, "message", r->message);
    if (r->success) {
        cJSON_AddNullToObject(o, "error");
        cJSON_AddNullToObject(o, "errorCode");
    } else {
        cJSON_AddStringToObject(o, "error",     r->error);
        cJSON_AddStringToObject(o, "errorCode", r->error_code);
    }
    cJSON_AddNumberToObject(o, "elapsedMs", (double)r->elapsed_ms);
    send_json(c, status, o);
}

/* ── Base64 decoding (inline, no external dep) ────────────────────── */

static int b64val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static uint8_t *b64_decode(const char *in, size_t *out_len) {
    if (!in) return NULL;
    size_t n = strlen(in);
    uint8_t *buf = (uint8_t *)malloc(n + 4);
    if (!buf) return NULL;

    int acc = 0, bits = 0;
    size_t len = 0;
    for (size_t i = 0; i < n; i++) {
        int c = (unsigned char)in[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        int v = b64val(c);
        if (v < 0) { free(buf); return NULL; }
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            buf[len++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    *out_len = len;
    return buf;
}

/* ── Endpoints ────────────────────────────────────────────────────── */

void route_post_print_raw(struct mg_connection *c, struct mg_http_message *hm) {
    char *body = mg_str_to_cstr(hm->body);
    cJSON *req = body ? cJSON_Parse(body) : NULL;
    free(body);

    if (!req) {
        print_response_t r;
        memset(&r, 0, sizeof r);
        r.success = false;
        snprintf(r.message, sizeof r.message, "Print failed");
        snprintf(r.error,   sizeof r.error,   "Invalid JSON body.");
        snprintf(r.error_code, sizeof r.error_code, "INVALID_REQUEST");
        send_print_response(c, &r);
        return;
    }

    const char *printer = json_str_or(req, "printer", "");
    const char *b64     = json_str_or(req, "base64Data", NULL);

    /* rawBytes: JSON array of integers 0..255 (matches C# byte[] serialization). */
    uint8_t *data = NULL;
    size_t   len  = 0;

    cJSON *raw_arr = cJSON_GetObjectItem(req, "rawBytes");
    if (cJSON_IsArray(raw_arr)) {
        size_t n = (size_t)cJSON_GetArraySize(raw_arr);
        if (n > 0) {
            data = (uint8_t *)malloc(n);
            if (!data) {
                cJSON_Delete(req);
                print_response_t r;
                memset(&r, 0, sizeof r);
                r.success = false;
                snprintf(r.error, sizeof r.error, "Out of memory");
                snprintf(r.error_code, sizeof r.error_code, "SERVER_ERROR");
                send_print_response(c, &r);
                return;
            }
            for (size_t i = 0; i < n; i++) {
                cJSON *it = cJSON_GetArrayItem(raw_arr, (int)i);
                data[i] = (uint8_t)((cJSON_IsNumber(it) ? (int)it->valuedouble : 0) & 0xFF);
            }
            len = n;
        }
    }

    if (!data && b64 && *b64) {
        data = b64_decode(b64, &len);
        if (!data) {
            cJSON_Delete(req);
            print_response_t r;
            memset(&r, 0, sizeof r);
            r.success = false;
            snprintf(r.message, sizeof r.message, "Print failed");
            snprintf(r.error, sizeof r.error,
                     "Invalid Base64Data. Provide a valid base64-encoded string.");
            snprintf(r.error_code, sizeof r.error_code, "INVALID_DATA");
            send_print_response(c, &r);
            return;
        }
    }

    print_response_t r;
    print_service_raw(printer, data, len, &r);
    send_print_response(c, &r);

    free(data);
    cJSON_Delete(req);
}

void route_post_print_text(struct mg_connection *c, struct mg_http_message *hm) {
    char *body = mg_str_to_cstr(hm->body);
    cJSON *req = body ? cJSON_Parse(body) : NULL;
    free(body);

    const char *printer    = json_str_or   (req, "printer",   "");
    const char *text       = json_str_or   (req, "text",      "");
    int         paper_w_r  = json_int_or   (req, "paperWidth", -1);
    int         paper_w    = printer_config_paper_width(printer, paper_w_r, 48);
    bool        cut_paper  = json_bool_or  (req, "cutPaper",  true);
    int         feed_lines = json_int_or   (req, "feedLines", 3);

    print_response_t r;
    print_service_text(printer, text, paper_w, cut_paper, feed_lines, &r);
    send_print_response(c, &r);

    if (req) cJSON_Delete(req);
}

void route_post_print_test(struct mg_connection *c, struct mg_http_message *hm) {
    char *body = mg_str_to_cstr(hm->body);
    cJSON *req = body ? cJSON_Parse(body) : NULL;
    free(body);

    const char *printer = json_str_or(req, "printer",   "");
    int         paper_w_r = json_int_or(req, "paperWidth", -1);
    int         paper_w = printer_config_paper_width(printer, paper_w_r, 48);

    print_response_t r;
    print_service_test(printer, paper_w, &r);
    send_print_response(c, &r);

    if (req) cJSON_Delete(req);
}

void route_post_print_beep(struct mg_connection *c, struct mg_http_message *hm) {
    char *body = mg_str_to_cstr(hm->body);
    cJSON *req = body ? cJSON_Parse(body) : NULL;
    free(body);

    const char *printer  = json_str_or(req, "printer",  "");
    int         count    = json_int_or(req, "count",    2);
    int         duration = json_int_or(req, "duration", 3);

    print_response_t r;
    print_service_beep(printer, count, duration, &r);
    send_print_response(c, &r);

    if (req) cJSON_Delete(req);
}

void route_post_print_cut(struct mg_connection *c, struct mg_http_message *hm) {
    char *body = mg_str_to_cstr(hm->body);
    cJSON *req = body ? cJSON_Parse(body) : NULL;
    free(body);

    const char *printer    = json_str_or(req, "printer",    "");
    const char *cut_type   = json_str_or(req, "cutType",    "full");
    int         feed_lines = json_int_or(req, "feedLines",  3);

    print_response_t r;
    print_service_cut(printer, cut_type, feed_lines, &r);
    send_print_response(c, &r);

    if (req) cJSON_Delete(req);
}

void route_post_print_open_drawer(struct mg_connection *c, struct mg_http_message *hm) {
    char *body = mg_str_to_cstr(hm->body);
    cJSON *req = body ? cJSON_Parse(body) : NULL;
    free(body);

    const char *printer = json_str_or(req, "printer", "");

    print_response_t r;
    print_service_open_drawer(printer, &r);
    send_print_response(c, &r);

    if (req) cJSON_Delete(req);
}

/* ── Multipart/form-data for /api/print/image ─────────────────────── */

/* Fetch a named field value from a multipart body. Returns pointer+len
 * into the body, or NULL if not found. */
static struct mg_str multipart_field(struct mg_http_message *hm, const char *name) {
    struct mg_http_part part;
    size_t ofs = 0;
    while ((ofs = mg_http_next_multipart(hm->body, ofs, &part)) > 0) {
        if (part.name.len == strlen(name) &&
            memcmp(part.name.buf, name, part.name.len) == 0) {
            return part.body;
        }
    }
    return mg_str_n(NULL, 0);
}

static struct mg_http_part multipart_file(struct mg_http_message *hm, const char *name) {
    struct mg_http_part part = {0};
    size_t ofs = 0;
    while ((ofs = mg_http_next_multipart(hm->body, ofs, &part)) > 0) {
        if (part.name.len == strlen(name) &&
            memcmp(part.name.buf, name, part.name.len) == 0 &&
            part.filename.len > 0) {
            return part;
        }
    }
    struct mg_http_part empty = {0};
    return empty;
}

/* Copy an mg_str into a bounded buffer. */
static void copy_str(char *dst, size_t dst_len, struct mg_str s) {
    size_t n = s.len < dst_len - 1 ? s.len : dst_len - 1;
    if (s.buf && n > 0) memcpy(dst, s.buf, n);
    dst[n] = '\0';
}

void route_post_print_image(struct mg_connection *c, struct mg_http_message *hm) {
    /* Must be multipart/form-data. */
    struct mg_str ct = mg_str("");
    struct mg_str *ct_ptr = mg_http_get_header(hm, "Content-Type");
    if (ct_ptr) ct = *ct_ptr;
    if (ct.len < 19 || strncasecmp(ct.buf, "multipart/form-data", 19) != 0) {
        print_response_t r;
        memset(&r, 0, sizeof r);
        r.success = false;
        snprintf(r.message, sizeof r.message, "Print failed");
        snprintf(r.error, sizeof r.error, "Request must be multipart/form-data.");
        snprintf(r.error_code, sizeof r.error_code, "INVALID_REQUEST");
        send_print_response(c, &r);
        return;
    }

    struct mg_http_part file = multipart_file(hm, "image");
    if (!file.body.len) {
        print_response_t r;
        memset(&r, 0, sizeof r);
        r.success = false;
        snprintf(r.message, sizeof r.message, "Print failed");
        snprintf(r.error, sizeof r.error, "No image file provided. Use form field name 'image'.");
        snprintf(r.error_code, sizeof r.error_code, "NO_DATA");
        send_print_response(c, &r);
        return;
    }

    char printer[256];
    copy_str(printer, sizeof printer, multipart_field(hm, "printer"));

    if (!printer[0]) {
        print_response_t r;
        memset(&r, 0, sizeof r);
        r.success = false;
        snprintf(r.message, sizeof r.message, "Print failed");
        snprintf(r.error, sizeof r.error, "Printer name is required in 'printer' form field.");
        snprintf(r.error_code, sizeof r.error_code, "PRINTER_NAME_MISSING");
        send_print_response(c, &r);
        return;
    }

    /* Numeric/bool form fields — resolve against per-printer config. */
    char buf[64];

    int paper_w_raw = -1;
    copy_str(buf, sizeof buf, multipart_field(hm, "paperWidth"));
    if (buf[0]) paper_w_raw = atoi(buf);
    int paper_w = printer_config_paper_width(printer, paper_w_raw, 48);

    bool cut_paper = true;
    copy_str(buf, sizeof buf, multipart_field(hm, "cutPaper"));
    if (buf[0]) cut_paper = (strcasecmp(buf, "false") != 0 && strcmp(buf, "0") != 0);

    int feed_lines = 3;
    copy_str(buf, sizeof buf, multipart_field(hm, "feedLines"));
    if (buf[0]) feed_lines = atoi(buf);

    int brightness_raw = -1;
    copy_str(buf, sizeof buf, multipart_field(hm, "brightness"));
    if (buf[0]) brightness_raw = atoi(buf);
    int brightness = printer_config_brightness(printer, brightness_raw, 130);

    double gamma_raw = -1.0;
    copy_str(buf, sizeof buf, multipart_field(hm, "gamma"));
    if (buf[0]) gamma_raw = atof(buf);
    double gamma = printer_config_gamma(printer, gamma_raw, 1.8);

    char dithering_raw[32] = {0};
    copy_str(dithering_raw, sizeof dithering_raw, multipart_field(hm, "dithering"));
    const char *dithering_val = printer_config_dithering(printer, dithering_raw[0] ? dithering_raw : NULL, "floyd-steinberg");
    char dithering[32];
    snprintf(dithering, sizeof dithering, "%s", dithering_val);

    int threshold_raw = -1;
    copy_str(buf, sizeof buf, multipart_field(hm, "threshold"));
    if (buf[0]) threshold_raw = atoi(buf);
    int threshold = printer_config_threshold(printer, threshold_raw, 128);

    print_response_t r;
    print_service_image(printer,
                        (const uint8_t *)file.body.buf, file.body.len,
                        paper_w, cut_paper, feed_lines,
                        brightness, gamma, dithering, threshold,
                        &r);
    send_print_response(c, &r);
}
