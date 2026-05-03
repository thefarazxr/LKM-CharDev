#!/usr/bin/env bash
# scripts/unload.sh — safely unload gpu_telem kernel module
#
# The driver's cleanup path (gpu_telem_exit) stops the collector kthread,
# destroys the cdev/class (removes /dev/gpu_telem), then frees the ring.
# Any process still blocked in read() will be woken with -EIO.
#
# Usage:
#   ./scripts/unload.sh
#   sudo ./scripts/unload.sh          (if not running as root)

set -euo pipefail

[[ $EUID -ne 0 ]] && exec sudo "$0" "$@"

if ! lsmod | grep -q '^gpu_telem '; then
    echo "gpu_telem is not loaded."
    exit 0
fi

OPEN_FDS=$(lsof /dev/gpu_telem 2>/dev/null | tail -n +2 | wc -l || echo 0)
if [[ "$OPEN_FDS" -gt 0 ]]; then
    echo "Warning: $OPEN_FDS process(es) still have /dev/gpu_telem open."
    echo "They will receive -EIO on their next read()."
fi

rmmod gpu_telem

echo "gpu_telem unloaded."
echo ""
echo "--- dmesg tail ---"
dmesg | grep gpu_telem | tail -5
