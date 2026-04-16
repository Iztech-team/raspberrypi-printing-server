/*
 * cert_service.h — Self-signed HTTPS certificate lifecycle.
 *
 * Parity map from the Windows version:
 *   CertificateService.GetOrCreateCertificate() → cert_get_or_create()
 *   CertificateService.GetLocalIpAddresses()    → cert_get_local_ips()
 *   CertificateService.GetPublicCertPath()      → cert_get_public_cert_path()
 *
 * On first run:
 *   1. Enumerate local IPv4 addresses (skipping loopback).
 *   2. Generate a 2048-bit RSA key and a self-signed X.509 certificate
 *      valid for 10 years whose Subject Alternative Name covers localhost
 *      and every local IP.
 *   3. Write cert.pem, key.pem, and cert.der (the "public .cer" file) into
 *      /etc/printer-server/certs (or $XDG_CONFIG_HOME/printer-server/certs
 *      if running unprivileged).
 *
 * Subsequent runs reuse the existing files. `cert_regenerate()` deletes and
 * recreates all three.
 */
#ifndef PRINTER_CERT_SERVICE_H
#define PRINTER_CERT_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

/* Ensure cert/key files exist. On success, fills out_cert_path and out_key_path
 * with absolute filesystem paths (PEM-encoded). Returns true on success. */
bool cert_get_or_create(char *out_cert_path, size_t cert_path_len,
                        char *out_key_path,  size_t key_path_len);

/* Path to the DER-encoded public cert (.cer file) for client-device install. */
const char *cert_get_public_cert_path(void);

/* Regenerate all three files (cert.pem, key.pem, cert.der). */
bool cert_regenerate(char *err, size_t err_len);

/* Fill `out` with comma-separated local IPv4 addresses (skips loopback).
 * Returns number of addresses written. */
size_t cert_get_local_ips(char (*out)[64], size_t max);

#endif /* PRINTER_CERT_SERVICE_H */
