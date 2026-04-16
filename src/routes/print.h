/*
 * routes/print.h — /api/print/* handlers.
 */
#ifndef PRINTER_ROUTES_PRINT_H
#define PRINTER_ROUTES_PRINT_H

#include "../../vendor/mongoose.h"

void route_post_print_raw       (struct mg_connection *c, struct mg_http_message *hm);
void route_post_print_text      (struct mg_connection *c, struct mg_http_message *hm);
void route_post_print_test      (struct mg_connection *c, struct mg_http_message *hm);
void route_post_print_beep      (struct mg_connection *c, struct mg_http_message *hm);
void route_post_print_cut       (struct mg_connection *c, struct mg_http_message *hm);
void route_post_print_open_drawer(struct mg_connection *c, struct mg_http_message *hm);
void route_post_print_image     (struct mg_connection *c, struct mg_http_message *hm);

#endif
