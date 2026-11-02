#!/usr/bin/env python3
"""Render the fixed-ring-budget comparison against ADBC + Arrow from its log.

One image, two panels, the row count on the x axis (log2):

    left   the same read, rows/s: the ring's stream() against ADBC's
           record-batch reader and against its materialized Arrow table
    right  what each side held while it delivered: the ring's batches and the
           fixed transfer buffer the worker mapped, the largest Arrow batch the
           reader held, and the whole result for the table path - the three
           numbers that say whether "the same rows" cost the same memory

The right panel is the point of the comparison, so the budget is drawn as a flat
line: the ring's segment is the same number of bytes for every row count, while
the table path's buffer is the result's size.

Usage:
    just charts arrow-memory                       plots the committed log
    just charts arrow-memory --log my.log          plots another run's log
    just charts arrow-memory --out docs/assets/         writes the PNG elsewhere
"""
import argparse
import re
import sys
from pathlib import Path

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import matplotlib.ticker as mticker
except ImportError:
    sys.exit("matplotlib is required (pip install matplotlib)")

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LOG = ROOT / "benchmarks" / "results" / "fastpostgres-vs-arrow-memory.log"
FILENAME = "arrow-memory.png"

ACCENT = "#1f77b4"
GREEN = "#2ca02c"
ORANGE = "#e1812c"
GREY = "#9a9a9a"

SIZE_RE = re.compile(r"^# (\d+) rows")
ROW_RE = re.compile(r"^\s+(ring|arrow-batch|arrow-table)\s+([\d.]+)M\s+\S+\s+\S+\s*$")
EVIDENCE_RE = re.compile(r"^\s+(ring|arrow-batch|arrow-table)\s+(\S+=.+)$")
PAIR_RE = re.compile(r"(\w+)=(-?\d+)")
ENV_KEYS = ("cpu", "postgres", "pyarrow", "adbc", "git")


def parse(text):
    """{rows: {player: rows/s}}, {rows: {key: bytes}}, and the log's env."""
    data, carried, env = {}, {}, {}
    rows = None
    for line in text.splitlines():
        m = SIZE_RE.match(line)
        if m:
            rows = int(m.group(1))
            data.setdefault(rows, {})
            carried.setdefault(rows, {})
            continue
        m = ROW_RE.match(line)
        if m and rows is not None:
            data[rows][m.group(1)] = float(m.group(2)) * 1e6
            continue
        m = EVIDENCE_RE.match(line)
        if m and rows is not None:
            for key, value in PAIR_RE.findall(m.group(2)):
                carried[rows].setdefault(m.group(1), {})[key] = int(value)
            continue
        for key in ENV_KEYS:
            if line.startswith(f"{key}: "):
                env[key] = line.split(": ", 1)[1].strip()
    return data, carried, env


def fmt_rows(v):
    if v >= 1e6:
        return f"{v / 1e6:g}M"
    if v >= 1e3:
        return f"{v / 1e3:g}K"
    return f"{v:g}"


def fmt_bytes(v, _pos=None):
    if v >= 1024**3:
        return f"{v / 1024**3:g} GiB"
    if v >= 1024**2:
        return f"{v / 1024**2:g} MiB"
    if v >= 1024:
        return f"{v / 1024:g} KiB"
    return f"{v:g}"


def line(ax, xs, ys, color, ls="-", marker="o", width=1.8, labels=True, dy=7, fmt=fmt_bytes):
    if not xs:
        return
    ax.plot(xs, ys, color=color, linestyle=ls, marker=marker, markersize=4.5,
            linewidth=width, zorder=3)
    if not labels:
        return
    for x, y in zip(xs, ys):
        ax.annotate(fmt(y), (x, y), textcoords="offset points", xytext=(0, dy),
                    ha="center", va="bottom" if dy > 0 else "top",
                    fontsize=6.5, color=color, zorder=5)


