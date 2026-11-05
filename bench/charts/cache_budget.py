#!/usr/bin/env python3
"""Render the equal-memory-budget comparison from its log.

One image, two panels, the budget on the x axis (log2), one line per player,
because the same number of bytes buys different things:

    left   the full scan, rows/s     delivery-bound: the ring is flat, the cache
                                     pays to ship the rows again - until its
                                     budget stops it registering anything at all
    right  count/sums, ms/read (log) execution-bound: a cache hit does no work,
                                     so the cache is far below the ring wherever
                                     its budget lets it hold the entry

The budget is the log's own schema: each `# budget M` section runs the ring with
a transfer segment of M/2 and the cache in a container capped at M. The dashed
line on the left is the pass-through workload - the same rows through the proxy
with no cache in front of them - so the cache's line can be read against what
forwarding alone costs. The shaded budgets are the ones where the cache's own
evidence says it held nothing (its container was OOM-killed, or nothing was
registered): on those bytes it is a plain proxy.

Usage:
    just charts cache-budget                        plots the committed log
    just charts cache-budget --log my.log           plots another run's log
    just charts cache-budget --out docs/assets/          writes the PNG elsewhere
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
DEFAULT_LOG = ROOT / "bench" / "results" / "fastpostgres-vs-pgcache-memory.log"
FILENAME = "pgcache-memory.png"

ACCENT = "#1f77b4"
ORANGE = "#e1812c"
GREY = "#9a9a9a"
SHADE = "#f0f0f0"

BUDGET_RE = re.compile(r"^# budget ([\d.]+) (KiB|MiB|GiB), segment ")
# the pgcache cell is "-" at a budget the cache cannot run at: the ring's own
# number is still measured there, and it is the interesting half of a flat line
ROW_RE = re.compile(
    r"^\s+(large|small|through)\s+"
    r"fastpostgres\s+([\d.]+)(M| ms)\s+"
    r"pgcache\s+(?:-|([\d.]+)(M| ms))"
)
RING_RE = re.compile(
    r"^\s+ring\s+seg_bytes=(?P<seg_bytes>\d+) segments=(?P<segments>\d+) "
    r"pair_bytes=(?P<pair_bytes>\d+) ctrl_bytes=(?P<ctrl_bytes>\d+) "
    r"shm_reads=(?P<shm_reads>\d+)/(?P<shm_reps>\d+) rss_bytes=(?P<rss_bytes>\d+)"
)
CACHE_RE = re.compile(
    r"^\s+cache\s+budget_bytes=(?P<budget_bytes>\d+) used_bytes=(?P<used_bytes>\d+) "
    r"rss_bytes=(?P<rss_bytes>\d+) memo_bytes=(?P<memo_bytes>\d+) "
    r"memo_hits=(?P<memo_hits>\d+) registered=(?P<registered>\d+) "
    r"throttled=(?P<throttled>\d+) count_cap=(?P<count_cap>\d+) "
    r"marginal_bytes=(?P<marginal_bytes>\d+) disk_used_bytes=(?P<disk_used_bytes>\d+)"
)
# the proxy container was OOM-killed at this budget: the cache does not exist in
# that many bytes, which is a result about the budget, not a missing measurement
CACHE_DOWN_RE = re.compile(r"^\s+cache\s+unavailable:")
ENV_KEYS = ("cpu", "postgres", "pgcache image", "git")


def parse_units(value, unit):
    return int(float(value) * {"KiB": 1024, "MiB": 1024**2, "GiB": 1024**3}[unit])


def parse(text):
    """{budget: {workload: {player: value}}}, the evidence, and the log's env."""
    data, evidence, env = {}, {}, {}
    budget = workload = None
    for line in text.splitlines():
        m = BUDGET_RE.match(line)
        if m:
            budget = parse_units(m.group(1), m.group(2))
            data.setdefault(budget, {})
            evidence.setdefault(budget, {})
            workload = None
            continue
        m = ROW_RE.match(line)
        if m and budget is not None:
            workload = m.group(1)
            values = {"fastpostgres": float(m.group(2)) * (1e6 if m.group(3) == "M" else 1)}
            if m.group(4):
                values["pgcache"] = float(m.group(4)) * (1e6 if m.group(5) == "M" else 1)
            data[budget][workload] = values
            evidence[budget][workload] = {}
            continue
        for tag, pattern in (("ring", RING_RE), ("cache", CACHE_RE)):
            m = pattern.match(line)
            if m and budget is not None and workload is not None:
                evidence[budget][workload][tag] = {k: int(v) for k, v in m.groupdict().items()}
        if CACHE_DOWN_RE.match(line) and budget is not None:
            evidence[budget]["down"] = True
        for key in ENV_KEYS:
            if line.startswith(f"{key}: "):
                env[key] = line.split(": ", 1)[1].strip()
    return data, evidence, env


def fmt_budget(v):
    if v >= 1024**3:
        return f"{v / 1024**3:g} GiB"
    if v >= 1024**2:
        return f"{v / 1024**2:g} MiB"
    return f"{v / 1024:g} KiB"


def fmt_rows(v):
    if v >= 1e6:
        return f"{v / 1e6:.2f}M"
    if v >= 1e3:
        return f"{v / 1e3:.0f}K"
    return f"{v:g}"


