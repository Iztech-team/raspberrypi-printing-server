/*
 * server.c — mongoose event handler + route dispatch.
 *
 * Responsibilities:
 *   - Enable TLS on HTTPS listener connections (fn_data carries mg_tls_opts)
 *   - Handle CORS preflight (OPTIONS) with the same headers the Windows version sends
 *   - Dispatch HTTP method + URL to route handlers
 *   - Static-file fallback from www/
 *   - Graceful 500 on any unexpected error
 *
 * Route dispatch intentionally uses plain string matching rather than a
 * regex engine — our URL space is tiny and explicit. Path parameters for
 * /api/printers/{name}, /api/printers/clear/{name}, and /api/printers/status/{name}
 * are extracted by slicing the URI after the known prefix and URL-decoding.
 */
#include "server.h"
#include "routes/printers.h"
#include "routes/print.h"
#include "routes/discovery.h"
#include "helpers/response.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const server_config_t *g_config = NULL;

/* ── helpers ──────────────────────────────────────────────────────── */

static bool mg_eq(struct mg_str a, const char *lit) {
    size_t n = strlen(lit);
    return a.len == n && memcmp(a.buf, lit, n) == 0;
}

static bool mg_ieq(struct mg_str a, const char *lit) {
    size_t n = strlen(lit);
    if (a.len != n) return false;
    return strncasecmp(a.buf, lit, n) == 0;
}

/* URI starts-with helper that also tolerates a trailing segment. */
static bool uri_starts_with(struct mg_str uri, const char *prefix) {
    size_t n = strlen(prefix);
    return uri.len >= n && memcmp(uri.buf, prefix, n) == 0;
}

/* Extract + URL-decode the path-parameter segment after a fixed prefix.
 * Writes into `out`, null-terminated. */
static void extract_name(struct mg_str uri, const char *prefix,
                         char *out, size_t out_len) {
    size_t plen = strlen(prefix);
    if (uri.len < plen) { if (out_len) out[0] = '\0'; return; }
    const char *start = uri.buf + plen;
    size_t rem = uri.len - plen;
    /* Stop at next '/' or '?' if present. */
    size_t take = 0;
    while (take < rem && start[take] != '/' && start[take] != '?') take++;
    mg_url_decode(start, take, out, out_len, 0);
}

static void send_cors_preflight(struct mg_connection *c) {
    mg_printf(c,
        "HTTP/1.1 204 No Content\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, PATCH, OPTIONS\r\n"
        "Access-Control-Allow-Headers: *\r\n"
        "Access-Control-Allow-Private-Network: true\r\n"
        "Access-Control-Max-Age: 86400\r\n"
        "Content-Length: 0\r\n"
        "\r\n");
}

static void send_404(struct mg_connection *c) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject  (o, "success", false);
    cJSON_AddStringToObject(o, "message", "Not found");
    cJSON_AddStringToObject(o, "error",   "Unknown endpoint");
    cJSON_AddStringToObject(o, "errorCode","NOT_FOUND");
    cJSON_AddNumberToObject(o, "elapsedMs", 0);
    send_json(c, 404, o);
}

/* ── dispatch ─────────────────────────────────────────────────────── */

