#!/usr/bin/env python3
"""Plot the nativepostgress benchmark: rows processed vs elapsed time.

One coloured line per transport (tcp / unix socket / shared memory) in two
panels: read and write. The x axis is elapsed time (s), the y axis is the number
of rows processed — a straight line from the origin means constant throughput,
and a curve that flattens to the right means throughput dropped as the load grew.

The L3 cache threshold is drawn as a horizontal line: the working set is roughly
`rows x bytes_per_row x copies` bytes (shm segment + table + WAL).

Data comes from a benchmark log (`scripts/bench-node-docker.sh` output) when one
is passed with --log or found as ./bench.log; otherwise an embedded sample run
is used. Times are derived from the printed rates (rows / rows-per-second).

Usage:
    scripts/bench-node-docker.sh 100000 1000000 10000000 100000000 > bench.log
    scripts/plot_bench.py --log bench.log --out bench_chart.png
"""
import argparse
import re
import sys
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit("matplotlib is required (pip install matplotlib)")

SERIES = ("tcp", "unix", "shm")
COLORS = {"tcp": "#1f77b4", "unix": "#2ca02c", "shm": "#d62728"}
MARKERS = {"tcp": "o", "unix": "s", "shm": "^"}

# "<load>  <tcp>M  <unix>M  <shm>M  <speedup>x  <speedup>x"
ROW_RE = re.compile(
    r"^\s*(\d+)([KM])\s+([\d.]+)M\s+([\d.]+)M\s+([\d.]+)M\s+[\d.]+x\s+[\d.]+x\s*$"
)

# rows/s (millions) from a sample run, used when no benchmark log is available
SAMPLE = {
    "read": {"100K": (2.4, 2.8, 4.6), "1M": (2.4, 3.2, 5.1),
             "10M": (0.3, 3.1, 5.3)},
    "write": {"100K": (1.6, 1.1, 2.4), "1M": (1.6, 1.0, 2.1),
              "10M": (0.9, 0.7, 1.5)},
}

# Benchmark row size (node-extension/bench/bench.js):
#   id 4 + account_id 8 + balance 8 + active 1 + name ~13 + email ~25
#   + score 8 + age 4 = ~71 bytes
BYTES_PER_ROW = 71
# The shm write touches the data as: the /dev/shm segment, the table, and WAL.
COPIES = 3

L3_MB = 32


def parse_log(path):
    """Extract {read|write: {load: (tcp, unix, shm)}} rates in millions of rows/s."""
    data = {"read": {}, "write": {}}
    section = None
    for line in Path(path).read_text().splitlines():
        if line.startswith("READ (rows/s)"):
            section = "read"
        elif line.startswith("WRITE (rows/s)"):
            section = "write"
        elif line.startswith(("SCAN", "PAGINATED", "TOP-N", "QUERY")):
            section = None
        elif section:
            m = ROW_RE.match(line)
            if m:
                data[section][f"{m.group(1)}{m.group(2)}"] = tuple(
                    float(m.group(i)) for i in (3, 4, 5)
                )
    return data


def num_rows(label):
    n = float(label[:-1])
    return n * (1_000 if label.endswith("K") else 1_000_000)


def human_bytes(nbytes):
    return f"{nbytes / 1024**2:.0f} MB"


def human_rows(n):
    if n >= 1_000_000:
        return f"{n / 1_000_000:.1f}M"
    return f"{n / 1_000:.0f}K"


def threshold_rows(nbytes, bytes_per_row, copies):
    """Row count at which the working set (rows x bytes x copies) hits nbytes."""
    return nbytes / float(bytes_per_row * copies)


def seconds(rows, rate_millions):
    """Elapsed seconds for `rows` rows at a rate in millions of rows/s."""
    return rows / (rate_millions * 1e6)


