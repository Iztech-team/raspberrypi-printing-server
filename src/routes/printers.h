/*
 * routes/printers.h — /api/printers/* handlers.
 */
#ifndef PRINTER_ROUTES_PRINTERS_H
#define PRINTER_ROUTES_PRINTERS_H

#include "../../vendor/mongoose.h"

void route_get_printers       (struct mg_connection *c, struct mg_http_message *hm);
void route_get_drivers        (struct mg_connection *c, struct mg_http_message *hm);
void route_post_add_printer   (struct mg_connection *c, struct mg_http_message *hm);
void route_get_usb            (struct mg_connection *c, struct mg_http_message *hm);
void route_post_add_usb       (struct mg_connection *c, struct mg_http_message *hm);
void route_post_auto_discover (struct mg_connection *c, struct mg_http_message *hm);
void route_delete_printer     (struct mg_connection *c, struct mg_http_message *hm, const char *name);
void route_post_clear_queue   (struct mg_connection *c, struct mg_http_message *hm, const char *name);
void route_get_printer_status (struct mg_connection *c, struct mg_http_message *hm, const char *name);
void route_get_printer_config (struct mg_connection *c, struct mg_http_message *hm, const char *name);
void route_post_printer_config(struct mg_connection *c, struct mg_http_message *hm, const char *name);

#endif
