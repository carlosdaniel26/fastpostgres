#!/usr/bin/env python3
"""Render fast-postgres vs PgCache from the comparison log.

One image, two panels, two bars, because the answer depends on the shape of the
result (the largest size in the log; the smaller one tells the same relation):

    left   the full scan, rows/s     delivery-bound: the ring wins, and a cache
                                     hit pays to ship the rows again
    right  count/sums, ms/read (log) execution-bound: a hit does no work at all
                                     and wins by a wide margin

The bars are fast-postgres (the accent) and the same read through the PgCache
proxy with the cache warm (orange, over a psycopg 3 socket). The pgcache bar
carries its own difference against fast-postgres, the column the log prints.

Usage:
    just charts cache                 plots bench/results/fastpostgres-vs-pgcache.log
    just charts cache --log my.log    plots another run's log
    just charts cache --out docs/assets/   writes the PNG elsewhere
"""
import argparse
import re
import sys
from pathlib import Path

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.ticker import FuncFormatter, NullFormatter
except ImportError:
    sys.exit("matplotlib is required (pip install matplotlib)")

import theme  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LOG = ROOT / "bench" / "results" / "fastpostgres-vs-pgcache.log"
FILENAME = "pgcache-read.png"

# Strategic colour: the ring takes the accent, the cache is the thing priced.
ACCENT = "#1f77b4"
ORANGE = "#e1812c"
SERIES = (
    ("fastpostgres", ACCENT, "fast-postgres (shm ring)"),
    ("pgcache", ORANGE, "psycopg 3 → PgCache (cache warm)"),
)

SIZE_RE = re.compile(r"^# (\d+) rows")
ROW_RE = re.compile(
    r"^\s+(large|small|through)\s+"
    r"fastpostgres\s+([\d.]+)(M| ms)\s+"
    r"pgcache\s+([\d.]+)(M| ms)"
)
FILL_RE = re.compile(
    r"^\s+fill-(\w+)\s+pgcache\s+([\d.]+) ms\s+fastpostgres\s+([\d.]+) ms"
)
ENV_KEYS = ("cpu", "postgres", "pgcache image", "shape", "git")


def parse(text):
    """{size: {workload: {player: value}}}, the fills, and the log's own env."""
    data, fills, env = {}, {}, {}
    size = None
    for line in text.splitlines():
        m = SIZE_RE.match(line)
        if m:
            size = int(m.group(1))
            data.setdefault(size, {})
            continue
        m = ROW_RE.match(line)
        if m and size is not None:
            workload = m.group(1)
            values = {}
            for i, player in enumerate(("fastpostgres", "pgcache")):
                value, unit = float(m.group(2 + 2 * i)), m.group(3 + 2 * i)
                values[player] = value * 1e6 if unit == "M" else value  # rows/s or ms
            data[size][workload] = values
            continue
        m = FILL_RE.match(line)
        if m and size is not None:
            fills[(size, m.group(1))] = {
                "pgcache": float(m.group(2)),
                "fastpostgres": float(m.group(3)),
            }
            continue
        for key in ENV_KEYS:
            if line.startswith(f"{key}: "):
                env[key] = line.split(": ", 1)[1].strip()
    return data, fills, env


def fmt_rows(v, _pos=None):
    if v >= 1e6:
        return f"{v / 1e6:g}M"
    if v >= 1e3:
        return f"{v / 1e3:g}K"
    return f"{v:g}"


def fmt_ms(v, _pos=None):
    return f"{v:g}"


def bars(ax, data, workload, fmt, ylabel, log=False):
    sizes = [max(data)]  # the largest size: the smaller one tells the same relation
    labels = [f"{n // 1000000}M" if n >= 1e6 else f"{n // 1000}K" for n in sizes]
    width = 0.8 / len(SERIES)
    xs = list(range(len(sizes)))
    handles = []
    for i, (player, color, label) in enumerate(SERIES):
        values = [data[n][workload][player] for n in sizes]
        pos = [x - 0.4 + (i + 0.5) * width for x in xs]
        drawn = ax.bar(pos, values, width=width, color=color, label=label, zorder=3)
        handles.append(drawn[0])
        texts = []
        for size, v in zip(sizes, values):
            text = fmt(v)
            if player == "pgcache":
                # the cache's value as a share of the ring's
                pct = v / data[size][workload]["fastpostgres"] * 100
                text += f"\n{pct:.0f}%"
            texts.append(text)
        ax.bar_label(drawn, labels=texts, fontsize=6.8, padding=2)

    ax.set_xticks(xs)
    ax.set_xticklabels(labels)
    ax.set_xlabel("rows")
    ax.set_ylabel(ylabel)
    if log:
        ax.set_yscale("log")
        ax.yaxis.set_minor_formatter(NullFormatter())
    ax.yaxis.set_major_formatter(FuncFormatter(fmt))
    theme.declutter(ax)
    ax.margins(y=0.24)
    return handles


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--log", type=Path, default=DEFAULT_LOG, help="the comparison log to plot")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "assets", help="where to write the PNG")
    args = ap.parse_args()

    text = args.log.read_text()
    data, fills, env = parse(text)
    if not data:
        sys.exit(f"{args.log}: no rows found -- run `just pgcache-bench` first")

    fig, (ax_scan, ax_agg) = plt.subplots(1, 2, figsize=(11.0, 5.0))
    bars(ax_scan, data, "large", fmt_rows, "rows/s")
    ax_scan.set_title("the full scan: n rows out, delivery-bound", fontsize=9.5)
    handles = bars(ax_agg, data, "small", fmt_ms, "ms/read", log=True)
    ax_agg.set_title("count + two sums: one row out, execution-bound", fontsize=9.5)

    fig.suptitle("fast-postgres against a result cache, same rows, same output",
                 fontsize=12.5, y=0.975)
    theme.legend(fig, loc="upper center", bbox_to_anchor=(0.5, 0.925), ncol=2,
                 handles=handles, labels=[label for _, _, label in SERIES], fontsize=9)

    footer = " · ".join(env.get(k, "?") for k in ("cpu", "postgres", "pgcache image", "git"))
    theme.footer(fig, footer, y=0.03)
    fig.subplots_adjust(left=0.075, right=0.985, top=0.80, bottom=0.14)

    path = args.out / FILENAME
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"wrote {path}")
    for (size, workload), values in sorted(fills.items()):
        print(f"  fill {workload} at {size}: pgcache {values['pgcache']:.1f} ms, "
              f"fastpostgres {values['fastpostgres']:.1f} ms")


if __name__ == "__main__":
    main()
