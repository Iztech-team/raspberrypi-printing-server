/*
 * cups_service.h — CUPS-backed printer management.
 *
 * Parity map from the Windows version:
 *   PrinterDiscoveryService.GetInstalledPrinters()  →  cups_list_printers()
 *   PrinterDiscoveryService.GetDetailedStatus()     →  cups_get_status()
 *   PrinterManagementService.GetInstalledDrivers()  →  cups_list_drivers()
 *   PrinterManagementService.AddNetworkPrinter()    →  cups_add_network_printer()
 *   PrinterManagementService.DiscoverUsbPorts()     →  cups_discover_usb_ports()
 *   PrinterManagementService.AddUsbPrinter()        →  cups_add_usb_printer()
 *   PrinterManagementService.RemovePrinter()        →  cups_remove_printer()
 *   PrinterManagementService.ClearQueue()           →  cups_clear_queue()
 *   PrinterManagementService.AutoDiscoverAndConnect →  cups_auto_discover()
 *   SpoolerPrinterService.SendAsync()               →  cups_send_raw()
 *
 * All "add" operations shell out to `lpadmin` (there is no pure-C API in libcups
 * for creating queues). Status and listing use the `libcups` C API directly.
 * Printing uses the `cupsCreateJob` + `cupsStartDocument(CUPS_FORMAT_RAW)` flow.
 */
#ifndef PRINTER_CUPS_SERVICE_H
#define PRINTER_CUPS_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include "../types.h"

/* Fill caller-owned array. Returns number written (≤ max). */
size_t cups_list_printers(printer_info_t *out, size_t max);

/* Does the named CUPS queue exist? */
bool cups_printer_exists(const char *name);

/* Detailed status for one printer. */
void cups_get_status(const char *name, printer_status_detail_t *out);

/* Available PPD model identifiers usable with `lpadmin -m`.
 * Returns number of drivers written into `out` (one per entry, truncated to max_per).
 * Example output: "drv:///sample.drv/generic.ppd", "raw", "everywhere", etc. */
size_t cups_list_drivers(char (*out)[256], size_t max);

/* Install a network thermal printer using socket://IP:9100 device URI.
 * The CUPS default driver for raw ESC/POS is "raw" (or "drv:///sample.drv/generic.ppd"
 * if the raw driver isn't enabled on this system). */
bool cups_add_network_printer(const char *ip, const char *name,
                              const char *driver,
                              char *msg, size_t msg_len);

/* Discover USB-attached printer devices via `lpinfo -v`. */
size_t cups_discover_usb_ports(usb_port_info_t *out, size_t max);

/* Install a USB printer by device URI (e.g. usb://HP/Deskjet). */
bool cups_add_usb_printer(const char *device_uri, const char *name,
                          const char *driver,
                          char *msg, size_t msg_len);

bool cups_remove_printer(const char *name, char *msg, size_t msg_len);
bool cups_clear_queue(const char *name, char *msg, size_t msg_len);

/* Walks USB + (optionally) subnet, installing each new printer found.
 * `subnet` like "192.168.1" scans .1..254 on port 9100; NULL = USB only. */
void cups_auto_discover(const char *subnet, auto_discover_result_t *result);
void cups_auto_discover_result_free(auto_discover_result_t *result);

/* Submit raw bytes to a printer as application/vnd.cups-raw.
 * Returns 0 on success, or a negative number on failure; fills err on failure. */
int cups_send_raw(const char *name,
                  const uint8_t *data, size_t len,
                  char *err, size_t err_len,
                  char *err_code, size_t err_code_len);

#endif /* PRINTER_CUPS_SERVICE_H */
