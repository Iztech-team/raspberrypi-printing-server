/*
 * server.h — central mongoose event handler + route dispatch table.
 */
#ifndef PRINTER_SERVER_H
#define PRINTER_SERVER_H

#include "../vendor/mongoose.h"
#include <stdbool.h>

typedef struct {
    int  http_port;
    int  https_port;
    bool https_enabled;
    char www_root[512];
} server_config_t;

/* Global config pointer; initialised in main() before mg_mgr_poll(). */
extern const server_config_t *g_config;

/* The single HTTP event callback. Use with mg_http_listen().
 * TLS is enabled on accept if fn_data is non-NULL (mg_tls_opts *). */
void server_event(struct mg_connection *c, int ev, void *ev_data);

#endif