def style(ax, ticks, ylabel, log_y=False):
    ax.set_xscale("log", base=10)
    ax.set_xticks(ticks)
    ax.set_xticklabels([fmt_rows(t) for t in ticks], fontsize=8.5)
    ax.xaxis.set_minor_formatter(mticker.NullFormatter())
    ax.set_xlabel("rows read")
    ax.set_ylabel(ylabel)
    if log_y:
        ax.set_yscale("log")
        # binary buffer sizes: what the lines are read against are MiB figures
        ax.yaxis.set_major_locator(mticker.FixedLocator([2**i * 1048576 for i in range(0, 12)]))
        ax.yaxis.set_major_formatter(mticker.FuncFormatter(fmt_bytes))
        ax.yaxis.set_minor_formatter(mticker.NullFormatter())
    else:
        ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _p=None: fmt_rows(v)))
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color("#cccccc")
    ax.tick_params(colors="#666666", length=0)
    ax.margins(y=0.26, x=0.12)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--log", type=Path, default=DEFAULT_LOG, help="the comparison log to plot")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "assets", help="where to write the PNG")
    args = ap.parse_args()

    data, carried, env = parse(args.log.read_text())
    if not data:
        sys.exit(f"{args.log}: no rows found -- run `just arrow-memory-bench` first")

    rows = sorted(data)
    fig, (ax_rate, ax_bytes) = plt.subplots(1, 2, figsize=(11.0, 5.0))
    style(ax_rate, rows, "rows/s")
    style(ax_bytes, rows, "bytes held", log_y=True)

    for player, color in (("ring", ACCENT), ("arrow-batch", GREEN), ("arrow-table", ORANGE)):
        xs = [n for n in rows if player in data[n]]
        # the two Arrow paths sit within a few percent of each other: keep their
        # labels apart, one above the point and one below
        line(ax_rate, xs, [data[n][player] for n in xs], color, fmt=fmt_rows,
             dy=7 if player != "arrow-table" else -11)
    ax_rate.set_title("the same read, same output, same rows", fontsize=9.5)

    # the ring's fixed budget first: the line the others are read against
    budget = next((carried[n]["ring"]["seg_bytes"] for n in rows
                   if carried.get(n, {}).get("ring", {}).get("seg_bytes")), 0)
    if budget:
        line(ax_bytes, [rows[0], rows[-1]], [budget, budget], GREY, ls="--", marker=None,
             width=1.4, labels=False)
        ax_bytes.annotate(f"the ring's transfer buffer: {fmt_bytes(budget)}, fixed",
                          xy=(rows[0], budget), textcoords="offset points", xytext=(6, -12),
                          fontsize=7, color="#888888", zorder=4)
    # the table path is the tallest line, so its labels go below the markers
    for player, key, color, dy in (("ring", "max_batch_bytes", ACCENT, -11),
                                   ("arrow-batch", "max_batch_bytes", GREEN, -11),
                                   ("arrow-table", "result_bytes", ORANGE, -11)):
        xs = [n for n in rows if carried.get(n, {}).get(player, {}).get(key)]
        line(ax_bytes, xs, [carried[n][player][key] for n in xs], color, dy=dy)
    ax_bytes.set_title("what each held while it delivered", fontsize=9.5)

    handles = [
        plt.Line2D([], [], color=ACCENT, marker="o", markersize=4.5,
                   label="fast-postgres (stream(), one segment)"),
        plt.Line2D([], [], color=GREEN, marker="o", markersize=4.5,
                   label="ADBC record-batch reader"),
        plt.Line2D([], [], color=ORANGE, marker="o", markersize=4.5,
                   label="ADBC Arrow table"),
        plt.Line2D([], [], color=GREY, linestyle="--", marker=None,
                   label="the ring's fixed transfer buffer"),
    ]
    fig.suptitle("the same rows against a fixed 512 MiB shared-memory buffer",
                 fontsize=12.5, y=0.975)
    fig.legend(handles, [h.get_label() for h in handles], loc="upper center",
               bbox_to_anchor=(0.5, 0.925), ncol=4, frameon=False, fontsize=8.5)

    footer = " · ".join(env.get(k, "?") for k in ENV_KEYS)
    fig.text(0.5, 0.03, footer, ha="center", fontsize=7.5, color="#888888")
    fig.subplots_adjust(left=0.07, right=0.985, top=0.79, bottom=0.14)

    path = args.out / FILENAME
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"wrote {path}")
    for n in rows:
        ring = carried.get(n, {}).get("ring", {})
        batch = carried.get(n, {}).get("arrow-batch", {})
        table = carried.get(n, {}).get("arrow-table", {})
        print(f"  rows {fmt_rows(n)}: ring buffer {fmt_bytes(ring.get('seg_bytes', 0))}, "
              f"batch {fmt_bytes(ring.get('max_batch_bytes', 0))}; "
              f"arrow batch {fmt_bytes(batch.get('max_batch_bytes', 0))}, "
              f"arrow table {fmt_bytes(table.get('result_bytes', 0))}")


if __name__ == "__main__":
    main()
