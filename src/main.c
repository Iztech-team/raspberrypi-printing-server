/*
 * main.c — entry point for the Raspberry Pi thermal-printer server.
 *
 *   1. Load config from config.json (or environment) — port, httpsPort,
 *      enableHttps, wwwRoot.
 *   2. Ensure self-signed cert/key exist (generating on first run).
 *   3. Bind an HTTP listener on :5123 and (optionally) an HTTPS listener
 *      on :5124 that shares the same event handler.
 *   4. Run the mongoose event loop until SIGINT/SIGTERM.
 *
 * Signal handling drains connections gracefully and frees every dynamically
 * allocated resource at shutdown so valgrind stays quiet.
 */
#include "server.h"
#include "services/cert_service.h"
#include "services/printer_config.h"
#include "helpers/response.h"
#include "../vendor/cJSON.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t s_running = 1;
static void on_signal(int sig) { (void)sig; s_running = 0; }

/* ── Config loading ───────────────────────────────────────────────── */

static void load_config(const char *path, server_config_t *out) {
    /* Windows defaults. */
    out->http_port     = 5123;
    out->https_port    = 5124;
    out->https_enabled = true;
    snprintf(out->www_root, sizeof out->www_root, "www");

    FILE *f = fopen(path, "rb");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1 << 20) { fclose(f); return; }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    buf[nr] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    cJSON *v;
    if ((v = cJSON_GetObjectItem(root, "port"))         && cJSON_IsNumber(v)) out->http_port     = (int)v->valuedouble;
    if ((v = cJSON_GetObjectItem(root, "httpsPort"))    && cJSON_IsNumber(v)) out->https_port    = (int)v->valuedouble;
    if ((v = cJSON_GetObjectItem(root, "enableHttps"))  && cJSON_IsBool(v))   out->https_enabled = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(root, "wwwRoot"))      && cJSON_IsString(v)) snprintf(out->www_root, sizeof out->www_root, "%s", v->valuestring);

    cJSON_Delete(root);

    /* Environment overrides take highest priority. */
    const char *ep;
    if ((ep = getenv("PRINTER_SERVER_PORT")))       out->http_port    = atoi(ep);
    if ((ep = getenv("PRINTER_SERVER_HTTPS_PORT"))) out->https_port   = atoi(ep);
    if ((ep = getenv("PRINTER_SERVER_HTTPS"))) {
        out->https_enabled =
            (strcasecmp(ep, "true") == 0 || strcmp(ep, "1") == 0);
    }
    if ((ep = getenv("PRINTER_SERVER_WWW"))) snprintf(out->www_root, sizeof out->www_root, "%s", ep);
}

/* ── main ─────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    /* Ignore SIGPIPE — we handle broken pipes via return codes. */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    const char *config_path = "config.json";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) config_path = argv[++i];
        else if (strncmp(argv[i], "--config=", 9) == 0) config_path = argv[i] + 9;
    }

    server_config_t cfg;
    load_config(config_path, &cfg);
    g_config = &cfg;

    printf("[server] Raspberry Pi Print Server starting\n");
    printf("[server] HTTP  port: %d\n", cfg.http_port);
    printf("[server] HTTPS port: %d (%s)\n",
           cfg.https_port, cfg.https_enabled ? "enabled" : "disabled");
    printf("[server] www root : %s\n", cfg.www_root);
    printf("[server] config   : %s\n", config_path);

    /* ── Per-printer config ───────────────────────────────────── */
    printer_config_init();

    /* ── TLS setup ────────────────────────────────────────────── */
    struct mg_tls_opts tls_opts = {0};
    struct mg_str tls_cert_contents = {0};
    struct mg_str tls_key_contents  = {0};
    bool tls_ok = false;

    if (cfg.https_enabled) {
        char cert_path[512], key_path[512];
        if (cert_get_or_create(cert_path, sizeof cert_path,
                               key_path,  sizeof key_path)) {
            tls_cert_contents = mg_file_read(&mg_fs_posix, cert_path);
            tls_key_contents  = mg_file_read(&mg_fs_posix, key_path);
            if (tls_cert_contents.buf && tls_key_contents.buf) {
                tls_opts.cert = tls_cert_contents;
                tls_opts.key  = tls_key_contents;
                tls_ok = true;
                printf("[server] TLS cert: %s\n", cert_path);
                printf("[server] TLS key : %s\n", key_path);
            } else {
                fprintf(stderr, "[server] Could not read cert/key files — HTTPS disabled.\n");
            }
        } else {
            fprintf(stderr, "[server] Could not create cert/key — HTTPS disabled.\n");
        }
    }
    /* If TLS setup failed, reflect that in the discovery payload. */
    if (!tls_ok) cfg.https_enabled = false;

    /* ── Mongoose setup ───────────────────────────────────────── */
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    /* mongoose log level: 1 = error, 2 = info, 3 = debug */
    mg_log_set(2);

    char http_url[64];
    snprintf(http_url, sizeof http_url, "http://0.0.0.0:%d", cfg.http_port);
    if (!mg_http_listen(&mgr, http_url, server_event, NULL)) {
        fprintf(stderr, "[server] FATAL: cannot bind %s: %s\n",
                http_url, strerror(errno));
        mg_mgr_free(&mgr);
        return 1;
    }
    printf("[server] listening on %s\n", http_url);

    if (cfg.https_enabled) {
        char https_url[64];
        snprintf(https_url, sizeof https_url, "http://0.0.0.0:%d", cfg.https_port);
        /* Pass tls_opts via fn_data so server_event can enable TLS on accept. */
        if (!mg_http_listen(&mgr, https_url, server_event, &tls_opts)) {
            fprintf(stderr, "[server] WARNING: cannot bind HTTPS on %s: %s\n",
                    https_url, strerror(errno));
        } else {
            printf("[server] listening on https://0.0.0.0:%d (TLS)\n", cfg.https_port);
        }
    }

    /* ── Event loop ───────────────────────────────────────────── */
    while (s_running) {
        mg_mgr_poll(&mgr, 500);
    }

    printf("\n[server] shutting down...\n");
    mg_mgr_free(&mgr);
    free((void *)tls_cert_contents.buf);
    free((void *)tls_key_contents.buf);
    return 0;
}
