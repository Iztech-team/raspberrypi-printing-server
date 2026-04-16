/*
 * routes/discovery.c — PWA discovery + self-signed certificate download.
 *
 * Parity:
 *   GET  /api/discover              → same JSON shape as Windows
 *   GET  /api/certificate           → serves the DER-encoded .cer
 *   POST /api/certificate/regenerate → recreates cert/key/der
 */
#include "discovery.h"
#include "../services/cert_service.h"
#include "../helpers/response.h"
#include "../../vendor/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

/* GET /api/discover */
void route_get_discover(struct mg_connection *c, struct mg_http_message *hm,
                        const discovery_ctx_t *ctx) {
    (void)hm;

    char hostname[256] = {0};
    if (gethostname(hostname, sizeof hostname) != 0) snprintf(hostname, sizeof hostname, "raspberry-pi");

    char ips[16][64] = {{0}};
    size_t nip = cert_get_local_ips(ips, 16);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "service",      "RaspberryPiPrintServer");
    cJSON_AddStringToObject(root, "version",      "1.0");
    cJSON_AddStringToObject(root, "hostname",     hostname);

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < nip; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(ips[i]));
    cJSON_AddItemToObject(root, "ips", arr);

    cJSON_AddNumberToObject(root, "httpPort", ctx->http_port);
    if (ctx->https_enabled) cJSON_AddNumberToObject(root, "httpsPort", ctx->https_port);
    else                    cJSON_AddNullToObject  (root, "httpsPort");
    cJSON_AddBoolToObject(root, "httpsEnabled", ctx->https_enabled);

    send_json(c, 200, root);
}

/* GET /api/certificate */
void route_get_certificate(struct mg_connection *c, struct mg_http_message *hm) {
    const char *path = cert_get_public_cert_path();
    if (!path) {
        send_simple(c, 404, false,
            "No certificate available. HTTPS may be disabled.");
        return;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        send_simple(c, 404, false, "Certificate file not found.");
        return;
    }

    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0) {
        fclose(f);
        send_simple(c, 404, false, "Certificate file is empty.");
        return;
    }

    /* Android expects application/octet-stream, others x-x509-ca-cert. */
    struct mg_str *ua = mg_http_get_header(hm, "User-Agent");
    bool is_android = false;
    if (ua) {
        for (size_t i = 0; i + 7 <= ua->len; i++) {
            if (strncasecmp(ua->buf + i, "android", 7) == 0) { is_android = true; break; }
        }
    }

    const char *mime = is_android
        ? "application/octet-stream"
        : "application/x-x509-ca-cert";

    mg_printf(c,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Disposition: attachment; filename=\"printserver.cer\"\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "\r\n",
        mime, (long)st.st_size);

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        mg_send(c, buf, n);
    }
    fclose(f);
    c->is_resp = 0;
    c->is_draining = 1;
}

/* POST /api/certificate/regenerate */
void route_post_regenerate_certificate(struct mg_connection *c,
                                       struct mg_http_message *hm) {
    (void)hm;
    char err[512] = {0};
    bool ok = cert_regenerate(err, sizeof err);

    char msg[512];
    if (ok)
        snprintf(msg, sizeof msg, "Certificate regenerated. Restart the service to apply.");
    else
        snprintf(msg, sizeof msg, "Failed to regenerate certificate: %s",
                 err[0] ? err : "unknown error");

    send_simple(c, ok ? 200 : 400, ok, msg);
}
