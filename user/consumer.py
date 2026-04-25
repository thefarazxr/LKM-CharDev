#!/usr/bin/env python3
"""
consumer.py — Python user-space consumer for /dev/gpu_telem

Producer: gpu_telem.ko (kernel module)
  kthread → ring buffer → /dev/gpu_telem (chardev)

Consumer: this script
  select.poll() → read() → struct.unpack() → display / JSON / CSV

The kernel writes struct gpu_sample (48 bytes) into the ring buffer.
We decode each sample using the same layout defined in gpu_telem.h:

  offset  size  field
  ------  ----  -----
       0     8  timestamp_ns      (u64, ktime_get_ns())
       8     4  temp_millideg     (s32, millidegrees C; -1 = unavailable)
      12     4  fan_rpm           (u32; 0 = unavailable)
      16     8  power_uw          (u64, microwatts; 0 = unavailable)
      24     4  core_freq_mhz     (u32; 0 = unavailable)
      28     4  mem_freq_mhz      (u32; 0 = unavailable)
      32     4  gpu_util_pct      (u32; 0xFFFFFFFF = unavailable)
      36     4  mem_util_pct      (u32; 0xFFFFFFFF = unavailable)
      40     1  throttle_reasons  (u8 bitmask)
      41     7  _pad
  total = 48 bytes

Usage:
  python3 consumer.py                     # stream to stdout (table)
  python3 consumer.py --json              # newline-delimited JSON
  python3 consumer.py --csv              # CSV (header + rows)
  python3 consumer.py -n 10              # print 10 samples then exit
  python3 consumer.py --set-interval 200 # set kernel collection interval to 200 ms
  python3 consumer.py --device /dev/gpu_telem
"""

import argparse
import csv
import fcntl
import json
import os
import select
import signal
import struct
import sys
import time
from dataclasses import asdict, dataclass
from typing import Iterator, Optional

# ── struct layout ────────────────────────────────────────────────────────────
# '<' = little-endian (same byte order the kernel writes on x86/arm)
# Q=u64  i=s32  I=u32  B=u8  7x=7 padding bytes
_FMT  = "<Q i I Q I I I I B 7x"
_SIZE = struct.calcsize(_FMT)   # must be 48
assert _SIZE == 48, f"unexpected struct size {_SIZE}"

# ── ioctl numbers (mirrors gpu_telem.h) ──────────────────────────────────────
def _IOC(direction: int, type_: int, nr: int, size: int) -> int:
    return (direction << 30) | (size << 16) | (type_ << 8) | nr

_MAGIC       = ord('G')
_IOC_READ    = 2
_IOC_WRITE   = 1
_UINT32_SIZE = 4

IOCTL_GET_RING_SIZE   = _IOC(_IOC_READ,  _MAGIC, 0, _UINT32_SIZE)
IOCTL_GET_INTERVAL_MS = _IOC(_IOC_READ,  _MAGIC, 1, _UINT32_SIZE)
IOCTL_SET_INTERVAL_MS = _IOC(_IOC_WRITE, _MAGIC, 2, _UINT32_SIZE)

# ── throttle bitmask ─────────────────────────────────────────────────────────
_THROTTLE = {
    0x01: "POWER",
    0x02: "THERMAL",
    0x04: "CURRENT",
    0x08: "UTIL",
}