def bars(ax, budgets, data, series, fmt, empty_budgets, ylabel, log_y=False):
    """One group per budget, one bar per player, labelled with its value.

    The shape is the same in every panel: the budgets on the x axis, so the
    question the figure asks (what each side buys with M bytes) is read the way
    a bar chart is read. A bar the cache does not have - its container was
    OOM-killed, or nothing registered - is left out and the slot says why.
    """
    width = 0.8 / len(series)
    step = width * 1.12  # a sliver between the bars of a group
    xs = list(range(len(budgets)))
    handles = []
    for i, (workload, player, color, label) in enumerate(series):
        present = [b for b in budgets if player in data[b].get(workload, {})]
        pos = [xs[budgets.index(b)] + (i - (len(series) - 1) / 2) * step for b in present]
        vals = [data[b][workload][player] for b in present]
        drawn = ax.bar(pos, vals, width=width, color=color, zorder=3)
        for rect, b in zip(drawn, present):
            # hatched only where the cache ran but held nothing: the ring's bars
            # are the same read whatever the budget
            if player == "pgcache" and b in empty_budgets:
                rect.set_hatch("//")
        handles.append(plt.Rectangle((0, 0), 1, 1, color=color, label=label))
        ax.bar_label(drawn, labels=[fmt(v) for v in vals], fontsize=6.8, padding=2)
        if player == "pgcache":
            for b in budgets:
                if b in empty_budgets and b not in present and i == 1:
                    ax.annotate("OOM", (xs[budgets.index(b)] + (i - (len(series) - 1) / 2) * step, 0.02),
                                xycoords=("data", "axes fraction"), ha="center", va="bottom",
                                fontsize=6.8, color=ORANGE, zorder=4)

    ax.set_xticks(xs)
    ax.set_xticklabels([fmt_budget(b) for b in budgets], fontsize=8.5)
    ax.set_xlabel("memory budget on both sides")
    ax.set_ylabel(ylabel)
    if log_y:
        ax.set_yscale("log")
        ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _p=None: f"{v:g}"))
    ax.margins(y=0.24)
    theme.declutter(ax)
    theme.ticks(ax, log=log_y)
    return handles


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--log", type=Path, default=DEFAULT_LOG, help="the comparison log to plot")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "assets", help="where to write the PNG")
    args = ap.parse_args()

    data, evidence, env = parse(args.log.read_text())
    if not data:
        sys.exit(f"{args.log}: no rows found -- run `just pgcache-memory-bench` first")

    budgets = sorted(data)
    fig, (ax_scan, ax_agg) = plt.subplots(1, 2, figsize=(11.0, 5.0))

    # the budgets where the cache held nothing at all: its container was
    # OOM-killed, or every workload in that cell reported nothing registered.
    # A *throttled* cache that still held the query under test is not hatched:
    # the log's evidence lines carry that distinction.
    def empty(b):
        if evidence.get(b, {}).get("down"):
            return True
        gauges = [evidence[b].get(w, {}).get("cache") for w in data[b]]
        gauges = [g for g in gauges if g]
        return bool(gauges) and all(g["registered"] == 0 for g in gauges)

    empty_budgets = [b for b in budgets if empty(b)]

    scan_series = (
        ("large", "fastpostgres", ACCENT, "fast-postgres (shm ring, M/2 per segment)"),
        ("large", "pgcache", ORANGE, "psycopg 3 → PgCache (capped at M)"),
        ("through", "pgcache", GREY, "the cache's pass-through row (left: no cache in front)"),
    )
    handles = bars(ax_scan, budgets, data, scan_series, fmt_rows, empty_budgets, "rows/s")
    handles.append(plt.Rectangle((0, 0), 1, 1, facecolor="white", edgecolor="#888888",
                                 hatch="//", label="hatched: the cache holds nothing there"))
    ax_scan.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _p=None: fmt_rows(v)))
    ax_scan.set_title("the full scan: n rows out, delivery-bound", fontsize=9.5)

    bars(ax_agg, budgets, data,
         (("small", "fastpostgres", ACCENT, "fast-postgres (shm ring)"),
          ("small", "pgcache", ORANGE, "psycopg 3 → PgCache (cache)")),
         lambda v: f"{v:g}", empty_budgets, "ms/read", log_y=True)
    ax_agg.set_title("count + two sums: one row out, execution-bound", fontsize=9.5)

    fig.suptitle("the same memory budget on both sides: what each buys with the bytes",
                 fontsize=12.5, y=0.975)
    theme.legend(fig, loc="upper center", bbox_to_anchor=(0.5, 0.925), ncol=2,
                 handles=handles, labels=[h.get_label() for h in handles])

    footer = " · ".join(env.get(k, "?") for k in ENV_KEYS)
    theme.footer(fig, footer, y=0.03)
    fig.subplots_adjust(left=0.07, right=0.985, top=0.79, bottom=0.14)

    path = args.out / FILENAME
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"wrote {path}")
    for b in budgets:
        ring = evidence.get(b, {}).get("large", {}).get("ring", {})
        cache = evidence.get(b, {}).get("large", {}).get("cache", {})
        print(f"  budget {fmt_budget(b)}: ring seg {ring.get('seg_bytes', 0) / 1048576:g} MiB "
              f"x{ring.get('segments', 0)}, shm_reads {ring.get('shm_reads', 0)}/"
              f"{ring.get('shm_reps', 0)}; cache budget "
              f"{cache.get('budget_bytes', 0) / 1048576:g} MiB, memo "
              f"{cache.get('memo_bytes', 0) / 1048576:g} MiB, registered "
              f"{cache.get('registered', 0)}, throttled {cache.get('throttled', 0)}"
              f"{', did not run' if evidence.get(b, {}).get('down') else ''}")


if __name__ == "__main__":
    main()
