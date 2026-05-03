#!/usr/bin/env bash
# load.sh — insmod / rmmod / reload helper for gpu_telem
#
# Usage:
#   ./scripts/load.sh [load|unload|reload] [module_param=value ...]
#
# Examples:
#   ./scripts/load.sh load sample_ms=200 ring_size=512
#   ./scripts/load.sh reload
#   ./scripts/load.sh unload

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE="${SCRIPT_DIR}/../gpu_telem.ko"
DEVICE=/dev/gpu_telem
CMD="${1:-load}"

need_sudo() {
    if [[ $EUID -ne 0 ]]; then
        echo "Running with sudo..."
        exec sudo "$0" "$@"
    fi
}

do_unload() {
    if lsmod | grep -q '^gpu_telem '; then
        rmmod gpu_telem
        echo "gpu_telem unloaded."
    else
        echo "gpu_telem is not loaded."
    fi
}

do_load() {
    if [[ ! -f "$MODULE" ]]; then
        echo "ERROR: $MODULE not found. Run 'make' first."
        exit 1
    fi

    if lsmod | grep -q '^gpu_telem '; then
        echo "Already loaded — unloading first..."
        rmmod gpu_telem
        sleep 0.2
    fi

    insmod "$MODULE" "${@}"

    # udev may not have created the node yet; poll briefly
    for i in $(seq 1 10); do
        [[ -e "$DEVICE" ]] && break
        sleep 0.1
    done

    if [[ -e "$DEVICE" ]]; then
        chmod a+rw "$DEVICE"
        echo "Loaded.  $DEVICE is ready."
    else
        echo "ERROR: $DEVICE was not created."
        dmesg | tail -10
        exit 1
    fi

    echo ""
    echo "--- dmesg tail ---"
    dmesg | grep gpu_telem | tail -5
}

case "$CMD" in
    load)
        need_sudo "$@"
        do_load "${@:2}"
        ;;
    unload)
        need_sudo "$@"
        do_unload
        ;;
    reload)
        need_sudo "$@"
        do_unload || true
        do_load "${@:2}"
        ;;
    *)
        echo "Usage: $0 [load|unload|reload] [module params...]"
        exit 1
        ;;
esac
