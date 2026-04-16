/*
 * routes/printers.c — printer management endpoints.
 *
 * JSON shapes match the Windows version exactly (camelCase field names,
 * same nested structure). Each handler is short — the real work is in
 * cups_service.c.
 */
#include "printers.h"
#include "../services/cups_service.h"
#include "../services/printer_config.h"
#include "../helpers/response.h"
#include "../../vendor/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Convert mg_str → null-terminated C string (malloc'd). */
static char *mg_str_to_cstr(struct mg_str s) {
    char *out = (char *)malloc(s.len + 1);
    if (!out) return NULL;
    memcpy(out, s.buf, s.len);
    out[s.len] = '\0';
    return out;
}

static cJSON *printer_info_to_json(const printer_info_t *p) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name",       p->name);
    cJSON_AddBoolToObject  (o, "isDefault",  p->is_default);
    cJSON_AddBoolToObject  (o, "isOnline",   p->is_online);
    cJSON_AddStringToObject(o, "status",     p->status);
    cJSON_AddNumberToObject(o, "queuedJobs", p->queued_jobs);
    return o;
}

/* GET /api/printers — list installed printers. */
void route_get_printers(struct mg_connection *c, struct mg_http_message *hm) {
    (void)hm;
    printer_info_t list[64];
    size_t n = cups_list_printers(list, 64);

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++)
        cJSON_AddItemToArray(arr, printer_info_to_json(&list[i]));

    send_json(c, 200, arr);
}

/* GET /api/printers/drivers — list installed drivers. */
void route_get_drivers(struct mg_connection *c, struct mg_http_message *hm) {
    (void)hm;
    char drivers[128][256];
    size_t n = cups_list_drivers(drivers, 128);

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(drivers[i]));

    send_json(c, 200, arr);
}

/* POST /api/printers/add — install a network printer. */
void route_post_add_printer(struct mg_connection *c, struct mg_http_message *hm) {
    char *body = mg_str_to_cstr(hm->body);
    cJSON *req = body ? cJSON_Parse(body) : NULL;
    free(body);

    const char *ip      = req ? cJSON_GetStringValue(cJSON_GetObjectItem(req, "ipAddress"))   : NULL;
    const char *name    = req ? cJSON_GetStringValue(cJSON_GetObjectItem(req, "printerName")) : NULL;
    const char *driver  = req ? cJSON_GetStringValue(cJSON_GetObjectItem(req, "driverName"))  : NULL;

    if (!ip || !*ip) {
        send_simple(c, 400, false, "IP address is required.");
        if (req) cJSON_Delete(req);
        return;
    }

    char msg[512];
    bool ok = cups_add_network_printer(ip, name, driver, msg, sizeof msg);
    send_simple(c, ok ? 200 : 400, ok, msg);

    if (req) cJSON_Delete(req);
}

/* GET /api/printers/usb — discover USB printer ports. */
void route_get_usb(struct mg_connection *c, struct mg_http_message *hm) {
    (void)hm;
    usb_port_info_t ports[32];
    size_t n = cups_discover_usb_ports(ports, 32);

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name",        ports[i].name);
        cJSON_AddStringToObject(o, "description", ports[i].description);
        /* `deviceUri` is an extension (not present in Windows version, but
         * required on Linux where add-usb takes a URI rather than a port name).
         * The Windows client's UI will ignore unknown fields. */
        cJSON_AddStringToObject(o, "deviceUri",   ports[i].device_uri);
        cJSON_AddItemToArray(arr, o);
    }
    send_json(c, 200, arr);
}

/* POST /api/printers/add-usb — install a USB printer.
 *
 * Accepts either:
 *   { portName, printerName, driverName }         (Windows-style, portName = URI)
 *   { deviceUri, printerName, driverName }        (Pi-style, new field name)
 *
 * portName is treated as the device URI for cross-compat. */
