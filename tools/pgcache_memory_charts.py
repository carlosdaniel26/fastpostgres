#!/usr/bin/env python3
"""Render the equal-memory-budget comparison from its log.

One image, two panels, the budget on the x axis (log2), one line per player,
because the same number of bytes buys different things:

    left   the full scan, rows/s     delivery-bound: the ring is flat, the cache
                                     pays to ship the rows again - until its
                                     budget stops it registering anything, and
                                     its line falls to the forward line
    right  count/sums, ms/read (log) execution-bound: a cache hit does no work,
                                     so the cache is far below the ring wherever
                                     its budget lets it hold the entry at all

The budget is the schema of the log: each `# budget M` section runs the ring
with a segment of M/2 and the cache in a container capped at M. The dashed line
on the left is the pass-through workload - the same rows through the proxy with
no cache in front of them - so the cache's own line can be read against what
forwarding alone costs. Where the log's evidence says the cache's registration
was throttled, the budget is shaded: on those bytes the cache cannot hold
anything.

Usage:
    just pgcache-memory-charts                        plots the committed log
    just pgcache-memory-charts --log my.log           plots another run's log
    just pgcache-memory-charts --out assets/          writes the PNG elsewhere
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

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_LOG = ROOT / "docs" / "fastpostgres-vs-pgcache-memory.log"
FILENAME = "pgcache-memory.png"

ACCENT = "#1f77b4"
ORANGE = "#e1812c"
GREY = "#9a9a9a"
SHADE = "#f0f0f0"
SERIES = (
    ("fastpostgres", ACCENT, "fast-postgres (shm ring, M/2 per segment)"),
    ("pgcache", ORANGE, "psycopg 3 → PgCache (capped at M)"),
)

BUDGET_RE = re.compile(r"^# budget ([\d.]+) (KiB|MiB|GiB), segment ")
ROW_RE = re.compile(
    r"^\s+(large|small|through)\s+"
    r"fastpostgres\s+([\d.]+)(M| ms)\s+"
    r"pgcache\s+([\d.]+)(M| ms)"
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
    scale = {"KiB": 1024, "MiB": 1024**2, "GiB": 1024**3}[unit]
    return int(float(value) * scale)


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
            values = {}
            for i, player in enumerate(("fastpostgres", "pgcache")):
                value, unit = float(m.group(2 + 2 * i)), m.group(3 + 2 * i)
                values[player] = value * 1e6 if unit == "M" else value  # rows/s or ms
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


def fmt_budget(v, _pos=None):
    if v >= 1024**3:
        return f"{v / 1024**3:g} GiB"
    if v >= 1024**2:
        return f"{v / 1024**2:g} MiB"
    return f"{v / 1024:g} KiB"


def fmt_rows(v, _pos=None):
    if v >= 1e6:
        return f"{v / 1e6:g}M"
    if v >= 1e3:
        return f"{v / 1e3:g}K"
    return f"{v:g}"


def series(ax, budgets, data, workload, player, color, label, ls="-", marker="o", width=1.8):
    xs = [b for b in budgets if workload in data[b]]
    ys = [data[b][workload][player] for b in xs]
    if not xs:
        return None
    (line,) = ax.plot(xs, ys, color=color, linestyle=ls, marker=marker, markersize=4.5,
                      linewidth=width, label=label, zorder=3)
    return line


def style(ax, ylabel, log_y=False):
    ax.set_xscale("log", base=2)
    ax.xaxis.set_major_formatter(FuncFormatter(fmt_budget))
    ax.xaxis.set_minor_formatter(NullFormatter())
    ax.set_xlabel("memory budget on both sides")
    ax.set_ylabel(ylabel)
    if log_y:
        ax.set_yscale("log")
        ax.yaxis.set_minor_formatter(NullFormatter())
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color("#cccccc")
    ax.tick_params(colors="#666666", length=0)
    ax.margins(y=0.24, x=0.12)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--log", type=Path, default=DEFAULT_LOG, help="the comparison log to plot")
    ap.add_argument("--out", type=Path, default=Path.cwd(), help="where to write the PNG")
    args = ap.parse_args()

    data, evidence, env = parse(args.log.read_text())
    if not data:
        sys.exit(f"{args.log}: no rows found -- run `just pgcache-memory-bench` first")

    budgets = sorted(data)
    fig, (ax_scan, ax_agg) = plt.subplots(1, 2, figsize=(11.0, 5.0))

    # the budgets where the cache held nothing at all: its container was
    # OOM-killed, or every workload's own gauges reported nothing registered. On
    # those bytes it is a plain proxy, so its line there is the forward line - or
    # absent entirely. A *throttled* cache that still holds the query under test
    # is not shaded: the log's evidence lines carry that distinction.
    def empty(b):
        if evidence.get(b, {}).get("down"):
            return True
        gauges = [evidence.get(b, {}).get(w, {}).get("cache") for w in data[b]]
        gauges = [g for g in gauges if g]
        return bool(gauges) and all(g["registered"] == 0 for g in gauges)

    empty_budgets = [b for b in budgets if empty(b)]
    for ax in (ax_scan, ax_agg):
        for b in empty_budgets:
            ax.axvspan(b * 0.72, b * 1.38, color=SHADE, zorder=0)
        style(ax, "rows/s" if ax is ax_scan else "ms/read", log_y=ax is ax_agg)
    if empty_budgets:
        ax_scan.annotate(
            "shaded: the cache holds nothing at that budget\n"
            "(its container OOM-killed, or nothing registered)",
            xy=(empty_budgets[0], 0.06), xycoords=("data", "axes fraction"), fontsize=7,
            color="#888888", ha="center" if len(empty_budgets) > 1 else "left", zorder=4,
        )

    ax_scan.yaxis.set_major_formatter(FuncFormatter(fmt_rows))
    series(ax_scan, budgets, data, "large", "fastpostgres", ACCENT, None)
    series(ax_scan, budgets, data, "large", "pgcache", ORANGE, None)
    series(ax_scan, budgets, data, "through", "pgcache", GREY, None,
           ls="--", marker="s", width=1.2)
    ax_scan.set_title("the full scan: n rows out, delivery-bound", fontsize=9.5)

    ax_agg.yaxis.set_major_formatter(FuncFormatter(lambda v, _p=None: f"{v:g}"))
    # a log axis whose data sits inside one decade gets no major ticks at all,
    # so pin the ticks to the values actually measured
    agg_values = sorted({data[b]["small"][p] for b in budgets if "small" in data[b]
                         for p in ("fastpostgres", "pgcache")})
    if agg_values:
        ax_agg.set_yticks(agg_values)
        ax_agg.set_yticklabels([f"{v:g}" for v in agg_values])
    series(ax_agg, budgets, data, "small", "fastpostgres", ACCENT, None)
    series(ax_agg, budgets, data, "small", "pgcache", ORANGE, None)
    ax_agg.set_title("count + two sums: one row out, execution-bound", fontsize=9.5)

    # label the last point of each line: the number, and for the cache its
    # resolved budget against the one it was asked for
    for ax, workload, unit in ((ax_scan, "large", "M"), (ax_agg, "small", "ms")):
        for player, color in (("fastpostgres", ACCENT), ("pgcache", ORANGE)):
            xs = [b for b in budgets if workload in data[b]]
            if not xs:
                continue
            last = xs[-1]
            value = data[last][workload][player]
            text = f"{value / 1e6:.2f}M rows/s" if unit == "M" else f"{value:.3f} ms"
            ax.annotate(text, (last, value), textcoords="offset points", xytext=(4, 6),
                        fontsize=7, color=color)

    handles = [
        plt.Line2D([], [], color=ACCENT, marker="o", markersize=4.5,
                   label="fast-postgres (shm ring, M/2 per segment)"),
        plt.Line2D([], [], color=ORANGE, marker="o", markersize=4.5,
                   label="psycopg 3 → PgCache (capped at M)"),
        plt.Line2D([], [], color=GREY, linestyle="--", marker="s", markersize=4,
                   label="the cache's pass-through row (no cache in front)"),
    ]
    fig.suptitle("the same memory budget on both sides: what each buys with the bytes",
                 fontsize=12.5, y=0.975)
    fig.legend(handles, [h.get_label() for h in handles], loc="upper center",
               bbox_to_anchor=(0.5, 0.925), ncol=3, frameon=False, fontsize=8.5)

    footer = " · ".join(env.get(k, "?") for k in ENV_KEYS)
    fig.text(0.5, 0.03, footer, ha="center", fontsize=7.5, color="#888888")
    fig.subplots_adjust(left=0.07, right=0.985, top=0.79, bottom=0.14)

    path = args.out / FILENAME
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"wrote {path}")
    for b in budgets:
        cache = evidence.get(b, {}).get("large", {}).get("cache", {})
        ring = evidence.get(b, {}).get("large", {}).get("ring", {})
        print(f"  budget {fmt_budget(b)}: ring seg {ring.get('seg_bytes', 0) / 1048576:g} MiB "
              f"x{ring.get('segments', 0)}, shm_reads {ring.get('shm_reads', 0)}/"
              f"{ring.get('shm_reps', 0)}; cache budget "
              f"{cache.get('budget_bytes', 0) / 1048576:g} MiB, memo "
              f"{cache.get('memo_bytes', 0) / 1048576:g} MiB, registered "
              f"{cache.get('registered', 0)}, throttled {cache.get('throttled', 0)}")


if __name__ == "__main__":
    main()
