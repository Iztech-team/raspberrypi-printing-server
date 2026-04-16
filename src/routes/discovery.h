/*
 * routes/discovery.h — /api/discover, /api/certificate, /api/certificate/regenerate.
 */
#ifndef PRINTER_ROUTES_DISCOVERY_H
#define PRINTER_ROUTES_DISCOVERY_H

#include "../../vendor/mongoose.h"

typedef struct {
    int  http_port;
    int  https_port;
    bool https_enabled;
} discovery_ctx_t;

void route_get_discover             (struct mg_connection *c, struct mg_http_message *hm, const discovery_ctx_t *ctx);
void route_get_certificate          (struct mg_connection *c, struct mg_http_message *hm);
void route_post_regenerate_certificate(struct mg_connection *c, struct mg_http_message *hm);

#endif
