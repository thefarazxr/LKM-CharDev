# GPU Kernel Telemetry Driver

A Linux loadable kernel module that builds a **kernel→user telemetry pipeline**:

```
sysfs/hwmon  →  kthread collector  →  ring buffer  →  /dev/gpu_telem  →  user app
```

Key concepts demonstrated: character device, file_operations, ring buffer with spinlock,
wait_queue blocking reads, poll() event-driven I/O, copy_to_user(), kthread lifecycle.

## Quick start

```bash
# 1. Discover what GPU sysfs paths exist on this machine
bash scripts/probe_gpu.sh

# 2. Build kernel module + user tools
make

# 3. Load the module (auto-probes hwmon, or pass hwmon_path= explicitly)
make load
# or: bash scripts/load.sh load hwmon_path=/sys/class/hwmon/hwmon2

# 4. Stream telemetry
./user/gpu_telem_reader           # C reader
python3 user/consumer.py          # Python reader (table)
python3 user/consumer.py --json   # JSON stream
python3 user/consumer.py --csv    # CSV stream

# 5. Plot a capture
python3 user/consumer.py --csv -n 300 > telem.csv
python3 user/plot_telem.py telem.csv

# 6. Compare with NVML (if NVIDIA driver installed)
./user/nvml_validate

# 7. Run functional tests
make test

# 8. Test safely in QEMU (no physical hardware needed)
make qemu

# 9. Unload
make unload
```

## Directory layout

```
├── include/
│   └── gpu_telem_uapi.h      shared kernel/user struct + ioctl definitions
│
├── kernel/
│   ├── gpu_telem_core.c      file_operations, blocking reads, wait queue
│   ├── gpu_telem.h           kernel-internal structs (reader_ctx, device state)
│   ├── ringbuf.c             ring buffer: spinlocks, producer/consumer races
│   ├── ringbuf.h             ring buffer API
│   └── Makefile              builds gpu_telem.ko
│
├── user/
│   ├── gpu_telem_reader.c    C consumer: poll() + read() + ioctl
│   ├── nvml_validate.cpp     C++ tool: kernel vs NVML side-by-side comparison
│   ├── consumer.py           Python consumer: struct.unpack, JSON/CSV output
│   ├── plot_telem.py         post-process CSV → matplotlib charts
│   └── Makefile
│
├── scripts/
│   ├── load.sh               insmod + auto-chmod /dev/gpu_telem
│   ├── unload.sh             rmmod with open-fd warning
│   ├── run_qemu.sh           boot QEMU VM for safe kernel iteration
│   ├── dmesg_watch.sh        tail dmesg / toggle dynamic debug (pr_debug)
│   ├── probe_gpu.sh          discover GPU sysfs paths before loading
│   └── test.sh               functional test suite (6 tests)
│
└── docs/
    ├── architecture.md       pipeline diagram + design decisions
    └── interview_notes.md    blocking vs non-blocking, spinlocks, copy_to_user, etc.
```

## Module parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `hwmon_path` | *(auto-probe)* | Base sysfs hwmon path, e.g. `/sys/class/hwmon/hwmon2` |
| `sample_ms` | `500` | Collection interval in milliseconds (min 10) |
| `ring_size` | `256` | Ring buffer capacity (power-of-2, max 4096) |

```bash
sudo insmod kernel/gpu_telem.ko sample_ms=100 ring_size=512
```

## ioctl interface

```c
#include "include/gpu_telem_uapi.h"

int fd = open("/dev/gpu_telem", O_RDONLY);

uint32_t ring_sz, interval;
ioctl(fd, GPU_TELEM_GET_RING_SIZE,   &ring_sz);    // read ring capacity
ioctl(fd, GPU_TELEM_GET_INTERVAL_MS, &interval);   // read current interval
ioctl(fd, GPU_TELEM_SET_INTERVAL_MS, &(uint32_t){200}); // set to 200 ms
```

```python
# Python equivalent
python3 user/consumer.py --set-interval 200
```

## Debugging

```bash
# Enable pr_debug() output (requires CONFIG_DYNAMIC_DEBUG=y)
bash scripts/dmesg_watch.sh debug on
bash scripts/dmesg_watch.sh watch        # live tail

# KASAN — enable in kernel config: CONFIG_KASAN=y CONFIG_KASAN_INLINE=y
# Then load the module normally; KASAN will report any memory errors.

# ftrace — trace function calls inside the module
echo gpu_telem_read > /sys/kernel/debug/tracing/set_ftrace_filter
echo function > /sys/kernel/debug/tracing/current_tracer
cat /sys/kernel/debug/tracing/trace_pipe
```

## Key insight: NVML vs kernel interface gap

On NVIDIA hardware, `nvidia.ko` exposes only a small subset of its internal
telemetry via generic sysfs/hwmon interfaces.  NVML accesses the full set
directly.  Run `nvml_validate` to see the gap concretely.

See `docs/architecture.md` for the full pipeline design and
`docs/interview_notes.md` for design tradeoff analysis.
