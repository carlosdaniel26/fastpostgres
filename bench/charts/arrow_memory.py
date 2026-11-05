#!/usr/bin/env python3
"""Render the fixed-ring-budget comparison against ADBC + Arrow from its log.

One image, two panels, bars at the largest row count in the log (the smaller
sizes tell the same relation):

    left   the same read, rows/s: the ring's stream() against ADBC's
           record-batch reader and against its materialized Arrow table
    right  what each side held while it delivered: the ring's batch, the largest
           Arrow batch the reader held, and the whole result for the table path,
           read against the ring's fixed transfer buffer (the dashed rule)


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

import theme  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LOG = ROOT / "bench" / "results" / "fastpostgres-vs-arrow-memory.log"
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
        return f"{v / 1024**3:.3g} GiB"
    if v >= 1024**2:
        return f"{v / 1024**2:.3g} MiB"
    if v >= 1024:
        return f"{v / 1024:.3g} KiB"
    return f"{v:g}"


def style(ax, ticks, ylabel, log_y=False):
    # categorical x: one group per row count, the bars side by side within it
    ax.set_xticks(range(len(ticks)))
    ax.set_xticklabels([fmt_rows(t) for t in ticks], fontsize=8.5)
    ax.set_xlabel("rows read")
    ax.set_ylabel(ylabel)
    if log_y:
        ax.set_yscale("log")
        # a handful of binary buffer sizes, not every power of two
        ax.yaxis.set_major_locator(mticker.FixedLocator(
            [8 * 2**20, 32 * 2**20, 128 * 2**20, 512 * 2**20]))
        ax.yaxis.set_major_formatter(mticker.FuncFormatter(fmt_bytes))
        ax.yaxis.set_minor_formatter(mticker.NullFormatter())
    else:
        ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _p=None: fmt_rows(v)))
    ax.margins(y=0.26, x=0.12)
    theme.declutter(ax)


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
    if not rows:
        sys.exit(f"{args.log}: no rows found")
    rows = [rows[-1]]  # the largest size: the smaller ones tell the same relation
    fig, (ax_rate, ax_bytes) = plt.subplots(1, 2, figsize=(11.0, 5.0))
    style(ax_bytes, rows, "bytes held", log_y=True)

    # The rate panel is bars - one group per size, one bar per player - so it is
    # categorical on x and its grid runs on y; the bytes panel stays lines, where
    # the ring's flat window against the table's working set is the point.
    players = (("ring", ACCENT), ("arrow-batch", GREEN), ("arrow-table", ORANGE))
    slot = min(0.8 / len(players), 0.26)  # a bar keeps a bar's width
    step = slot * 1.12  # a sliver between the bars of a group
    peak = 0.0
    for index, (player, color) in enumerate(players):
        xs, values = [], []
        for position, n in enumerate(rows):
            if player not in data[n]:
                continue
            xs.append(position + (index - (len(players) - 1) / 2) * step)
            values.append(data[n][player])
        if not xs:
            continue
        peak = max([peak] + values)
        ax_rate.bar(xs, values, slot * 0.9, color=color, zorder=3)
        for x, value in zip(xs, values):
            ax_rate.annotate(fmt_rows(value), (x, value), textcoords="offset points",
                             xytext=(0, 5), ha="center", fontsize=6.5, color=color, zorder=5)
    ax_rate.set_xticks(range(len(rows)))
    ax_rate.set_xticklabels([fmt_rows(n) for n in rows], fontsize=8.5)
    ax_rate.set_xlabel("rows read")
    ax_rate.set_ylabel("rows/s")
    ax_rate.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _p=None: fmt_rows(v)))
    pad = 0.7 if len(rows) == 1 else 0.0
    ax_rate.set_xlim(-0.5 - pad, len(rows) - 0.5 + pad)
    if peak:
        ax_rate.set_ylim(0, peak * 1.18)
    theme.declutter(ax_rate)
    theme.ticks(ax_rate)
    ax_rate.set_title("the same read, same output, same rows", fontsize=9.5)

    # the ring's fixed budget first: the rule the bars are read against
    budget = next((carried[n]["ring"]["seg_bytes"] for n in rows
                   if carried.get(n, {}).get("ring", {}).get("seg_bytes")), 0)
    if budget:
        ax_bytes.axhline(budget, color=GREY, linestyle="--", linewidth=1.4, zorder=2)
        ax_bytes.annotate(f"the ring's transfer buffer: {fmt_bytes(budget)}, fixed",
                          xy=(0.02, budget), xycoords=("axes fraction", "data"),
                          textcoords="offset points", xytext=(4, -12),
                          fontsize=7, color="#888888", zorder=4)

    slot = min(0.8 / len(players), 0.26)  # a bar keeps a bar's width
    step = slot * 1.12  # a sliver between the bars of a group
    smallest = None
    peak = 0.0
    for index, (player, color) in enumerate(players):
        key = "result_bytes" if player == "arrow-table" else "max_batch_bytes"
        xs, values = [], []
        for position, n in enumerate(rows):
            value = carried.get(n, {}).get(player, {}).get(key)
            if not value:
                continue
            xs.append(position + (index - (len(players) - 1) / 2) * step)
            values.append(value)
        if not xs:
            continue
        peak = max([peak] + values)
        smallest = min([smallest or peak] + values)
        ax_bytes.bar(xs, values, slot * 0.9, color=color, zorder=3)
        for x, value in zip(xs, values):
            # a bar that reaches the rule keeps its label inside: above the bar
            # the label would run into the dashed transfer line
            inside = bool(budget) and value > budget / 1.15
            ax_bytes.annotate(fmt_bytes(value), (x, value), textcoords="offset points",
                              xytext=(0, -6 if inside else 5), ha="center",
                              va="top" if inside else "bottom", fontsize=6.5,
                              color="white" if inside else color, zorder=5)
    ax_bytes.set_xticks(range(len(rows)))
    ax_bytes.set_xticklabels([fmt_rows(n) for n in rows], fontsize=8.5)
    ax_bytes.set_xlabel("rows read")
    ax_bytes.set_xlim(-0.5 - pad, len(rows) - 0.5 + pad)
    ax_bytes.yaxis.set_major_formatter(mticker.FuncFormatter(fmt_bytes))
    if peak and smallest:
        ax_bytes.set_ylim(smallest * 0.5, peak * 2.2)  # bars on a log axis
    ax_bytes.set_title("what each held while it delivered", fontsize=9.5)

    handles = [
        plt.Rectangle((0, 0), 1, 1, color=ACCENT, label="fast-postgres (stream(), one segment)"),
        plt.Rectangle((0, 0), 1, 1, color=GREEN, label="ADBC record-batch reader"),
        plt.Rectangle((0, 0), 1, 1, color=ORANGE, label="ADBC Arrow table"),
        plt.Line2D([], [], color=GREY, linestyle="--", marker=None,
                   label="the ring's fixed transfer buffer"),
    ]
    fig.suptitle("the same rows against a fixed 512 MiB shared-memory buffer",
                 fontsize=12.5, y=0.975)
    theme.legend(fig, loc="upper center", bbox_to_anchor=(0.5, 0.925), ncol=4,
                 handles=handles, labels=[h.get_label() for h in handles])

    footer = " · ".join(env.get(k, "?") for k in ENV_KEYS)
    theme.footer(fig, footer, y=0.03)
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
