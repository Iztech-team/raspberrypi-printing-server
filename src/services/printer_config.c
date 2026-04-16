/*
 * printer_config.c — Per-printer configuration storage.
 *
 * Thread-safety: mongoose is single-threaded, so no mutex is needed.
 */
#include "printer_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static cJSON *s_root = NULL;            /* top-level JSON object */
static char   s_path[512] = {0};       /* path to printers.json */

/* ── Internal helpers ────────────────────────────────────────────── */

static void default_path(void) {
    const char *env = getenv("PRINTER_CONFIG_PATH");
    if (env && *env) {
        snprintf(s_path, sizeof s_path, "%s", env);
    } else {
        snprintf(s_path, sizeof s_path, "/etc/printer-server/printers.json");
    }
}

static bool save_to_disk(void) {
    if (!s_root) return false;

    char tmp[520];
    snprintf(tmp, sizeof tmp, "%s.tmp", s_path);

    char *json = cJSON_Print(s_root);
    if (!json) return false;

    FILE *f = fopen(tmp, "w");
    if (!f) { free(json); return false; }
    fputs(json, f);
    fputc('\n', f);
    fclose(f);
    free(json);

    if (rename(tmp, s_path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

static cJSON *get_printer_obj(const char *name) {
    if (!s_root || !name || !*name) return NULL;
    return cJSON_GetObjectItem(s_root, name);
}

static int get_int(cJSON *obj, const char *field) {
    if (!obj) return -1;
    cJSON *v = cJSON_GetObjectItem(obj, field);
    return (v && cJSON_IsNumber(v)) ? (int)v->valuedouble : -1;
}

static double get_double(cJSON *obj, const char *field) {
    if (!obj) return -1.0;
    cJSON *v = cJSON_GetObjectItem(obj, field);
    return (v && cJSON_IsNumber(v)) ? v->valuedouble : -1.0;
}

static const char *get_string(cJSON *obj, const char *field) {
    if (!obj) return NULL;
    cJSON *v = cJSON_GetObjectItem(obj, field);
    return (v && cJSON_IsString(v) && v->valuestring[0]) ? v->valuestring : NULL;
}

/* ── Public API ──────────────────────────────────────────────────── */

void printer_config_init(void) {
    default_path();

    FILE *f = fopen(s_path, "rb");
    if (!f) {
        /* File doesn't exist — start with empty config. */
        s_root = cJSON_CreateObject();
        save_to_disk();
        printf("[config] Created empty %s\n", s_path);
        return;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > (1 << 20)) {
        fclose(f);
        s_root = cJSON_CreateObject();
        return;
    }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); s_root = cJSON_CreateObject(); return; }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    buf[nr] = '\0';
    fclose(f);

    s_root = cJSON_Parse(buf);
    free(buf);

    if (!s_root) {
        fprintf(stderr, "[config] WARNING: failed to parse %s — starting empty.\n", s_path);
        s_root = cJSON_CreateObject();
    } else {
        printf("[config] Loaded printer configs from %s\n", s_path);
    }
}

cJSON *printer_config_get_json(const char *printer_name) {
    cJSON *p = get_printer_obj(printer_name);
    if (!p) return NULL;
    return cJSON_Duplicate(p, true);
}

bool printer_config_set_from_json(const char *printer_name, cJSON *body) {
    if (!s_root || !printer_name || !*printer_name || !body) return false;

    cJSON *p = cJSON_GetObjectItem(s_root, printer_name);
    if (!p) {
        p = cJSON_CreateObject();
        cJSON_AddItemToObject(s_root, printer_name, p);
    }

    /* Update only fields present in body. */
    static const char *int_fields[] = {"paperWidth", "brightness", "threshold", NULL};
    for (int i = 0; int_fields[i]; i++) {
        cJSON *v = cJSON_GetObjectItem(body, int_fields[i]);
        if (v && cJSON_IsNumber(v)) {
            cJSON_DeleteItemFromObject(p, int_fields[i]);
            cJSON_AddNumberToObject(p, int_fields[i], v->valuedouble);
        }
    }

    cJSON *v = cJSON_GetObjectItem(body, "gamma");
    if (v && cJSON_IsNumber(v)) {
        cJSON_DeleteItemFromObject(p, "gamma");
        cJSON_AddNumberToObject(p, "gamma", v->valuedouble);
    }

    v = cJSON_GetObjectItem(body, "dithering");
    if (v && cJSON_IsString(v) && v->valuestring[0]) {
        cJSON_DeleteItemFromObject(p, "dithering");
        cJSON_AddStringToObject(p, "dithering", v->valuestring);
    }

    return save_to_disk();
}

void printer_config_delete(const char *printer_name) {
    if (!s_root || !printer_name) return;
    cJSON_DeleteItemFromObject(s_root, printer_name);
    save_to_disk();
}

/* ── Resolution helpers ──────────────────────────────────────────── */

int printer_config_paper_width(const char *name, int request_val, int fallback) {
    if (request_val > 0) return request_val;
    int stored = get_int(get_printer_obj(name), "paperWidth");
    return stored > 0 ? stored : fallback;
}

int printer_config_brightness(const char *name, int request_val, int fallback) {
    if (request_val > 0) return request_val;
    int stored = get_int(get_printer_obj(name), "brightness");
    return stored > 0 ? stored : fallback;
}

double printer_config_gamma(const char *name, double request_val, double fallback) {
    if (request_val > 0.0) return request_val;
    double stored = get_double(get_printer_obj(name), "gamma");
    return stored > 0.0 ? stored : fallback;
}

int printer_config_threshold(const char *name, int request_val, int fallback) {
    if (request_val > 0) return request_val;
    int stored = get_int(get_printer_obj(name), "threshold");
    return stored > 0 ? stored : fallback;
}

const char *printer_config_dithering(const char *name,
                                     const char *request_val,
                                     const char *fallback) {
    if (request_val && *request_val) return request_val;
    const char *stored = get_string(get_printer_obj(name), "dithering");
    return stored ? stored : fallback;
}
