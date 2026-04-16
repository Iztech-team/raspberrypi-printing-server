/*
 * printer_config.h — Per-printer configuration storage.
 *
 * Manages /etc/printer-server/printers.json — a JSON file keyed by
 * printer name that stores per-printer defaults (paperWidth, brightness,
 * gamma, dithering, threshold).
 *
 * Resolution logic for each field:
 *   1. Explicit value from API request (if > 0 or non-empty)
 *   2. Stored value from printers.json
 *   3. Global default (48 chars, 130 brightness, 1.8 gamma, etc.)
 */
#ifndef PRINTER_CONFIG_H
#define PRINTER_CONFIG_H

#include <stdbool.h>
#include "../../vendor/cJSON.h"

/* Load config from disk. Creates empty file if missing. */
void printer_config_init(void);

/* Get a printer's config as a new cJSON object. Caller must cJSON_Delete.
 * Returns NULL if no config stored for this printer. */
cJSON *printer_config_get_json(const char *printer_name);

/* Update a printer's config from a JSON body. Partial update — only
 * fields present in `body` are written. Persists to disk immediately.
 * Returns true on success. */
bool printer_config_set_from_json(const char *printer_name, cJSON *body);

/* Delete a printer's stored config. Persists to disk. */
void printer_config_delete(const char *printer_name);

/*
 * Resolution helpers — return the effective value for a print request.
 *
 *   request_val : value from the API request (-1 or 0 means "not set")
 *   fallback    : global default if neither request nor config has a value
 *
 * Priority: request_val > stored config > fallback
 */
int    printer_config_paper_width(const char *name, int request_val, int fallback);
int    printer_config_brightness (const char *name, int request_val, int fallback);
double printer_config_gamma      (const char *name, double request_val, double fallback);
int    printer_config_threshold  (const char *name, int request_val, int fallback);

/* For dithering: request_val may be NULL or empty to mean "not set". */
const char *printer_config_dithering(const char *name,
                                     const char *request_val,
                                     const char *fallback);

#endif /* PRINTER_CONFIG_H */
