/*
 * response.c — PrintResponse helpers.
 *
 * Implementation matches WindowsPrinterServer/Models/PrintResponse.cs including
 * the ClassifyError() lookup table (lowercased substring match).
 */
#include "response.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <strings.h>   /* strcasestr */

/* Case-insensitive substring search — portable fallback if strcasestr is missing. */
static const char *ci_strstr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    size_t nlen = strlen(needle);
    if (nlen == 0) return haystack;
    for (const char *p = haystack; *p; ++p) {
        if (strncasecmp(p, needle, nlen) == 0) return p;
    }
    return NULL;
}

const char *classify_error(const char *message) {
    if (!message || !*message) return "UNKNOWN";

    /* Order matches the Windows ClassifyError exactly — more specific first. */
    if (ci_strstr(message, "not found") || ci_strstr(message, "not installed"))
        return "PRINTER_NOT_FOUND";
    if (ci_strstr(message, "failed to open printer"))
        return "PRINTER_OPEN_FAILED";
    if (ci_strstr(message, "failed to start document"))
        return "SPOOLER_DOC_FAILED";
    if (ci_strstr(message, "failed to write"))
        return "SPOOLER_WRITE_FAILED";
    if (ci_strstr(message, "incomplete write"))
        return "SPOOLER_PARTIAL_WRITE";
    if (ci_strstr(message, "offline") || ci_strstr(message, "not available"))
        return "PRINTER_OFFLINE";
    if (ci_strstr(message, "paper") &&
        (ci_strstr(message, "out") || ci_strstr(message, "empty") || ci_strstr(message, "end")))
        return "PAPER_OUT";
    if (ci_strstr(message, "door") || ci_strstr(message, "cover") || ci_strstr(message, "open"))
        return "COVER_OPEN";
    if (ci_strstr(message, "jam"))
        return "PAPER_JAM";
    if (ci_strstr(message, "queue") || ci_strstr(message, "stuck") ||
        ci_strstr(message, "error in queue"))
        return "QUEUE_ERROR";
    if (ci_strstr(message, "timeout") || ci_strstr(message, "timed out"))
        return "TIMEOUT";
    if (ci_strstr(message, "access") || ci_strstr(message, "denied") ||
        ci_strstr(message, "permission"))
        return "ACCESS_DENIED";
    if (ci_strstr(message, "spooler") || ci_strstr(message, "service") ||
        ci_strstr(message, "cups"))
        return "SPOOLER_ERROR";
    if (ci_strstr(message, "invalid") && ci_strstr(message, "base64"))
        return "INVALID_DATA";
    if (ci_strstr(message, "no print data") || ci_strstr(message, "no image"))
        return "NO_DATA";
    if (ci_strstr(message, "image") || ci_strstr(message, "bitmap"))
        return "IMAGE_ERROR";

    return "UNKNOWN";
}

cJSON *response_ok(const char *message, long elapsed_ms) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "message", message ? message : "");
    cJSON_AddNullToObject(root, "error");
    cJSON_AddNullToObject(root, "errorCode");
    cJSON_AddNumberToObject(root, "elapsedMs", (double)elapsed_ms);
    return root;
}

cJSON *response_fail(const char *error, long elapsed_ms, const char *error_code) {
    cJSON *root = cJSON_CreateObject();
    const char *code = (error_code && *error_code) ? error_code : classify_error(error);
    cJSON_AddBoolToObject(root, "success", false);
    cJSON_AddStringToObject(root, "message", "Print failed");
    cJSON_AddStringToObject(root, "error", error ? error : "");
    cJSON_AddStringToObject(root, "errorCode", code);
    cJSON_AddNumberToObject(root, "elapsedMs", (double)elapsed_ms);
    return root;
}

void send_json(struct mg_connection *c, int status, cJSON *json) {
    char *body = cJSON_PrintUnformatted(json);
    if (!body) {
        mg_http_reply(c, 500, "Content-Type: application/json\r\n",
                      "{\"success\":false,\"message\":\"JSON serialize failed\"}");
        cJSON_Delete(json);
        return;
    }
    mg_http_reply(c, status,
                  "Content-Type: application/json\r\n"
                  "Access-Control-Allow-Origin: *\r\n",
                  "%s", body);
    free(body);
    cJSON_Delete(json);
}

void send_ok(struct mg_connection *c, const char *message, long elapsed_ms) {
    send_json(c, 200, response_ok(message, elapsed_ms));
}

void send_fail(struct mg_connection *c, int status, const char *error,
               long elapsed_ms, const char *error_code) {
    send_json(c, status, response_fail(error, elapsed_ms, error_code));
}

void send_simple(struct mg_connection *c, int status, bool success, const char *message) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", success);
    cJSON_AddStringToObject(root, "message", message ? message : "");
    send_json(c, status, root);
}

long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}