static bool dispatch_api(struct mg_connection *c, struct mg_http_message *hm) {
    struct mg_str method = hm->method;
    struct mg_str uri    = hm->uri;

    /* Strip query string if present. */
    size_t slen = 0;
    while (slen < uri.len && uri.buf[slen] != '?') slen++;
    uri.len = slen;

    /* ── /api/printers/* ────────────────────────────────────────── */
    if (mg_eq(method, "GET") && mg_eq(uri, "/api/printers")) {
        route_get_printers(c, hm); return true;
    }
    if (mg_eq(method, "GET") && mg_eq(uri, "/api/printers/drivers")) {
        route_get_drivers(c, hm); return true;
    }
    if (mg_eq(method, "POST") && mg_eq(uri, "/api/printers/add")) {
        route_post_add_printer(c, hm); return true;
    }
    if (mg_eq(method, "GET") && mg_eq(uri, "/api/printers/usb")) {
        route_get_usb(c, hm); return true;
    }
    if (mg_eq(method, "POST") && mg_eq(uri, "/api/printers/add-usb")) {
        route_post_add_usb(c, hm); return true;
    }
    if (mg_eq(method, "POST") && mg_eq(uri, "/api/printers/auto-discover")) {
        route_post_auto_discover(c, hm); return true;
    }
    if (mg_eq(method, "GET") && uri_starts_with(uri, "/api/printers/status/")) {
        char name[256];
        extract_name(uri, "/api/printers/status/", name, sizeof name);
        route_get_printer_status(c, hm, name); return true;
    }
    if (mg_eq(method, "POST") && uri_starts_with(uri, "/api/printers/clear/")) {
        char name[256];
        extract_name(uri, "/api/printers/clear/", name, sizeof name);
        route_post_clear_queue(c, hm, name); return true;
    }
    if (mg_eq(method, "GET") && uri_starts_with(uri, "/api/printers/config/")) {
        char name[256];
        extract_name(uri, "/api/printers/config/", name, sizeof name);
        route_get_printer_config(c, hm, name); return true;
    }
    if (mg_eq(method, "POST") && uri_starts_with(uri, "/api/printers/config/")) {
        char name[256];
        extract_name(uri, "/api/printers/config/", name, sizeof name);
        route_post_printer_config(c, hm, name); return true;
    }
    /* DELETE /api/printers/{name} — must come AFTER the other /api/printers/*
     * prefix matches so we don't accidentally swallow /drivers or /usb. */
    if (mg_eq(method, "DELETE") && uri_starts_with(uri, "/api/printers/")) {
        char name[256];
        extract_name(uri, "/api/printers/", name, sizeof name);
        route_delete_printer(c, hm, name); return true;
    }

    /* ── /api/print/* ───────────────────────────────────────────── */
    if (mg_eq(method, "POST") && mg_eq(uri, "/api/print/raw")) {
        route_post_print_raw(c, hm); return true;
    }
    if (mg_eq(method, "POST") && mg_eq(uri, "/api/print/text")) {
        route_post_print_text(c, hm); return true;
    }
    if (mg_eq(method, "POST") && mg_eq(uri, "/api/print/test")) {
        route_post_print_test(c, hm); return true;
    }
    if (mg_eq(method, "POST") && mg_eq(uri, "/api/print/beep")) {
        route_post_print_beep(c, hm); return true;
    }
    if (mg_eq(method, "POST") && mg_eq(uri, "/api/print/cut")) {
        route_post_print_cut(c, hm); return true;
    }
    if (mg_eq(method, "POST") && mg_eq(uri, "/api/print/open-drawer")) {
        route_post_print_open_drawer(c, hm); return true;
    }
    if (mg_eq(method, "POST") && mg_eq(uri, "/api/print/image")) {
        route_post_print_image(c, hm); return true;
    }

    /* ── /api/discover + /api/certificate ───────────────────────── */
    if (mg_eq(method, "GET") && mg_eq(uri, "/api/discover")) {
        discovery_ctx_t ctx = {
            .http_port     = g_config->http_port,
            .https_port    = g_config->https_port,
            .https_enabled = g_config->https_enabled
        };
        route_get_discover(c, hm, &ctx); return true;
    }
    if (mg_eq(method, "GET") && mg_eq(uri, "/api/certificate")) {
        route_get_certificate(c, hm); return true;
    }
    if (mg_eq(method, "POST") && mg_eq(uri, "/api/certificate/regenerate")) {
        route_post_regenerate_certificate(c, hm); return true;
    }

    return false;
}

/* Serve static files from www_root.  mongoose automatically serves
 * index.html when the URI is "/", so the dashboard loads at the root. */
static void serve_root_or_static(struct mg_connection *c,
                                 struct mg_http_message *hm) {
    struct mg_http_serve_opts opts = {0};
    opts.root_dir = g_config->www_root;
    opts.extra_headers = "Access-Control-Allow-Origin: *\r\n";
    mg_http_serve_dir(c, hm, &opts);
}

/* ── Public event handler ─────────────────────────────────────────── */

void server_event(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_ACCEPT && c->fn_data != NULL) {
        /* HTTPS listener — enable TLS with the opts passed via fn_data. */
        mg_tls_init(c, (struct mg_tls_opts *)c->fn_data);
        return;
    }

    if (ev != MG_EV_HTTP_MSG) return;
    struct mg_http_message *hm = (struct mg_http_message *)ev_data;

    /* CORS preflight for any OPTIONS request. */
    if (mg_ieq(hm->method, "OPTIONS")) {
        send_cors_preflight(c);
        return;
    }

    /* API dispatch. */
    if (dispatch_api(c, hm)) return;

    /* Not API: serve static or banner. */
    serve_root_or_static(c, hm);
}
