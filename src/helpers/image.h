/*
 * image.h — Image → ESC/POS raster conversion.
 *
 * Direct port of Helpers/EscPosImageHelper.cs. Accepts PNG/JPG/BMP bytes
 * (anything stb_image can decode), produces a GS v 0 raster byte stream
 * with band splitting at 256 rows to avoid overflowing small thermal-printer
 * receive buffers (typically 4–64 KB).
 *
 * Pipeline:
 *   1. Decode with stb_image
 *   2. Flatten alpha onto white background + high-quality bilinear resize
 *   3. Convert to grayscale (ITU-R BT.601 coefficients: 0.299/0.587/0.114)
 *   4. Apply brightness
 *   5. Apply gamma correction
 *   6. Dither (floyd-steinberg | atkinson | threshold)
 *   7. Pack bits into ESC/POS GS v 0 raster bands
 */
#ifndef PRINTER_IMAGE_H
#define PRINTER_IMAGE_H

#include <stdint.h>
#include <stddef.h>

/* 203 dpi at common receipt widths. */
#define PRINTER_PIX_WIDTH_58MM  384
#define PRINTER_PIX_WIDTH_80MM  576
#define PRINTER_PIX_WIDTH_EPSON 512   /* Epson TM-T88/T20 at 80mm */

/* Map paper character width → pixel width.
 *   32 = 58mm (384px), 42 = 80mm Epson (512px), 48 = 80mm (576px) */
int image_pixel_width_for(int paper_char_width);

/*
 * Convert an image buffer to ESC/POS GS v 0 raster bytes.
 *
 *   image_data, image_len : raw PNG/JPG/BMP bytes
 *   max_width_px          : target pixel width (384 or 576)
 *   brightness            : 100 = no change, 130 = +30%. Clamped to 50..200.
 *   gamma                 : 1.0 = no change, 1.8 = recommended. Clamped 0.5..3.0.
 *   dithering             : "floyd-steinberg", "atkinson", or "threshold"
 *   threshold             : 0..255, only used when dithering == "threshold"
 *   out_len               : receives byte length of returned buffer
 *   err_buf, err_len      : if conversion fails, human-readable error goes here
 *
 * Returns: malloc'd byte buffer on success (caller free), NULL on failure.
 */
uint8_t *image_to_escpos_raster(const uint8_t *image_data, size_t image_len,
                                int max_width_px,
                                int brightness, double gamma,
                                const char *dithering, int threshold,
                                size_t *out_len,
                                char *err_buf, size_t err_len);

#endif /* PRINTER_IMAGE_H */