def draw(ax, section, data, title, args):
    loads = sorted(data[section], key=num_rows)
    if not loads:
        ax.set_visible(False)
        return

    rows = [num_rows(load) for load in loads]
    series_times = {}
    for i, name in enumerate(SERIES):
        times = [seconds(rows[j], data[section][loads[j]][i])
                 for j in range(len(loads))]
        series_times[name] = times
        ax.plot(times, rows, color=COLORS[name], marker=MARKERS[name],
                linewidth=2, markersize=7, label=name, zorder=3)

    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("time (s)")
    ax.set_ylabel("rows processed")
    ax.set_yticks(rows)
    ax.set_yticklabels(loads)
    ax.set_title(title)
    ax.grid(True, which="major", alpha=0.3)
    ax.legend(title="transport", loc="upper left")
    ax.margins(x=0.14, y=0.18)

    # L3 cache threshold, as a row count (horizontal line)
    l3_bytes = args.l3_mb * 1024**2
    l3_rows = threshold_rows(l3_bytes, args.bytes_per_row, args.copies)
    y0, y1 = ax.get_ylim()
    if y0 < l3_rows < y1:
        ax.axhline(l3_rows, color="#6b7280", linestyle=(0, (4, 3)),
                   linewidth=1.2, alpha=0.85, zorder=2)
        ax.annotate(
            f"L3 {human_bytes(l3_bytes)} (~{human_rows(l3_rows)} rows)",
            xy=(0.02, l3_rows), xycoords=("axes fraction", "data"),
            xytext=(0, 5), textcoords="offset points", ha="left", va="bottom",
            fontsize=8, color="#6b7280",
            bbox=dict(boxstyle="round,pad=0.15", fc="white", ec="none",
                      alpha=0.85),
        )

    # elapsed time per transport, in the empty lower-right corner
    table = ["elapsed (s)"]
    table.append(f"{'load':>5} " + " ".join(f"{n:>8}" for n in SERIES))
    for j, load in enumerate(loads):
        table.append(f"{load:>5} " + " ".join(
            f"{series_times[n][j]:>8.3f}" for n in SERIES))
    ax.text(0.97, 0.05, "\n".join(table), transform=ax.transAxes, ha="right",
            va="bottom", family="monospace", fontsize=9,
            bbox=dict(boxstyle="round,pad=0.4", fc="white", ec="#bbbbbb",
                      alpha=0.9))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--log", help="benchmark log to parse (default: ./bench.log if present)")
    ap.add_argument("--out", default="bench_chart.png", help="output image (default: %(default)s)")
    ap.add_argument("--bytes-per-row", type=int, default=BYTES_PER_ROW,
                    help="approximate row size in bytes (default: %(default)s)")
    ap.add_argument("--copies", type=int, default=COPIES,
                    help="how many times the write path touches each row, for the "
                         "working-set model (default: %(default)s)")
    ap.add_argument("--l3-mb", type=int, default=L3_MB,
                    help="L3 cache size in MB (default: %(default)s)")
    args = ap.parse_args()

    if args.log:
        data, source = parse_log(args.log), args.log
    elif Path("bench.log").exists():
        data, source = parse_log("bench.log"), "bench.log"
    else:
        data, source = SAMPLE, "embedded sample"
    if not data["read"] and not data["write"]:
        sys.exit(f"no benchmark rows found in {source}")

    fig, axes = plt.subplots(1, 2, figsize=(13, 5.4))
    draw(axes[0], "read", data, "Read: rows fetched", args)
    draw(axes[1], "write", data, "Write: rows inserted", args)
    fig.suptitle(f"nativepostgress — rows vs time  ({source})", fontsize=13)
    l3_rows = threshold_rows(args.l3_mb * 1024**2, args.bytes_per_row, args.copies)
    fig.text(
        0.5, 0.015,
        f"working set ≈ rows × {args.bytes_per_row} B × {args.copies} "
        f"(shm segment + table + WAL) — L3 {args.l3_mb} MB ≈ {human_rows(l3_rows)} rows",
        ha="center", fontsize=9, color="#4b5563",
    )
    fig.subplots_adjust(left=0.075, right=0.985, top=0.87, bottom=0.16, wspace=0.18)
    fig.savefig(args.out, dpi=150)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
