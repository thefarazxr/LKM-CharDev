#!/usr/bin/env bash
# scripts/dmesg_watch.sh — watch gpu_telem kernel printk + dynamic debug output
#
# Three modes:
#   watch (default) — live tail of dmesg, filtered to gpu_telem lines
#   debug on        — enable dynamic debug (pr_debug) for the module
#   debug off       — disable dynamic debug
#
# Dynamic debug requires CONFIG_DYNAMIC_DEBUG=y in the kernel.
# It controls pr_debug() calls at runtime without reloading the module.
#
# Usage:
#   ./scripts/dmesg_watch.sh              # tail dmesg (Ctrl-C to stop)
#   ./scripts/dmesg_watch.sh debug on     # enable pr_debug() output
#   ./scripts/dmesg_watch.sh debug off    # disable pr_debug() output
#   ./scripts/dmesg_watch.sh dump         # one-shot: print all gpu_telem messages

set -euo pipefail

DYNDBG=/sys/kernel/debug/dynamic_debug/control
MODULE=gpu_telem

enable_dyndbg() {
    if [[ ! -f "$DYNDBG" ]]; then
        echo "Dynamic debug not available (CONFIG_DYNAMIC_DEBUG not set or debugfs not mounted)"
        echo "  Mount debugfs: mount -t debugfs debugfs /sys/kernel/debug"
        exit 1
    fi
    echo "module ${MODULE} +p" | sudo tee "$DYNDBG" > /dev/null
    echo "Dynamic debug enabled for module '${MODULE}'."
    echo "pr_debug() messages will appear in dmesg."
}

disable_dyndbg() {
    if [[ ! -f "$DYNDBG" ]]; then
        echo "Dynamic debug not available."
        exit 1
    fi
    echo "module ${MODULE} -p" | sudo tee "$DYNDBG" > /dev/null
    echo "Dynamic debug disabled for module '${MODULE}'."
}

CMD="${1:-watch}"

case "$CMD" in
    watch)
        echo "=== Watching dmesg for gpu_telem (Ctrl-C to stop) ==="
        # dmesg -w streams new kernel messages; we filter and highlight
        dmesg -w 2>/dev/null | grep --line-buffered --color=auto "${MODULE}" \
        || \
        # Fallback: poll-based watch for kernels without dmesg -w
        while true; do
            dmesg | grep "${MODULE}" | tail -20
            sleep 1
            clear
        done
        ;;
    debug)
        [[ $EUID -ne 0 ]] && exec sudo "$0" "$@"
        case "${2:-}" in
            on)  enable_dyndbg ;;
            off) disable_dyndbg ;;
            *)   echo "Usage: $0 debug [on|off]"; exit 1 ;;
        esac
        ;;
    dump)
        echo "=== gpu_telem dmesg messages ==="
        dmesg | grep "${MODULE}" || echo "(none found)"
        ;;
    *)
        echo "Usage: $0 [watch|debug on|debug off|dump]"
        exit 1
        ;;
esac
