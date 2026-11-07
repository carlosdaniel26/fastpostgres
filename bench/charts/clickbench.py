#!/usr/bin/env python3
"""Render the ClickBench comparison from its committed log.

Two panels, one bar per player, each bar carrying its own value and its delta
against fast-postgres:

    left   ClickBench's metric: queries per second over the 43-query set
    right  rows delivered per second, the number this repository claims

The two numbers answer different questions and the figure keeps them apart: q/s
is what the published tables score, rows/s is what the ring is for. Read against
the caption, the bars say why: 41 of the 43 queries return a handful of
aggregated rows, so there is almost nothing to deliver and the executor dominates.

Usage:
    just charts clickbench                     plots the committed record
    just charts clickbench --log my.log        plots another run's log
    just charts clickbench --out docs/assets/  writes the PNG elsewhere
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
DEFAULT_LOG = ROOT / "bench" / "results" / "clickbench.log"
FILENAME = "clickbench.png"

# The ring first, then the rivals.
PLAYERS = (
    ("fastpostgres", "fast-postgres", "#1f77b4"),
    ("adbc", "ADBC +\nArrow", "#2ca02c"),
    ("psycopg", "psycopg 3", "#e1812c"),
)
BASELINE = "fastpostgres"

ROW_RE = re.compile(r"^(fastpostgres|adbc|psycopg)\s+([\d.]+)\s+([\d.]+)\s+([\d,]+)\s*$")
SET_RE = re.compile(r"^ClickBench: (\d+) queries, (\d+) clients?, (\d+) reps?")
ROWS_RE = re.compile(r"^load: hits ([\d,]+) rows")
ENV_KEYS = ("cpu", "postgres", "git")


def parse(text):
    """{player: (q/s, rows/s)}, the run's shape, and its env block."""
    data, env, shape = {}, {}, {}
    inside = False
    for line in text.splitlines():
        if line.strip() == "### ENV":
            inside = True
            continue
        if line.strip() == "### END ENV":
            inside = False
            continue
        if inside and ": " in line:
            key, value = line.split(": ", 1)
            env[key.strip()] = value.strip()
            continue
        m = ROW_RE.match(line)
        if m:
            data[m.group(1)] = (float(m.group(3)), float(m.group(4).replace(",", "")))
            continue
        m = SET_RE.match(line)
        if m:
            shape["queries"], shape["clients"], shape["reps"] = map(int, m.groups())
            continue
        m = ROWS_RE.match(line)
        if m:
            shape["rows"] = int(m.group(1).replace(",", ""))
    return data, shape, env


def fmt_rows(v):
    if v >= 1e6:
        return f"{v / 1e6:g}M"
    if v >= 1e3:
        return f"{v / 1e3:g}K"
    return f"{v:g}"


def panel(ax, data, key, fmt, title):
    """One bar per player, each labelled with its value and its delta."""
    base = data[BASELINE][key]
    width = 0.42  # a bar keeps a bar's width on an axis grown around the group
    for position, (name, label, color) in enumerate(PLAYERS):
        value = data[name][key]
        ax.bar([position], [value], width, color=color, zorder=3)
        ax.annotate(fmt(value), (position, value), textcoords="offset points",
                    xytext=(0, 12), ha="center", fontsize=8.5, color=color, zorder=5)
        delta = "baseline" if name == BASELINE else f"{100 * (value / base - 1):+.0f}%"
        ax.annotate(delta, (position, value), textcoords="offset points",
                    xytext=(0, 3), ha="center", fontsize=7, color="#888888", zorder=5)

    ax.set_xticks(range(len(PLAYERS)))
    ax.set_xticklabels([label for _name, label, _color in PLAYERS], fontsize=8.5)
    ax.set_xlim(-0.6, len(PLAYERS) - 0.4)
    ax.set_ylim(0, max(data[name][key] for name, _l, _c in PLAYERS) * 1.24)
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _p=None: fmt(v)))
    ax.set_title(title, fontsize=9.5)
    theme.declutter(ax)
    theme.ticks(ax)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--log", type=Path, default=DEFAULT_LOG, help="the comparison log to plot")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "assets", help="where to write the PNG")
    args = ap.parse_args()

    data, shape, env = parse(args.log.read_text())
    missing = [name for name, _l, _c in PLAYERS if name not in data]
    if missing:
        sys.exit(f"{args.log}: no rows for {', '.join(missing)} -- run `just rivals clickbench` first")

    fig, (ax_qps, ax_rows) = plt.subplots(1, 2, figsize=(7.8, 4.4))
    panel(ax_qps, data, 0, lambda v: f"{v:g}", "queries per second (ClickBench's metric)")
    panel(ax_rows, data, 1, fmt_rows, "rows delivered per second")
    ax_qps.set_ylabel("q/s")
    ax_rows.set_ylabel("rows/s")

    fig.suptitle("ClickBench: 43 queries, three players, one machine", fontsize=12.5, y=0.965)

    scale = f"{shape['rows']:,} rows" if "rows" in shape else "unknown rows"
    versions = " · ".join(
        [env.get("cpu", "?")] + [f"{key} {env[key]}" for key in ENV_KEYS[1:] if key in env]
    )
    footer = " · ".join([
        f"{scale}, {shape.get('clients', '?')} clients per player",
        "41 of 43 queries return aggregated rows: the executor dominates, not the transport",
        versions,
    ])
    theme.footer(fig, footer, y=0.025)
    fig.subplots_adjust(left=0.075, right=0.985, top=0.86, bottom=0.28)

    path = args.out / FILENAME
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"wrote {path}")
    for name, label, _color in PLAYERS:
        qps, rows = data[name]
        print(f"  {label.replace(chr(10), ' ').rstrip('*'):<14} {qps:6.1f} q/s  {rows:8,.0f} rows/s  "
              f"({100 * (qps / data[BASELINE][0] - 1):+.1f}% q/s)")


if __name__ == "__main__":
    main()
