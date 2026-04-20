#!/usr/bin/env bash
#
# harden-cups.sh — Apply CUPS queue-hardening settings.
#
# Why: The CUPS defaults (retry-job, 5s retry, no MaxJobTime) cause stuck
# jobs on flaky network printers to pile up indefinitely. These settings
# cancel failed jobs, limit retries, and auto-abort stuck jobs.
#
# Usage:   sudo bash scripts/harden-cups.sh
# Safe to run multiple times — idempotent.

set -euo pipefail

CONF=/etc/cups/cupsd.conf
MARK="# === printer-server CUPS hardening ==="

if [ ! -f "$CONF" ]; then
    echo "ERROR: $CONF not found. Is CUPS installed?" >&2
    exit 1
fi

if grep -qF "$MARK" "$CONF"; then
    echo "[harden-cups] Already applied — skipping."
    exit 0
fi

cp "$CONF" "$CONF.bak-$(date +%s)"

# Flip existing ErrorPolicy if present, else append.
if grep -q "^ErrorPolicy" "$CONF"; then
    sed -i 's/^ErrorPolicy .*/ErrorPolicy abort-job/' "$CONF"
fi

cat >> "$CONF" <<EOF

$MARK
# Fail fast instead of retrying stuck jobs forever.
ErrorPolicy abort-job
JobKillDelay 30
Timeout 30
JobRetryInterval 30
JobRetryLimit 3
MaxJobTime 120
MaxHoldTime 300
PreserveJobFiles No
PreserveJobHistory No
# ===================================
EOF

# Cancel any currently-stuck jobs and restart.
cancel -a || true
systemctl restart cups
sleep 1
systemctl is-active cups

echo "[harden-cups] Applied. Key settings:"
grep -E "^(ErrorPolicy|Timeout|JobRetry|JobKillDelay|MaxJobTime|MaxHoldTime|PreserveJob)" "$CONF"
