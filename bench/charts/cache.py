#!/usr/bin/env python3
"""Render fast-postgres vs PgCache from the comparison log.

One figure, two panels, two bars, because the answer depends on the shape of the
result (the largest size in the log; the smaller one tells the same relation):

    left   the full scan, rows/s     delivery-bound: the ring wins, and a cache
                                     hit pays to ship the rows again
    right  count/sums, ms/read (log) execution-bound: a hit does no work at all
                                     and wins by a wide margin

The bars are fast-postgres and the same read through the PgCache proxy with the
cache warm. The pgcache bar carries its own difference against fast-postgres,
the column the log prints. The look comes from `styles.py`: the repository
default is `mono`, the deck default is `brutal`.

Usage:
    just charts cache                        plots the default log, repo style
    just charts cache --chosen presentation  the same figure, deck style
    just charts cache --log my.log           plots another run's log
    just charts cache --out docs/assets/     writes the PNG elsewhere
"""
import argparse
import re
import sys
from pathlib import Path

import cache_figure  # noqa: E402
import styles  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LOG = ROOT / "bench" / "results" / "fastpostgres-vs-pgcache.log"
FILENAME = "pgcache-read.png"

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


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--log", type=Path, default=DEFAULT_LOG, help="the comparison log to plot")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "assets", help="where to write the PNG")
    ap.add_argument("--style", choices=styles.ORDER, default=None,
                    help="override the style (default: the chosen repository style)")
    ap.add_argument("--chosen", choices=tuple(styles.CHOSEN), default="repository",
                    help="which selected look to use (default: repository)")
    args = ap.parse_args()

    text = args.log.read_text()
    data, fills, env = parse(text)
    if not data:
        sys.exit(f"{args.log}: no rows found -- run `just pgcache-bench` first")

    style_name = args.style or styles.CHOSEN[args.chosen]
    path = cache_figure.render(style_name, data[max(data)], env, args.out, FILENAME)
    print(f"wrote {path}  ({style_name})")
    for (size, workload), values in sorted(fills.items()):
        print(f"  fill {workload} at {size}: pgcache {values['pgcache']:.1f} ms, "
              f"fastpostgres {values['fastpostgres']:.1f} ms")


if __name__ == "__main__":
    main()
