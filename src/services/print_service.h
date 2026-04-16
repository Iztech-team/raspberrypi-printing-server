/*
 * print_service.h — high-level print orchestration.
 *
 * Parity map from the Windows version:
 *   PrinterService.PrintRawAsync      → print_service_raw()
 *   PrinterService.PrintTextAsync     → print_service_text()
 *   PrinterService.PrintTestAsync     → print_service_test()
 *   PrinterService.BeepAsync          → print_service_beep()
 *   PrinterService.CutAsync           → print_service_cut()
 *   PrinterService.OpenDrawerAsync    → print_service_open_drawer()
 *   PrinterService.PrintImageAsync    → print_service_image()
 *
 * Each function returns a fully-populated print_response_t (including
 * elapsed_ms and errorCode).
 */
#ifndef PRINTER_PRINT_SERVICE_H
#define PRINTER_PRINT_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "../types.h"

void print_service_raw(const char *printer,
                       const uint8_t *data, size_t len,
                       print_response_t *out);

void print_service_text(const char *printer,
                        const char *text,
                        int paper_width,
                        bool cut_paper,
                        int feed_lines,
                        print_response_t *out);

void print_service_test(const char *printer,
                        int paper_width,
                        print_response_t *out);

void print_service_beep(const char *printer,
                        int count, int duration,
                        print_response_t *out);

void print_service_cut(const char *printer,
                       const char *cut_type,
                       int feed_lines,
                       print_response_t *out);

void print_service_open_drawer(const char *printer,
                               print_response_t *out);

void print_service_image(const char *printer,
                         const uint8_t *image_data, size_t image_len,
                         int paper_width,
                         bool cut_paper, int feed_lines,
                         int brightness, double gamma,
                         const char *dithering, int threshold,
                         print_response_t *out);

#endif /* PRINTER_PRINT_SERVICE_H */
