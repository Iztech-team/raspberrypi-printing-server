/*
 * types.h — shared types for the Raspberry Pi Printer Server.
 *
 * These mirror the C# records/classes in the Windows project:
 *   - PrinterInfo, UsbPortInfo, PrinterStatusDetail
 *   - AutoDiscoverResult, AutoDiscoverItem
 *   - PrintResponse (with errorCode)
 */
#ifndef PRINTER_TYPES_H
#define PRINTER_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ESC/POS text alignment. */
typedef enum {
    ALIGN_LEFT = 0,
    ALIGN_CENTER = 1,
    ALIGN_RIGHT = 2
} escpos_alignment_t;

/* ── Printer discovery ─────────────────────────────────────────────── */

typedef struct {
    char        name[256];
    bool        is_default;
    bool        is_online;
    char        status[64];    /* "Ready", "Offline", "PaperOut", etc. */
    int         queued_jobs;
} printer_info_t;

typedef struct {
    char name[128];        /* e.g. "usb://HP/Deskjet" or short alias */
    char description[256]; /* device model, make-and-model string */
    char device_uri[512];  /* full CUPS device URI */
} usb_port_info_t;

typedef struct {
    char printer[256];
    bool installed;
    bool online;
    char status[64];
    int  queued_jobs;
} printer_status_detail_t;

/* ── Auto-discover ─────────────────────────────────────────────────── */

typedef struct {
    char type[16];          /* "USB" | "Network" */
    char port[256];
    char description[256];
    char printer_name[128];
    bool success;
    char message[512];
} auto_discover_item_t;

typedef struct {
    int                    total_found;
    int                    total_installed;
    auto_discover_item_t  *items;
    size_t                 count;
    size_t                 capacity;
} auto_discover_result_t;

/* ── Print response (identical JSON shape to Windows) ──────────────── */

typedef struct {
    bool        success;
    char        message[512];
    char        error[512];        /* "" when not an error */
    char        error_code[64];    /* "" when not an error */
    long        elapsed_ms;
} print_response_t;

#endif /* PRINTER_TYPES_H */
