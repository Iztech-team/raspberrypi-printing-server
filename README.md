# Raspberry Pi Thermal Printer Server

A drop-in **Linux/Raspberry Pi** replacement for the `windows-printer-server`
ASP.NET project. Same endpoints, same JSON shape, same default values, same
error codes — your existing PWA, mobile app, or POS client works unchanged.

Written in **C** for tiny memory footprint (~1 MB binary, <10 MB RSS), uses
**CUPS** for printer management, and serves both **HTTP** and **HTTPS** on the
same ports the Windows version does (5123 and 5124).

---

## Features

- **All 19 endpoints** from the Windows version, byte-for-byte identical
  request/response contracts.
- **CUPS-backed** printer management (USB + network, queue clearing, status).
- **Raw ESC/POS passthrough** — bytes go to the printer untouched.
- **Image printing** (PNG/JPG/BMP) with Floyd-Steinberg / Atkinson dithering.
- **Self-signed TLS** with all local IPs in the SAN; downloadable `.cer` for
  Android/iOS install.
- **systemd** unit + clean shutdown handling.

---

## Quick install (one command)

```sh
git clone <your-repo-url> raspberry-pi-printer-server
cd raspberry-pi-printer-server
sudo ./setup.sh
```

That's it. The `setup.sh` script:

1. Detects your distro (Debian / Raspbian / Ubuntu).
2. Installs every apt dependency it needs (`build-essential`, `cmake`,
   `libcups2-dev`, `libssl-dev`, `cups`, `openssl`, …).
3. Ensures the CUPS daemon is enabled and running.
4. Adds your user to the `lpadmin` group.
5. Downloads the vendored libraries (mongoose, cJSON, stb_image).
6. Builds the binary.
7. Installs the binary, config, static files, and systemd unit.
8. Enables the service so it starts **automatically on every boot**.
9. Starts it now and prints the access URLs.

The script is idempotent — re-running it is safe. On reboot, the systemd
unit takes over and keeps the server running in the background.

Other modes:

```sh
sudo ./setup.sh --force         # rebuild + reinstall even if already up
sudo ./setup.sh --status        # show what's installed
sudo ./setup.sh --uninstall     # remove everything (keeps TLS cert)
sudo ./setup.sh --help
```

### Manual build (if you prefer)

```sh
sudo apt install -y build-essential cmake pkg-config \
                    libcups2-dev libssl-dev \
                    cups cups-client cups-bsd openssl curl
./scripts/fetch-vendor.sh
make
sudo make install
sudo systemctl enable --now printer-server
```

### Verify

```sh
curl http://localhost:5123/api/discover
curl -k https://localhost:5124/api/discover
systemctl status printer-server
journalctl -u printer-server -f       # tail logs
```

---

## Endpoints (cheat-sheet)

| Method | Path | Description |
|---|---|---|
| `GET` | `/api/printers` | List installed printers |
| `GET` | `/api/printers/drivers` | List installed drivers |
| `POST` | `/api/printers/add` | Add a network printer (`{ipAddress, printerName?, driverName?}`) |
| `GET` | `/api/printers/usb` | Discover USB printer ports |
| `POST` | `/api/printers/add-usb` | Add a USB printer (`{deviceUri, printerName?, driverName?}`) |
| `POST` | `/api/printers/auto-discover` | Auto-discover and install all printers (`{subnet?}`) |
| `DELETE` | `/api/printers/{name}` | Remove a printer |
| `POST` | `/api/printers/clear/{name}` | Clear a printer's queue |
| `GET` | `/api/printers/status/{name}` | Detailed status |
| `POST` | `/api/print/raw` | Print raw ESC/POS (`{printer, base64Data \| rawBytes}`) |
| `POST` | `/api/print/text` | Print formatted text |
| `POST` | `/api/print/test` | Print a test page |
| `POST` | `/api/print/beep` | Buzzer beep |
| `POST` | `/api/print/cut` | Paper cut |
| `POST` | `/api/print/open-drawer` | Open cash drawer |
| `POST` | `/api/print/image` | Print an image (multipart/form-data) |
| `GET` | `/api/discover` | Server identity + IPs |
| `GET` | `/api/certificate` | Download HTTPS cert (.cer) |
| `POST` | `/api/certificate/regenerate` | Regenerate HTTPS cert |

### Response shape

Every print operation returns:

```json
{
  "success":   true,
  "message":   "Printed 42 bytes to 'Thermal-Kitchen'.",
  "error":     null,
  "errorCode": null,
  "elapsedMs": 87
}
```

