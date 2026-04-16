/*
 * printer_config.h — Per-printer configuration storage.
 *
 * Manages /etc/printer-server/printers.json keyed by printer name.
 * Resolution: explicit request value > stored config > global default.
 */
#ifndef PRINTER_CONFIG_H
#define PRINTER_CONFIG_H

#include <stdbool.h>
#include "../../vendor/cJSON.h"

void   printer_config_init(void);
cJSON *printer_config_get_json(const char *printer_name);
bool   printer_config_set_from_json(const char *printer_name, cJSON *body);
void   printer_config_delete(const char *printer_name);

int         printer_config_paper_width(const char *name, int request_val, int fallback);
int         printer_config_brightness (const char *name, int request_val, int fallback);
double      printer_config_gamma      (const char *name, double request_val, double fallback);
int         printer_config_threshold  (const char *name, int request_val, int fallback);
const char *printer_config_dithering  (const char *name, const char *request_val, const char *fallback);

#endif
