#!/usr/bin/env bash
# probe_gpu.sh — Discover kernel-visible GPU telemetry paths
#
# Run this BEFORE loading the module to figure out which hwmon device
# corresponds to your GPU and which attributes are actually exposed.
# Pass the output path to insmod as hwmon_path=<path>.
#
# Example output:
#   /sys/class/hwmon/hwmon2  (name: nvidia)
#     temp1_input    = 52000       (52.0 °C)
#     fan1_input     = 1800        (RPM)
#     power1_average = 35000000    (35 W)

set -euo pipefail

FOUND_GPU=0

echo "============================================================"
echo " GPU sysfs / hwmon telemetry probe"
echo "============================================================"
echo ""

# ---- hwmon devices -------------------------------------------------------
echo "[ hwmon devices ]"
for hwmon_dir in /sys/class/hwmon/hwmon*/; do
    [[ -d "$hwmon_dir" ]] || continue
    name="$(cat "${hwmon_dir}name" 2>/dev/null || echo unknown)"
    is_gpu=0
    [[ "$name" =~ ^(nvidia|nouveau|amdgpu|radeon)$ ]] && is_gpu=1

    marker=""
    [[ $is_gpu -eq 1 ]] && marker=" ◄ GPU"
    printf "  %s  (name: %s)%s\n" "$hwmon_dir" "$name" "$marker"

    attrs=(temp1_input temp2_input temp3_input
           fan1_input fan2_input
           power1_average power1_input power2_average
           freq1_input in0_input curr1_input)

    for attr in "${attrs[@]}"; do
        path="${hwmon_dir}${attr}"
        if [[ -f "$path" ]]; then
            val="$(cat "$path" 2>/dev/null || echo '?')"
            case "$attr" in
                temp*_input)
                    note="$(awk "BEGIN{printf \"%.1f °C\", $val/1000}" 2>/dev/null || true)";;
                power*_average|power*_input)
                    note="$(awk "BEGIN{printf \"%.2f W\", $val/1e6}" 2>/dev/null || true)";;
                freq*_input)
                    note="$(awk "BEGIN{printf \"%.0f MHz\", $val/1e6}" 2>/dev/null || true)";;
                fan*_input)
                    note="RPM";;
                *)
                    note="";;
            esac
            printf "    %-22s = %-14s %s\n" "$attr" "$val" "$note"
        fi
    done

    if [[ $is_gpu -eq 1 ]]; then
        FOUND_GPU=1
        echo ""
        echo "  → insmod tip for this device:"
        echo "    sudo insmod gpu_telem.ko hwmon_path=${hwmon_dir%/}"
    fi
    echo ""
done

# ---- DRM / GPU frequency (Intel / AMD / some NVIDIA via nouveau) ----------
echo "[ DRM devices ]"
for card_dir in /sys/class/drm/card[0-9]*/; do
    [[ -d "$card_dir" ]] || continue
    printf "  %s\n" "$card_dir"
    drm_attrs=(gt_cur_freq_mhz gt_max_freq_mhz gt_min_freq_mhz
               device/power/power1_average)
    for attr in "${drm_attrs[@]}"; do
        path="${card_dir}${attr}"
        [[ -f "$path" ]] || continue
        val="$(cat "$path" 2>/dev/null || echo '?')"
        printf "    %-30s = %s\n" "$attr" "$val"
    done
    echo ""
done

# ---- NVIDIA proprietary driver -------------------------------------------
echo "[ NVIDIA proprietary driver ]"
if [[ -d /proc/driver/nvidia ]]; then
    echo "  /proc/driver/nvidia present"
    ls /proc/driver/nvidia/ 2>/dev/null | sed 's/^/    /' || true
    if command -v nvidia-smi &>/dev/null; then
        echo ""
        echo "  nvidia-smi summary:"
        nvidia-smi --query-gpu=name,temperature.gpu,fan.speed,power.draw,clocks.gr,clocks.mem \
                   --format=csv,noheader 2>/dev/null | sed 's/^/    /' || true
    fi
else
    echo "  /proc/driver/nvidia not found (NVIDIA proprietary driver not loaded)"
fi
echo ""

# ---- Summary -------------------------------------------------------------
echo "============================================================"
if [[ $FOUND_GPU -eq 1 ]]; then
    echo " Found GPU hwmon.  Use the 'insmod tip' above when loading."
else
    echo " No GPU hwmon detected.  Module will still load and the"
    echo " ring-buffer / chardev pipeline will work with sentinel values."
    echo " In QEMU this is expected and normal."
fi
echo "============================================================"