void route_post_add_usb(struct mg_connection *c, struct mg_http_message *hm) {
    char *body = mg_str_to_cstr(hm->body);
    cJSON *req = body ? cJSON_Parse(body) : NULL;
    free(body);

    const char *port   = req ? cJSON_GetStringValue(cJSON_GetObjectItem(req, "portName"))    : NULL;
    const char *uri    = req ? cJSON_GetStringValue(cJSON_GetObjectItem(req, "deviceUri"))   : NULL;
    const char *name   = req ? cJSON_GetStringValue(cJSON_GetObjectItem(req, "printerName")) : NULL;
    const char *driver = req ? cJSON_GetStringValue(cJSON_GetObjectItem(req, "driverName"))  : NULL;

    const char *device = (uri && *uri) ? uri : port;
    if (!device || !*device) {
        send_simple(c, 400, false,
            "USB device URI is required. Use GET /api/printers/usb to discover devices.");
        if (req) cJSON_Delete(req);
        return;
    }

    char msg[512];
    bool ok = cups_add_usb_printer(device, name, driver, msg, sizeof msg);
    send_simple(c, ok ? 200 : 400, ok, msg);
    if (req) cJSON_Delete(req);
}

/* POST /api/printers/auto-discover. */
void route_post_auto_discover(struct mg_connection *c, struct mg_http_message *hm) {
    char *body = mg_str_to_cstr(hm->body);
    cJSON *req = body ? cJSON_Parse(body) : NULL;
    free(body);

    const char *subnet = req ? cJSON_GetStringValue(cJSON_GetObjectItem(req, "subnet")) : NULL;

    auto_discover_result_t result;
    cups_auto_discover(subnet, &result);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "totalFound",     result.total_found);
    cJSON_AddNumberToObject(root, "totalInstalled", result.total_installed);

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < result.count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type",        result.items[i].type);
        cJSON_AddStringToObject(o, "port",        result.items[i].port);
        cJSON_AddStringToObject(o, "description", result.items[i].description);
        cJSON_AddStringToObject(o, "printerName", result.items[i].printer_name);
        cJSON_AddBoolToObject  (o, "success",     result.items[i].success);
        cJSON_AddStringToObject(o, "message",     result.items[i].message);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddItemToObject(root, "results", arr);
    send_json(c, 200, root);

    cups_auto_discover_result_free(&result);
    if (req) cJSON_Delete(req);
}

/* DELETE /api/printers/{name} */
void route_delete_printer(struct mg_connection *c, struct mg_http_message *hm,
                          const char *name) {
    (void)hm;
    char msg[512];
    bool ok = cups_remove_printer(name, msg, sizeof msg);
    send_simple(c, ok ? 200 : 400, ok, msg);
}

/* POST /api/printers/clear/{name} */
void route_post_clear_queue(struct mg_connection *c, struct mg_http_message *hm,
                            const char *name) {
    (void)hm;
    char msg[512];
    bool ok = cups_clear_queue(name, msg, sizeof msg);
    send_simple(c, ok ? 200 : 400, ok, msg);
}

/* GET /api/printers/status/{name} */
void route_get_printer_status(struct mg_connection *c, struct mg_http_message *hm,
                              const char *name) {
    (void)hm;
    printer_status_detail_t d;
    cups_get_status(name, &d);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "printer",    d.printer);
    cJSON_AddBoolToObject  (o, "installed",  d.installed);
    cJSON_AddBoolToObject  (o, "online",     d.online);
    cJSON_AddStringToObject(o, "status",     d.status);
    cJSON_AddNumberToObject(o, "queuedJobs", d.queued_jobs);
    send_json(c, 200, o);
}

/* GET /api/printers/config/{name} */
void route_get_printer_config(struct mg_connection *c, struct mg_http_message *hm,
                              const char *name) {
    (void)hm;
    cJSON *cfg = printer_config_get_json(name);
    if (!cfg) cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "printer", name);
    send_json(c, 200, cfg);
}

/* POST /api/printers/config/{name} */
void route_post_printer_config(struct mg_connection *c, struct mg_http_message *hm,
                               const char *name) {
    char *body = mg_str_to_cstr(hm->body);
    cJSON *req = body ? cJSON_Parse(body) : NULL;
    free(body);

    if (!req) {
        send_simple(c, 400, false, "Invalid JSON body.");
        return;
    }

    bool ok = printer_config_set_from_json(name, req);
    cJSON_Delete(req);

    if (ok) send_simple(c, 200, true, "Printer configuration saved.");
    else    send_simple(c, 500, false, "Failed to save printer configuration.");
}
