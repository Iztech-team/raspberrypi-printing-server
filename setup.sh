#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  setup.sh — one-shot installer for the Raspberry Pi Printer Server.
#
#  What this does (once):
#    1. Verify we're on a supported Linux (Debian/Raspbian/Ubuntu).
#    2. Install apt dependencies (build tools, libcups2-dev, libssl-dev, cups).
#    3. Ensure CUPS daemon is running + enabled on boot.
#    4. Fetch vendored deps (mongoose, cJSON, stb_image).
#    5. Build the binary.
#    6. Install /usr/local/bin/printer-server, config, systemd unit.
#    7. Enable + start the systemd service (runs in background on every boot).
#    8. Print a summary with the URLs the server is listening on.
#
#  Idempotent: safe to re-run. If the service is already installed & healthy,
#  it just reports that and exits.
#
#  Usage:
#     sudo ./setup.sh                 # full install
#     sudo ./setup.sh --force         # rebuild + reinstall even if up
#     sudo ./setup.sh --uninstall     # remove everything
#     sudo ./setup.sh --status        # show current status
# ─────────────────────────────────────────────────────────────────────────────

set -euo pipefail

# ── Colors ──────────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
  C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_YLW=$'\033[33m'
  C_BLU=$'\033[34m'; C_BOLD=$'\033[1m'; C_RST=$'\033[0m'
else
  C_RED=; C_GRN=; C_YLW=; C_BLU=; C_BOLD=; C_RST=
fi

log()   { printf "%s==>%s %s\n" "$C_BLU$C_BOLD"    "$C_RST" "$*"; }
ok()    { printf "%s ✓%s %s\n"  "$C_GRN"           "$C_RST" "$*"; }
warn()  { printf "%s ⚠%s %s\n"  "$C_YLW"           "$C_RST" "$*"; }
err()   { printf "%s ✗%s %s\n"  "$C_RED$C_BOLD"    "$C_RST" "$*" >&2; }
die()   { err "$*"; exit 1; }

# ── Paths ───────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
VENDOR_DIR="$SCRIPT_DIR/vendor"

BIN_PATH="/usr/local/bin/printer-server"
CONFIG_DIR="/etc/printer-server"
CONFIG_FILE="$CONFIG_DIR/config.json"
SERVICE_NAME="printer-server"
SERVICE_FILE="/lib/systemd/system/${SERVICE_NAME}.service"
WWW_DIR="/usr/local/share/printer-server/www"

# ── Helpers ─────────────────────────────────────────────────────────────────
require_root() {
  if [[ $EUID -ne 0 ]]; then
    die "This script must be run as root. Try:  sudo $0 $*"
  fi
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

is_service_active() {
  systemctl is-active --quiet "$SERVICE_NAME" 2>/dev/null
}

is_service_enabled() {
  systemctl is-enabled --quiet "$SERVICE_NAME" 2>/dev/null
}

# ── Detect distro ───────────────────────────────────────────────────────────
detect_distro() {
  if [[ ! -r /etc/os-release ]]; then
    die "Cannot detect distribution — /etc/os-release missing."
  fi
  . /etc/os-release
  case "${ID:-}" in
    debian|raspbian|ubuntu|pop|linuxmint) ok "Detected: $PRETTY_NAME" ;;
    *) warn "Untested distro: ${PRETTY_NAME:-unknown}. Proceeding with apt..." ;;
  esac
  need_cmd apt-get
}

# ── Install apt dependencies ────────────────────────────────────────────────
APT_PACKAGES=(
  build-essential     # gcc, make
  cmake
  pkg-config
  curl                # fetch-vendor.sh
  ca-certificates
  libcups2-dev        # CUPS development headers
  libssl-dev          # OpenSSL for mongoose TLS
  cups                # CUPS daemon
  cups-client         # lpadmin, lpinfo, lpstat
  cups-bsd            # lpr, lpq, cancel
  openssl             # cert generation CLI
)

