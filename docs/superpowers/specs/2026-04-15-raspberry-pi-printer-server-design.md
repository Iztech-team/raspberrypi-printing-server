# Raspberry Pi Thermal Printer Server — Design

**Date:** 2026-04-15
**Status:** Approved
**Source project:** `windows-printer-server` (ASP.NET 8)

## Purpose

A drop-in Linux/Raspberry Pi replacement for the Windows thermal-printer HTTP server.
Same endpoints, same JSON shapes, same default values, same error codes — so any
PWA, mobile app, or POS client built against the Windows server works unchanged.

## Technology Stack

| Concern | Library | Why |
|---|---|---|
| HTTP/HTTPS server | **mongoose v7** (vendored) | Single-file, embedded, native TLS, multipart, routing |
| Printer management | **libcups2** + `lpadmin` shell | Standard on all Linux distros, handles USB + network |
| Raw printing | CUPS `cupsCreateJob` + `CUPS_FORMAT_RAW` | Bypasses filters, sends ESC/POS bytes verbatim |
| JSON | **cJSON** (vendored) | Single-file, tiny, MIT |
| Image decode | **stb_image.h** (vendored) | Single-header, MIT, supports PNG/JPG/BMP |
| HTTPS cert | `openssl` CLI shelled out on first run | Same idea as Windows version's X509 APIs |
| Build | CMake + Makefile | Standard Pi toolchain |
| Service | systemd | Standard Pi service manager |

## Endpoint Parity

All 19 endpoints from the Windows version are implemented with identical paths,
request bodies, response schemas, and default values:

- `GET /api/printers`, `GET /api/printers/drivers`, `GET /api/printers/usb`, `GET /api/printers/status/{name}`
- `POST /api/printers/add`, `POST /api/printers/add-usb`, `POST /api/printers/auto-discover`, `POST /api/printers/clear/{name}`
- `DELETE /api/printers/{name}`
- `POST /api/print/{raw,text,test,beep,cut,open-drawer,image}`
- `GET /api/discover`, `GET /api/certificate`, `POST /api/certificate/regenerate`

## Status Mapping

Windows PRINTER_STATUS integers are replaced by CUPS IPP `printer-state` +
`printer-state-reasons` strings, mapped back to the same human-readable tokens
the Windows version emits (`Ready`, `Paused`, `PaperOut`, `DoorOpen`, `Offline`,
`Error`, `Printing`, …).

## Project Layout

```
raspberry-pi-printer-server/
├── CMakeLists.txt, Makefile, README.md, .gitignore
├── systemd/printer-server.service
├── scripts/fetch-vendor.sh        # one-time download of mongoose, cJSON, stb_image
├── config.json                    # port, httpsPort, enableHttps
├── src/
│   ├── main.c                     # entry, event loop, signal handling
│   ├── server.c/h                 # route dispatch, CORS, exception handler
│   ├── types.h                    # shared structs
│   ├── routes/{printers,print,discovery}.c/h
│   ├── services/{cups,print,cert}_service.c/h
│   └── helpers/{escpos,image,response}.c/h
└── vendor/                        # mongoose, cJSON, stb_image (fetched by script)
```

## Threading

Single-threaded mongoose event loop. Print jobs complete in <200ms for typical
receipts, acceptable for POS workloads. Future optimization: use `mg_wakeup`
for offloading to a worker pool if concurrency demands grow.

## Security Model

Same as Windows version: self-signed cert covering all local IPs via SAN,
served over HTTPS on :5124; client apps download the `.cer` from
`/api/certificate` and install it into the device trust store. HTTP :5123 also
available for LAN-only deployments.

## Runtime Dependencies

Build: `build-essential cmake libcups2-dev libssl-dev pkg-config`
Runtime: `cups cups-client openssl`

Install once:

```sh
sudo apt install build-essential cmake libcups2-dev libssl-dev pkg-config \
                 cups cups-client cups-bsd openssl
```