Failures populate `error` and `errorCode` (one of: `PRINTER_NOT_FOUND`,
`PRINTER_OFFLINE`, `PAPER_OUT`, `COVER_OPEN`, `PAPER_JAM`, `ACCESS_DENIED`,
`SPOOLER_ERROR`, `SPOOLER_DOC_FAILED`, `SPOOLER_WRITE_FAILED`,
`SPOOLER_PARTIAL_WRITE`, `INVALID_DATA`, `NO_DATA`, `IMAGE_ERROR`,
`PRINTER_NAME_MISSING`, `INVALID_REQUEST`, `TIMEOUT`, `UNKNOWN`).

---

## Differences from the Windows version

The contract is identical, but the underlying mechanisms differ in ways
that occasionally surface:

| Concern | Windows | Raspberry Pi |
|---|---|---|
| Add USB request field | `portName` (e.g. `USB006`) | `deviceUri` (e.g. `usb://Epson/TM-T20II`) — `portName` is still accepted as alias |
| Drivers list | Windows-installed driver names | CUPS PPD model identifiers (e.g. `raw`, `drv:///sample.drv/generic.ppd`, `everywhere`) |
| Default driver | `Generic / Text Only` | `raw` (with auto-fallback to `drv:///sample.drv/generic.ppd`) |
| Status strings | Mapped from `PRINTER_STATUS` codes | Mapped from CUPS IPP `printer-state` + `printer-state-reasons` to the same vocabulary |

For the API consumer, the JSON shape is unchanged.

---

## Running without systemd

```sh
./build/printer-server -c ./config.json
```

Environment variable overrides:

| Variable | Effect |
|---|---|
| `PRINTER_SERVER_PORT` | HTTP port (default 5123) |
| `PRINTER_SERVER_HTTPS_PORT` | HTTPS port (default 5124) |
| `PRINTER_SERVER_HTTPS` | `true` / `false` to toggle HTTPS |
| `PRINTER_SERVER_WWW` | Path to static-files root |

---

## Mobile certificate trust

To make Android or iOS PWAs talk to the Pi over HTTPS without warnings:

1. On the device, open `https://<pi-ip>:5124/api/certificate`
2. Open the downloaded `printserver.cer`
3. Install it as a CA certificate.

The cert covers `localhost`, `127.0.0.1`, the Pi's hostname, and every
non-loopback IPv4 the Pi has.  After a network change, regenerate:

```sh
curl -k -X POST https://localhost:5124/api/certificate/regenerate
sudo systemctl restart printer-server
```

---

## Project layout

```
raspberry-pi-printer-server/
├── CMakeLists.txt, Makefile, config.json, README.md, .gitignore
├── systemd/printer-server.service
├── scripts/fetch-vendor.sh
├── docs/superpowers/specs/2026-04-15-raspberry-pi-printer-server-design.md
├── src/
│   ├── main.c, server.c/h, types.h
│   ├── routes/{printers,print,discovery}.c/h
│   ├── services/{cups_service,cert_service,print_service}.c/h
│   └── helpers/{response,escpos,image}.c/h
├── vendor/                   # mongoose, cJSON, stb_image (fetched)
└── www/                      # static files (landing page)
```

---

## Troubleshooting

- **`lpadmin: not authorized`** — your user must be in the `lpadmin` group, or
  run the server as root (the systemd unit does this by default).
- **`Failed to add printer ... Bad device-uri scheme`** — your CUPS install
  doesn't have the `usb://` backend. Install with `sudo apt install
  printer-driver-cups-pdf cups-filters`.
- **HTTPS doesn't load** — check the certs exist:
  `ls -la /etc/printer-server/certs/`. Regenerate via the API or delete and
  restart the service.
- **`raw` driver rejected** — older CUPS versions disable raw queues; the
  server automatically falls back to `drv:///sample.drv/generic.ppd`. To force
  raw, edit `/etc/cups/cups-files.conf` and ensure no `FilterLimit` blocks raw.
- **Image prints look too dark / too light** — tweak `brightness` (50–200) and
  `gamma` (0.5–3.0) on the `/api/print/image` request.

---

## License

This project includes vendored copies of:

- [mongoose](https://github.com/cesanta/mongoose) (GPLv2 / commercial — check
  the upstream license header before commercial deployment)
- [cJSON](https://github.com/DaveGamble/cJSON) (MIT)
- [stb_image](https://github.com/nothings/stb) (MIT or Public Domain)

Your own code is unencumbered.