install_apt_packages() {
  log "Checking apt packages..."

  local to_install=()
  for pkg in "${APT_PACKAGES[@]}"; do
    if dpkg -s "$pkg" >/dev/null 2>&1; then
      ok "$pkg"
    else
      to_install+=("$pkg")
    fi
  done

  if [[ ${#to_install[@]} -eq 0 ]]; then
    ok "All apt packages already installed."
    return
  fi

  log "Installing missing packages: ${to_install[*]}"
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y --no-install-recommends "${to_install[@]}"
  ok "apt packages installed."
}

# ── Ensure CUPS is running ──────────────────────────────────────────────────
ensure_cups_running() {
  log "Checking CUPS daemon..."
  if ! systemctl is-enabled --quiet cups 2>/dev/null; then
    systemctl enable cups >/dev/null
    ok "Enabled cups.service on boot."
  else
    ok "cups.service already enabled on boot."
  fi

  if ! systemctl is-active --quiet cups 2>/dev/null; then
    systemctl start cups
    ok "Started cups.service."
  else
    ok "cups.service already running."
  fi
}

# ── Add current invoking user to lpadmin group ──────────────────────────────
ensure_lpadmin_group() {
  local user="${SUDO_USER:-}"
  if [[ -z "$user" || "$user" == "root" ]]; then
    return
  fi
  if id -nG "$user" | tr ' ' '\n' | grep -qx lpadmin; then
    ok "User '$user' already in 'lpadmin' group."
  else
    usermod -a -G lpadmin "$user" || warn "Could not add $user to lpadmin group."
    ok "Added user '$user' to 'lpadmin' group (re-login to take effect)."
  fi
}

# ── Fetch vendored deps ─────────────────────────────────────────────────────
fetch_vendor() {
  log "Fetching vendored dependencies (mongoose, cJSON, stb_image)..."
  if [[ -f "$VENDOR_DIR/mongoose.c" && \
        -f "$VENDOR_DIR/cJSON.c"    && \
        -f "$VENDOR_DIR/stb_image.h" ]]; then
    ok "Vendor files already present."
    return
  fi
  chmod +x "$SCRIPT_DIR/scripts/fetch-vendor.sh"
  sudo -u "${SUDO_USER:-$USER}" "$SCRIPT_DIR/scripts/fetch-vendor.sh" \
    || "$SCRIPT_DIR/scripts/fetch-vendor.sh"
  ok "Vendor files fetched."
}

# ── Build ───────────────────────────────────────────────────────────────────
build_binary() {
  log "Configuring build..."
  mkdir -p "$BUILD_DIR"
  (cd "$BUILD_DIR" && cmake -DCMAKE_BUILD_TYPE=Release .. >/dev/null)
  ok "CMake configured."

  log "Compiling (this takes ~30 s on a Pi 4)..."
  local jobs
  jobs="$(nproc 2>/dev/null || echo 2)"
  (cd "$BUILD_DIR" && make -j"$jobs")
  ok "Build complete: $BUILD_DIR/printer-server"
}

# ── Install ─────────────────────────────────────────────────────────────────
install_files() {
  log "Installing files..."

  install -m 0755 -D "$BUILD_DIR/printer-server" "$BIN_PATH"
  ok "Installed $BIN_PATH"

  mkdir -p "$CONFIG_DIR"
  if [[ ! -f "$CONFIG_FILE" ]]; then
    install -m 0644 -D "$SCRIPT_DIR/config.json" "$CONFIG_FILE"
    ok "Installed $CONFIG_FILE"
  else
    ok "Config already exists at $CONFIG_FILE (not overwritten)"
  fi

  # Cert directory — created now so first boot has a writable home for openssl.
  mkdir -p "$CONFIG_DIR/certs"
  chmod 755 "$CONFIG_DIR/certs"
  ok "Cert directory: $CONFIG_DIR/certs"

  # Static files
  mkdir -p "$WWW_DIR"
  cp -r "$SCRIPT_DIR/www/." "$WWW_DIR/"
  ok "Installed static files to $WWW_DIR"

  # Systemd unit
  install -m 0644 -D "$SCRIPT_DIR/systemd/${SERVICE_NAME}.service" "$SERVICE_FILE"
  systemctl daemon-reload
  ok "Installed $SERVICE_FILE"
}

# ── Enable + start service ──────────────────────────────────────────────────
enable_service() {
  log "Enabling service to start on every boot..."
  systemctl enable "$SERVICE_NAME" >/dev/null
  ok "Service will start automatically on boot."

  log "Starting service now..."
  systemctl restart "$SERVICE_NAME"
  sleep 1

  if is_service_active; then
    ok "Service is running."
  else
    err "Service failed to start. Showing last 30 log lines:"
    journalctl -u "$SERVICE_NAME" -n 30 --no-pager || true
    exit 1
  fi
}

# ── Uninstall ───────────────────────────────────────────────────────────────
uninstall() {
  log "Stopping and disabling service..."
  systemctl stop    "$SERVICE_NAME" 2>/dev/null || true
  systemctl disable "$SERVICE_NAME" 2>/dev/null || true

  log "Removing installed files..."
  rm -f  "$BIN_PATH"
  rm -f  "$SERVICE_FILE"
  rm -rf "$WWW_DIR"
  systemctl daemon-reload

  warn "Config at $CONFIG_DIR preserved (contains TLS cert). Remove manually if desired:"
  warn "    sudo rm -rf $CONFIG_DIR"
  ok   "Uninstalled."
}

# ── Status ──────────────────────────────────────────────────────────────────
show_status() {
  printf "\n%s%sRaspberry Pi Printer Server — status%s\n" "$C_BOLD" "$C_BLU" "$C_RST"
  printf "  %s\n" "─────────────────────────────────────────"

  if [[ -x "$BIN_PATH" ]]; then
    ok "Binary:     $BIN_PATH"
  else
    warn "Binary:     not installed"
  fi

  if [[ -f "$CONFIG_FILE" ]]; then
    ok "Config:     $CONFIG_FILE"
  else
    warn "Config:     missing"
  fi

  if [[ -f "$SERVICE_FILE" ]]; then
    ok "Service:    installed"
    if is_service_enabled; then ok "Enabled:    yes (starts on boot)"; else warn "Enabled:    no"; fi
    if is_service_active;  then ok "Running:    yes";                 else warn "Running:    no"; fi
  else
    warn "Service:    not installed"
  fi
  echo
}

# ── Pretty summary at end ───────────────────────────────────────────────────
summary() {
  local ips port https_port
  port=$(grep -E '"port"'      "$CONFIG_FILE" | sed -E 's/[^0-9]//g')
  https_port=$(grep -E '"httpsPort"' "$CONFIG_FILE" | sed -E 's/[^0-9]//g')
  port="${port:-5123}"
  https_port="${https_port:-5124}"

  # Read local IPv4 addresses, one per line (no loopback).
  ips="$(hostname -I 2>/dev/null | tr ' ' '\n' | grep -E '^[0-9]+\.' || true)"

  printf "\n%s%sInstallation complete.%s\n\n" "$C_GRN$C_BOLD" "" "$C_RST"
  printf "  Service: %s (enabled on boot)\n" "$SERVICE_NAME"
  printf "  Logs   : %s\n" "journalctl -u $SERVICE_NAME -f"
  printf "  Config : %s\n" "$CONFIG_FILE"
  printf "\n  Access URLs:\n"
  printf "    http://localhost:%s/api/discover\n" "$port"
  printf "    https://localhost:%s/api/discover  (self-signed)\n" "$https_port"
  if [[ -n "$ips" ]]; then
    while IFS= read -r ip; do
      [[ -n "$ip" ]] || continue
      printf "    http://%s:%s/api/discover\n"  "$ip" "$port"
      printf "    https://%s:%s/api/discover\n" "$ip" "$https_port"
    done <<<"$ips"
  fi
  printf "\n  Quick test:\n"
  printf "    curl -s http://localhost:%s/api/printers | head -c 200\n" "$port"
  printf "\n"
}

# ── Argument parsing ────────────────────────────────────────────────────────
FORCE=false
MODE="install"
for arg in "$@"; do
  case "$arg" in
    --force)     FORCE=true     ;;
    --uninstall) MODE="uninstall";;
    --status)    MODE="status"  ;;
    -h|--help)
      sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) die "Unknown argument: $arg  (try --help)" ;;
  esac
done

# ── Main ────────────────────────────────────────────────────────────────────
case "$MODE" in
  status)
    show_status
    exit 0
    ;;
  uninstall)
    require_root "$@"
    uninstall
    exit 0
    ;;
  install)
    require_root "$@"

    log "Raspberry Pi Printer Server — setup"

    # Early exit if already up and not forcing.
    if ! $FORCE && is_service_active && [[ -x "$BIN_PATH" ]]; then
      ok "Service is already installed and running."
      ok "Re-run with --force to rebuild and reinstall."
      show_status
      summary
      exit 0
    fi

    detect_distro
    install_apt_packages
    ensure_cups_running
    ensure_lpadmin_group
    fetch_vendor
    build_binary
    install_files
    enable_service
    summary
    ;;
esac