# ── data model ───────────────────────────────────────────────────────────────
@dataclass
class GpuSample:
    timestamp_ns:     int
    temp_millideg:    int      # -1 = unavailable
    fan_rpm:          int      # 0  = unavailable
    power_uw:         int      # 0  = unavailable
    core_freq_mhz:    int      # 0  = unavailable
    mem_freq_mhz:     int      # 0  = unavailable
    gpu_util_pct:     int      # 0xFFFFFFFF = unavailable
    mem_util_pct:     int      # 0xFFFFFFFF = unavailable
    throttle_reasons: int

    @classmethod
    def unpack(cls, raw: bytes) -> "GpuSample":
        fields = struct.unpack(_FMT, raw)
        return cls(*fields)

    # ── derived helpers ──

    @property
    def temp_c(self) -> Optional[float]:
        return None if self.temp_millideg == -1 else self.temp_millideg / 1000.0

    @property
    def power_w(self) -> Optional[float]:
        return None if self.power_uw == 0 else self.power_uw / 1e6

    @property
    def gpu_util(self) -> Optional[int]:
        return None if self.gpu_util_pct == 0xFFFF_FFFF else self.gpu_util_pct

    @property
    def mem_util(self) -> Optional[int]:
        return None if self.mem_util_pct == 0xFFFF_FFFF else self.mem_util_pct

    @property
    def throttle_names(self) -> list[str]:
        return [name for bit, name in _THROTTLE.items()
                if self.throttle_reasons & bit]

    def to_dict(self) -> dict:
        """Human-friendly dict for JSON/CSV output."""
        return {
            "timestamp_ns":   self.timestamp_ns,
            "temp_c":         self.temp_c,
            "fan_rpm":        self.fan_rpm or None,
            "power_w":        self.power_w,
            "core_freq_mhz":  self.core_freq_mhz or None,
            "mem_freq_mhz":   self.mem_freq_mhz or None,
            "gpu_util_pct":   self.gpu_util,
            "mem_util_pct":   self.mem_util,
            "throttle":       self.throttle_names,
        }

# ── device wrapper ────────────────────────────────────────────────────────────
class GpuTelemReader:
    """
    Wraps /dev/gpu_telem.

    The kernel ring buffer is the *producer*; each open() fd maintains its own
    read position so multiple GpuTelemReader instances get independent streams.
    """

    BATCH = 16  # samples per read() call

    def __init__(self, device: str = "/dev/gpu_telem"):
        try:
            self._fd = os.open(device, os.O_RDONLY)
        except OSError as e:
            hint = "\n  Hint: sudo insmod gpu_telem.ko" if e.errno == 2 else ""
            raise SystemExit(f"open {device}: {e}{hint}") from e

        self._poll = select.poll()
        self._poll.register(self._fd, select.POLLIN)

    def close(self) -> None:
        os.close(self._fd)

    # ── ioctl helpers ──

    def ring_size(self) -> int:
        buf = bytearray(4)
        fcntl.ioctl(self._fd, IOCTL_GET_RING_SIZE, buf)
        return struct.unpack("I", buf)[0]

    def interval_ms(self) -> int:
        buf = bytearray(4)
        fcntl.ioctl(self._fd, IOCTL_GET_INTERVAL_MS, buf)
        return struct.unpack("I", buf)[0]

    def set_interval_ms(self, ms: int) -> None:
        buf = struct.pack("I", ms)
        fcntl.ioctl(self._fd, IOCTL_SET_INTERVAL_MS, buf)

    # ── sample stream ──

    def samples(self, timeout_ms: int = 1500) -> Iterator[GpuSample]:
        """
        Yield GpuSample objects as the kernel produces them.

        Uses poll() so the thread sleeps in the kernel wait_queue until data
        arrives — zero CPU overhead between samples.
        """
        buf = bytearray(self.BATCH * _SIZE)
        view = memoryview(buf)

        while True:
            ready = self._poll.poll(timeout_ms)
            if not ready:
                continue   # timeout — check again (allows KeyboardInterrupt)

            n = os.readv(self._fd, [view])
            if n == 0:
                return

            count = n // _SIZE
            for i in range(count):
                chunk = buf[i * _SIZE : (i + 1) * _SIZE]
                yield GpuSample.unpack(bytes(chunk))

# ── formatters ────────────────────────────────────────────────────────────────
_HDR = ("  {:>6}  {:>9}  {:>7}  {:>8}  {:>9}  {:>8}  {:>7}  {:>7}  {}"
        .format("idx", "temp_°C", "fan_rpm", "power_W",
                "core_MHz", "mem_MHz", "gpu_%", "mem_%", "throttle"))
_SEP = "  " + "─" * (len(_HDR) - 2)

