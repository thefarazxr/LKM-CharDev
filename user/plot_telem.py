#!/usr/bin/env python3
"""
user/plot_telem.py — Post-processing: plot GPU telemetry logs

Reads CSV output produced by consumer.py --csv and generates time-series
charts using matplotlib.  Designed as a post-processing step, not a live
reader — pipe consumer.py output into this script or read a saved CSV file.

Requires: pip install matplotlib pandas

Usage:
  # Capture then plot
  python3 consumer.py --csv -n 300 > telem.csv
  python3 plot_telem.py telem.csv

  # Live pipe (plot rendered when stream ends / Ctrl-C)
  python3 consumer.py --csv | python3 plot_telem.py

  # Save to PNG instead of showing interactively
  python3 plot_telem.py telem.csv --output report.png

  # Select specific metrics
  python3 plot_telem.py telem.csv --metrics temp power

  # Set figure title
  python3 plot_telem.py telem.csv --title "GPU stress test"
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path
from typing import Optional

# ── optional deps — give a clear error if missing ────────────────────────────
try:
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker
    from matplotlib.gridspec import GridSpec
except ImportError:
    sys.exit("matplotlib is required: pip install matplotlib")

try:
    import pandas as pd
except ImportError:
    sys.exit("pandas is required: pip install pandas")

# ── expected CSV columns (from consumer.py --csv) ────────────────────────────
_REQUIRED_COLS = {"timestamp_ns", "temp_c", "fan_rpm", "power_w",
                  "core_freq_mhz", "mem_freq_mhz", "gpu_util_pct",
                  "mem_util_pct", "throttle"}

# ── metric definitions ────────────────────────────────────────────────────────
METRICS = {
    "temp":  dict(col="temp_c",        label="Temperature (°C)",      color="#e05252"),
    "power": dict(col="power_w",       label="Power draw (W)",         color="#e09d52"),
    "fan":   dict(col="fan_rpm",       label="Fan speed (RPM)",        color="#52a8e0"),
    "core":  dict(col="core_freq_mhz", label="Core frequency (MHz)",   color="#52e07a"),
    "mem":   dict(col="mem_freq_mhz",  label="Memory frequency (MHz)", color="#9d52e0"),
    "gpu_util": dict(col="gpu_util_pct", label="GPU utilisation (%)", color="#e0c852"),
}

# ── data loading ──────────────────────────────────────────────────────────────

def load_csv(source) -> pd.DataFrame:
    """Read CSV from a file path, Path, or stdin."""
    if source == "-" or source is None:
        raw = sys.stdin.read()
        import io
        df = pd.read_csv(io.StringIO(raw))
    else:
        df = pd.read_csv(source)

    missing = _REQUIRED_COLS - set(df.columns)
    if missing:
        sys.exit(f"CSV missing columns: {missing}\n"
                 f"Generate with: python3 consumer.py --csv")

    # Convert timestamp to seconds relative to first sample
    df["time_s"] = (df["timestamp_ns"] - df["timestamp_ns"].iloc[0]) / 1e9

    # Sentinels → NaN so matplotlib draws gaps instead of flat lines at 0
    df["gpu_util_pct"] = pd.to_numeric(df["gpu_util_pct"], errors="coerce")
    df["mem_util_pct"] = pd.to_numeric(df["mem_util_pct"], errors="coerce")
    df.replace(0xFFFF_FFFF, float("nan"), inplace=True)

    return df


def has_real_data(df: pd.DataFrame, col: str) -> bool:
    """Returns False if the column is all NaN or all zero (sentinel)."""
    s = df[col].dropna()
    return not s.empty and s.abs().sum() > 0


# ── plotting ──────────────────────────────────────────────────────────────────

def make_figure(df: pd.DataFrame,
                selected: list[str],
                title: str,
                output: Optional[str]) -> None:

    # Filter to metrics with real data
    active = [m for m in selected
              if m in METRICS and has_real_data(df, METRICS[m]["col"])]

    if not active:
        print("No metrics with real data to plot.")
        print("On NVIDIA hardware without hwmon, all fields are sentinels.")
        print("Load the module on a machine with a supported GPU,")
        print("or override hwmon_path= when loading the module.")
        return

    n = len(active)
    fig = plt.figure(figsize=(12, 3 * n + 1))
    fig.suptitle(title, fontsize=14, fontweight="bold")
    gs  = GridSpec(n, 1, figure=fig, hspace=0.45)

    for i, key in enumerate(active):
        meta = METRICS[key]
        col  = meta["col"]
        ax   = fig.add_subplot(gs[i])

        ax.plot(df["time_s"], df[col],
                color=meta["color"], linewidth=1.2, alpha=0.85)
        ax.fill_between(df["time_s"], df[col],
                        alpha=0.12, color=meta["color"])

        ax.set_ylabel(meta["label"], fontsize=9)
        ax.xaxis.set_minor_locator(ticker.AutoMinorLocator())
        ax.yaxis.set_minor_locator(ticker.AutoMinorLocator())
        ax.grid(True, which="major", linestyle="--", alpha=0.4)
        ax.grid(True, which="minor", linestyle=":",  alpha=0.2)

        if i == n - 1:
            ax.set_xlabel("Time (seconds)", fontsize=9)
        else:
            ax.tick_params(labelbottom=False)

        # Annotate throttle events if this is the temperature panel
        if key == "temp" and "throttle" in df.columns:
            events = df[df["throttle"].notna() & (df["throttle"] != "") &
                        (df["throttle"] != "nan")]
            for _, ev in events.iterrows():
                ax.axvline(ev["time_s"], color="red",
                           linewidth=0.8, linestyle=":", alpha=0.7)

    # Annotation: note about NVML gap
    fig.text(0.5, 0.01,
             "Source: /dev/gpu_telem (kernel sysfs/hwmon pipeline)  |  "
             "GPU utilisation unavailable on NVIDIA (requires NVML)",
             ha="center", fontsize=7, color="grey")

    plt.tight_layout(rect=[0, 0.03, 1, 0.96])

    if output:
        plt.savefig(output, dpi=150, bbox_inches="tight")
        print(f"Saved: {output}")
    else:
        plt.show()


# ── CLI ───────────────────────────────────────────────────────────────────────

def main() -> None:
    ap = argparse.ArgumentParser(
        description="Plot GPU telemetry CSV (produced by consumer.py --csv)")
    ap.add_argument("source", nargs="?", default="-",
                    help="CSV file path, or '-' / omit for stdin")
    ap.add_argument("--output", "-o", metavar="FILE",
                    help="Save plot to PNG/PDF instead of displaying")
    ap.add_argument("--title",  default="GPU Telemetry",
                    help="Figure title")
    ap.add_argument("--metrics", nargs="+",
                    choices=list(METRICS), default=list(METRICS),
                    help="Metrics to plot (default: all)")
    args = ap.parse_args()

    source = None if args.source == "-" else args.source
    df = load_csv(source)

    n_samples = len(df)
    duration  = df["time_s"].iloc[-1] if n_samples > 1 else 0
    print(f"Loaded {n_samples} samples, {duration:.1f}s duration.")

    make_figure(df, args.metrics, args.title, args.output)


if __name__ == "__main__":
    main()
