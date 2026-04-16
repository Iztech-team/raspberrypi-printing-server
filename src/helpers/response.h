/*
 * response.h — PrintResponse helpers.
 *
 * Produce the exact same JSON shape as the Windows server:
 *   { "success": bool, "message": str, "error": str|null, "errorCode": str|null, "elapsedMs": int }
 *
 * Error codes are classified from free-text error messages using the same
 * vocabulary the Windows PrintResponse.ClassifyError uses.
 */
#ifndef PRINTER_RESPONSE_H
#define PRINTER_RESPONSE_H

#include "../../vendor/cJSON.h"
#include "../../vendor/mongoose.h"
#include "../types.h"

/* Build an "ok" PrintResponse as a cJSON object (caller must cJSON_Delete). */
cJSON *response_ok(const char *message, long elapsed_ms);

/* Build a "fail" PrintResponse. If error_code is NULL, it is classified from
 * the error message using classify_error(). */
cJSON *response_fail(const char *error, long elapsed_ms, const char *error_code);

/* Classify a free-text error message into an error_code string
 * (pointer into a static table — do NOT free). Mirrors PrintResponse.ClassifyError. */
const char *classify_error(const char *message);

/* Send a cJSON object as a JSON response, then delete it. */
void send_json(struct mg_connection *c, int status, cJSON *json);

/* Convenience: build + send an ok PrintResponse. */
void send_ok(struct mg_connection *c, const char *message, long elapsed_ms);

/* Convenience: build + send a fail PrintResponse with the given HTTP status. */
void send_fail(struct mg_connection *c, int status, const char *error,
               long elapsed_ms, const char *error_code);

/* Convenience: send a generic {success, message} JSON for non-PrintResponse ops. */
void send_simple(struct mg_connection *c, int status, bool success, const char *message);

/* Monotonic wall-clock millisecond counter for elapsed-time measurement. */
long monotonic_ms(void);

#endif /* PRINTER_RESPONSE_H */