def _fmt_opt(val, fmt="{}", none="n/a"):
    return none if val is None else fmt.format(val)

def print_table_row(idx: int, s: GpuSample) -> None:
    throttle = ",".join(s.throttle_names) or "-"
    print("  {:>6}  {:>9}  {:>7}  {:>8}  {:>9}  {:>8}  {:>7}  {:>7}  {}".format(
        idx,
        _fmt_opt(s.temp_c,       "{:.1f}"),
        _fmt_opt(s.fan_rpm or None, "{}"),
        _fmt_opt(s.power_w,      "{:.3f}"),
        _fmt_opt(s.core_freq_mhz or None, "{}"),
        _fmt_opt(s.mem_freq_mhz  or None, "{}"),
        _fmt_opt(s.gpu_util,     "{}"),
        _fmt_opt(s.mem_util,     "{}"),
        throttle,
    ))

# ── main ──────────────────────────────────────────────────────────────────────
def main() -> None:
    ap = argparse.ArgumentParser(
        description="Python consumer for /dev/gpu_telem (kernel ring-buffer producer)")
    ap.add_argument("--device", default="/dev/gpu_telem",
                    metavar="PATH", help="character device path")
    ap.add_argument("-n", "--count", type=int, default=0,
                    metavar="N", help="exit after N samples (0 = stream forever)")
    ap.add_argument("--json",  action="store_true", help="newline-delimited JSON output")
    ap.add_argument("--csv",   action="store_true", help="CSV output")
    ap.add_argument("--set-interval", type=int, default=0,
                    metavar="MS", help="set kernel collection interval (ms) then exit")
    args = ap.parse_args()

    reader = GpuTelemReader(args.device)

    # One-shot: set interval and exit
    if args.set_interval:
        reader.set_interval_ms(args.set_interval)
        print(f"Collection interval set to {args.set_interval} ms.")
        reader.close()
        return

    ring_sz   = reader.ring_size()
    interval  = reader.interval_ms()

    running = True
    signal.signal(signal.SIGINT,  lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))

    # ── output mode: CSV ──
    if args.csv:
        fields = ["index", "timestamp_ns", "temp_c", "fan_rpm", "power_w",
                  "core_freq_mhz", "mem_freq_mhz", "gpu_util_pct",
                  "mem_util_pct", "throttle"]
        writer = csv.DictWriter(sys.stdout, fieldnames=fields)
        writer.writeheader()
        idx = 0
        try:
            for sample in reader.samples():
                d = sample.to_dict()
                d["index"] = idx
                d["throttle"] = "|".join(d["throttle"])
                writer.writerow(d)
                sys.stdout.flush()
                idx += 1
                if args.count and idx >= args.count:
                    break
        except KeyboardInterrupt:
            pass
        reader.close()
        return

    # ── output mode: JSON ──
    if args.json:
        idx = 0
        try:
            for sample in reader.samples():
                d = sample.to_dict()
                d["index"] = idx
                print(json.dumps(d))
                sys.stdout.flush()
                idx += 1
                if args.count and idx >= args.count:
                    break
        except KeyboardInterrupt:
            pass
        reader.close()
        return

    # ── output mode: table (default) ──
    print(f"Producer : gpu_telem.ko  ({args.device})")
    print(f"Ring     : {ring_sz} samples   Interval: {interval} ms")
    print(f"Consumer : {__file__}  (pid {os.getpid()})")
    print(f"Press Ctrl-C to stop.\n")
    print(_HDR)
    print(_SEP)

    idx = 0
    t_start = time.monotonic()
    try:
        for sample in reader.samples():
            print_table_row(idx, sample)
            sys.stdout.flush()
            idx += 1
            if args.count and idx >= args.count:
                break
    except KeyboardInterrupt:
        pass

    elapsed = time.monotonic() - t_start
    rate = idx / elapsed if elapsed > 0 else 0
    print(_SEP)
    print(f"\n  {idx} samples in {elapsed:.1f}s  ({rate:.2f} samples/s)")
    reader.close()


if __name__ == "__main__":
    main()
