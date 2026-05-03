#!/usr/bin/env bash
# scripts/run_qemu.sh — QEMU-based safe kernel iteration without physical hardware
# qemu_test.sh — Test gpu_telem inside a QEMU VM without touching physical HW.
#
# Strategy: reuse the host kernel and initrd, mount a small ext4 image that
# contains the module + tools + a test init script, and boot QEMU with that
# image as the root device.  The VM runs the tests then powers off.
#
# Prerequisites (Debian/Ubuntu):
#   apt-get install qemu-system-x86 qemu-utils e2fsprogs
#
# The module must be built for the SAME kernel running in QEMU.
# When using the host kernel (default), just run 'make module' first.
#
# Usage:
#   ./scripts/qemu_test.sh                    # use host kernel + initrd
#   KERNEL=/path/vmlinuz INITRD=/path/initrd ./scripts/qemu_test.sh
#   ./scripts/qemu_test.sh --no-kvm           # force TCG (no KVM)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${SCRIPT_DIR}/.."

# ---------- configuration ---------------------------------------------------
KERNEL="${KERNEL:-/boot/vmlinuz-$(uname -r)}"
INITRD="${INITRD:-/boot/initrd.img-$(uname -r)}"
MODULE="${ROOT}/kernel/gpu_telem.ko"
READER="${ROOT}/user/gpu_telem_reader"
DISK_IMG="/tmp/gpu_telem_qemu_test.img"
MOUNT_DIR="/tmp/gpu_telem_qemu_mnt"
DISK_MB=64
MEM_MB=512

USE_KVM=1
for arg in "$@"; do
    [[ "$arg" == "--no-kvm" ]] && USE_KVM=0
done

# ---------- prerequisites ---------------------------------------------------
check() {
    local cmd="$1" pkg="${2:-$1}"
    if ! command -v "$cmd" &>/dev/null; then
        echo "ERROR: '$cmd' not found.  Install: apt-get install $pkg"
        exit 1
    fi
}
check qemu-system-x86_64 qemu-system-x86
check mkfs.ext4             e2fsprogs

[[ -f "$MODULE" ]] || { echo "Build the module first: make module"; exit 1; }
[[ -f "$KERNEL" ]] || { echo "Kernel not found: $KERNEL"; exit 1; }
[[ -f "$INITRD" ]] || { echo "Initrd not found: $INITRD"; exit 1; }

# ---------- build test disk image -------------------------------------------
echo "==> Creating test disk image (${DISK_MB}M)..."
rm -f "$DISK_IMG"
dd if=/dev/zero of="$DISK_IMG" bs=1M count="$DISK_MB" status=none
mkfs.ext4 -q -L gpu_telem_test "$DISK_IMG"

mkdir -p "$MOUNT_DIR"
sudo mount -o loop "$DISK_IMG" "$MOUNT_DIR"

sudo cp "$MODULE" "${MOUNT_DIR}/gpu_telem.ko"
[[ -f "$READER" ]] && sudo cp "$READER" "${MOUNT_DIR}/reader"

# Write the test init script that runs inside the VM
sudo tee "${MOUNT_DIR}/test_init.sh" > /dev/null << 'INIT_EOF'
#!/bin/sh
# This runs as init inside the QEMU VM.
set -e
echo ""
echo "====================================================="
echo " gpu_telem QEMU functional test"
echo "====================================================="
echo ""

# Minimal environment
mount -t proc  proc  /proc  2>/dev/null || true
mount -t sysfs sysfs /sys   2>/dev/null || true

# Mount our test disk (the rootfs we booted from)
# The disk is /dev/vda; modules and tools are in /
echo "[1] Loading module..."
insmod /gpu_telem.ko sample_ms=200 ring_size=64
sleep 0.5

echo "[2] Checking device node..."
if [ -e /dev/gpu_telem ]; then
    echo "  PASS: /dev/gpu_telem exists"
else
    echo "  FAIL: /dev/gpu_telem not created"
    dmesg | tail -10
    poweroff -f
fi

echo "[3] dmesg:"
dmesg | grep gpu_telem

echo ""
echo "[4] Streaming 5 samples..."
if [ -x /reader ]; then
    /reader -n 5 /dev/gpu_telem
else
    # Fallback: read raw bytes with dd and show count
    BYTES=$(dd if=/dev/gpu_telem bs=48 count=5 2>/dev/null | wc -c)
    echo "  Read $BYTES bytes (expected 240)"
    [ "$BYTES" -eq 240 ] && echo "  PASS" || echo "  FAIL"
fi

echo ""
echo "[5] Two concurrent readers..."
/reader -n 3 /dev/gpu_telem > /tmp/r1.txt 2>&1 &
/reader -n 3 /dev/gpu_telem > /tmp/r2.txt 2>&1 &
wait
A=$(grep -c '^\[' /tmp/r1.txt || echo 0)
B=$(grep -c '^\[' /tmp/r2.txt || echo 0)
echo "  Reader A: $A samples   Reader B: $B samples"
[ "$A" -ge 3 ] && [ "$B" -ge 3 ] && echo "  PASS" || echo "  FAIL"

echo ""
echo "[6] Unloading module..."
rmmod gpu_telem
sleep 0.2
lsmod | grep -q gpu_telem && echo "  FAIL: still loaded" || echo "  PASS: unloaded"
dmesg | grep "gpu_telem: unloaded" && echo "  PASS: clean exit message" || true

echo ""
echo "====================================================="
echo " Test complete"
echo "====================================================="
poweroff -f
INIT_EOF

sudo chmod +x "${MOUNT_DIR}/test_init.sh"
sudo umount "$MOUNT_DIR"
rmdir "$MOUNT_DIR"

echo "==> Disk image ready.  Booting QEMU..."
echo ""

# ---------- boot QEMU -------------------------------------------------------
KVM_FLAGS=()
if [[ $USE_KVM -eq 1 ]] && [[ -e /dev/kvm ]]; then
    KVM_FLAGS+=(-enable-kvm -cpu host)
    echo "    KVM acceleration enabled"
else
    echo "    KVM not available — using TCG (slow but functional)"
fi

set +e
qemu-system-x86_64 \
    "${KVM_FLAGS[@]}" \
    -kernel "$KERNEL" \
    -initrd "$INITRD" \
    -drive  "file=${DISK_IMG},format=raw,if=virtio,cache=none" \
    -append "root=/dev/vda rw console=ttyS0 quiet loglevel=4 init=/test_init.sh" \
    -m "${MEM_MB}M" \
    -nographic \
    -no-reboot \
    -serial  stdio \
    -monitor none \
    2>&1
EXIT=$?
set -e

# ---------- cleanup ---------------------------------------------------------
rm -f "$DISK_IMG"
echo ""
echo "QEMU exited with code $EXIT."
[[ $EXIT -eq 0 ]]
