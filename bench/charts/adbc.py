#!/usr/bin/env python3
"""Run the fast-postgres vs ADBC + Arrow benchmark and render the charts — one shot.

Builds the container, runs the benchmark, then writes two images, one per
head-to-head the harness measures:

    fastpostgres_chart-driver.png     each stack's own DBAPI
    fastpostgres_chart-zerocopy.png   the zero-copy paths, summed in place

Each bar carries its rate; the accent bar (fast-postgres) also carries its
improvement over the rival it is measured against.

`--compare LOG` also prints the before/after deltas for the same loads and
writes those sections again, each load as a before/after bar pair:

    fastpostgres_chart-driver-compare.png / fastpostgres_chart-zerocopy-compare.png

Usage:
    just charts arrow [nrows ...]   (default: the benchmark's own)
    just charts arrow --log bench.log   (plot an existing log)
    just charts arrow --log after.log --compare before.log   (before/after deltas + charts)

`--log` skips the run and plots a saved log instead; the log's environment block
travels with it, so the numbers keep their context.
"""
import argparse
import re
import subprocess
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
OUT = ROOT / "docs" / "assets"

# Strategic colour (Knaflic, ch. 4): mute the baseline to grey, spend the one
# accent on the thing that matters (fast-postgres).
ACCENT = "#1f77b4"
GREY = ("#c8c8c8", "#8a8a8a")


# ---- measuring ------------------------------------------------------------


def run(cmd):
    """Run the benchmark harness and return its output."""
    print(f"==> {' '.join(cmd)}", flush=True)
    proc = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if proc.returncode != 0:
        sys.exit(f"{cmd[0]} failed:\n{proc.stdout[-3000:]}")
    print(proc.stdout, flush=True)
    return proc.stdout


# ---- parsing --------------------------------------------------------------


def parse_sections(text, sections, row_re):
    """{section: {load: (rate_millions, ...)}} for the requested section prefixes."""
    data = {name: {} for name in sections.values()}
    section = None
    for line in text.splitlines():
        for prefix, name in sections.items():
            if line.startswith(prefix):
                section = name
                break
        else:
            if section:
                m = row_re.match(line)
                if m:
                    rates = tuple(float(v[:-1]) for v in m.group(3).split())
                    data[section][f"{m.group(1)}{m.group(2)}"] = rates
    return data


# The two head-to-head sections the benchmark prints, and the players in its
# column order: fast-postgres takes the accent, the Arrow rival is grey. Each
# section compares the same output (tuples, then columnar), so each gets its own
# pair of labels.
CHART_SECTIONS = {"TUPLE READ": "tuples", "ZERO-COPY READ": "zerocopy"}
PLAYERS = ("fastpostgres", "adbc+arrow")
CHART_LABELS = {
    "tuples": ("fast-postgres (typed tuples)", "ADBC + Arrow → tuples"),
    "zerocopy": ("fast-postgres (stream + numpy)", "ADBC Arrow + pyarrow.compute"),
}
COLORS = (ACCENT, GREY[1])
CHARTS = {
    "tuples": (
        "fastpostgres_chart-tuples.png",
        "the same output — Python tuples — from each stack",
    ),
    "zerocopy": (
        "fastpostgres_chart-zerocopy.png",
        "the same output — columnar, one column summed in place",
    ),
}

# A row is: load, one rate per player ("1.36M"), the comparison column, then
# ok/MISMATCH.
_RATE = r"[\d.]+M"
_RATIO = r"(?:[\d.]+x|[+-][\d.]+%)"
DRIVER_ROW = re.compile(rf"^\s*(\d+)([KM])\s+({_RATE}(?:\s+{_RATE})*)\s+({_RATIO}(?:\s+{_RATIO})*)\s+\S+\s*$")


# ---- drawing --------------------------------------------------------------


def num_rows(label):
    """'100K' -> 100_000. The comparison charts key their bars by load *and*
    series ("100K\\npsycopg2"), so reading stops at the K/M suffix."""
    m = re.match(r"([\d.]+)([KM])", label)
    return float(m.group(1)) * (1_000 if m.group(2) == "K" else 1_000_000)


def fmt_rate(v, _pos=None):
    rows = v * 1e6
    if rows >= 1e6:
        return f"{rows / 1e6:g}M"
    if rows >= 1e3:
        return f"{rows / 1e3:g}K"
    return f"{rows:g}"


def fmt_pct(now, base):
    """Improvement over a reference, for a bar label. One decimal below 10% so a
    real but small change is not rounded away, whole numbers above it."""
    if base <= 0:
        return ""
    pct = (now / base - 1) * 100
    return f"{pct:+.1f}%" if abs(pct) < 10 else f"{pct:+.0f}%"


def bars(ax, data, series, colors, labels, scale, delta_vs=0):
    """`delta_vs` marks the series every other bar is measured against — the
    existing driver, or the 'before' bar — so each bar carries its own
    improvement, not just its rate."""
    loads = sorted(data, key=num_rows)
    if not loads:
        ax.set_visible(False)
        return []

    if scale == "auto":
        vals = [v for tup in data.values() for v in tup if v > 0]
        scale = "log" if vals and max(vals) / min(vals) >= 20 else "linear"

    width = 0.8 / len(series)
    xs = list(range(len(loads)))
    handles = []
    for i, name in enumerate(series):
        vals = [data[load][i] for load in loads]
        pos = [x - 0.4 + (i + 0.5) * width for x in xs]
        drawn = ax.bar(pos, vals, width=width, color=colors[i], label=labels[i], zorder=3)
        handles.append(drawn[0])

        texts = []
        for load, v in zip(loads, vals):
            text = fmt_rate(v)
            if delta_vs is not None and i != delta_vs:
                pct = fmt_pct(v, data[load][delta_vs])
                if pct:
                    text += "\n" + pct
            texts.append(text)
        ax.bar_label(drawn, labels=texts, fontsize=6.5, padding=2)

    ax.set_xticks(xs)
    ax.set_xticklabels(loads)
    ax.set_xlabel("rows")
    ax.set_ylabel("rows/s")
    if scale == "log":
        ax.set_yscale("log")
        ax.yaxis.set_minor_formatter(NullFormatter())
    ax.yaxis.set_major_formatter(FuncFormatter(fmt_rate))
    theme.declutter(ax)
    ax.margins(y=0.22)  # room for the second label line
    return handles


def figure(section, data, series, colors, labels, title, headline, footer, scale, delta_vs=0):
    fig, ax = plt.subplots(figsize=(7.8, 5.9))
    handles = bars(ax, data, series, colors, labels, scale, delta_vs)
    ax.set_title(title)
    fig.suptitle(headline, fontsize=13 if len(headline) <= 70 else 10, y=0.975)
    if handles:
        theme.legend(fig, loc="upper center", bbox_to_anchor=(0.5, 0.92), ncol=len(series),
                     handles=handles, labels=labels)
    theme.footer(fig, footer)
    fig.subplots_adjust(left=0.13, right=0.97, top=0.79, bottom=0.12)
    return fig


def save(fig, path, points):
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"wrote {path}  ({points} loads)")


def skip(path, section):
    """No rows for this section: remove a chart from an earlier run so a stale
    image cannot pass for current numbers."""
    if path.exists():
        path.unlink()
        print(f"no '{section}' rows in the log -- removed stale {path.name}")
    else:
        print(f"no '{section}' rows in the log -- nothing to plot")


def paired(before, after, names):
    """{f'{load}\\n{name}': (before, after)} for the loads both logs ran, so one
    x tick is one bar pair rather than one bar."""
    data = {}
    for load in sorted(before, key=num_rows):
        if load not in after:
            continue
        for i, name in enumerate(names):
            was = before[load][i]
            if was > 0:
                data[f"{load}\n{name}"] = (was, after[load][i])
    return data


def headline_compare(prefix, data, target):
    """Headline the change on the series the accent marks ("target")."""
    gains = [now / was - 1 for key, (was, now) in data.items()
             if key.endswith(f"\n{target}") and was > 0]
    best, worst = max(gains, default=0.0), min(gains, default=0.0)
    if best >= 0.005:
        return f"{prefix} — up to {best * 100:.0f}% faster than before"
    if worst <= -0.005:
        return f"{prefix} — up to {abs(worst) * 100:.0f}% slower than before"
    return f"{prefix} — unchanged before vs after"


def plot_compare(db, da, env, scale, note=""):
    """One before/after image per section: grey for before, the accent for after,
    which is all the two-tone key needs — `before`/`after` are the two series."""
    series, colors, labels = ("before", "after"), (GREY[0], ACCENT), ("before", "after")
    footer = spec(env, DRIVER_KEYS) or "fast-postgres vs ADBC + Arrow"
    if note:
        footer += " · " + note

    for section, (filename, subtitle) in CHARTS.items():
        bp, ap = db[section], da[section]
        shared = min([len(v) for v in (*bp.values(), *ap.values())], default=0)
        data = paired(bp, ap, PLAYERS[:shared])
        path = OUT / filename.replace(".png", "-compare.png")
        if not data:
            skip(path, section)
            continue
        fig = figure(section, data, series, colors, labels,
                     f"{subtitle} — before vs after",
                     headline_compare("Read", data, PLAYERS[0]), footer, scale)
        save(fig, path, len(data))


def compare(before_text, after_text, env=None, scale="auto", note=""):
    """Before/after deltas for the same loads — what a hot-path change is for."""
    db = parse_sections(before_text, CHART_SECTIONS, DRIVER_ROW)
    da = parse_sections(after_text, CHART_SECTIONS, DRIVER_ROW)

    widths = {
        section: (max((len(v) for v in db[section].values()), default=0),
                  max((len(v) for v in da[section].values()), default=0))
        for section in CHART_SECTIONS.values()
    }
    if any(before != after for before, after in widths.values()):
        print("note: the logs carry different player columns (the harness changed"
              " between them):\n      the deltas mix the code change with the harness change.")

    print("\nBEFORE -> AFTER (M rows/s)")
    for section in CHART_SECTIONS.values():
        for load in sorted(db[section], key=num_rows):
            if load not in da[section]:
                continue
            shared = min(len(db[section][load]), len(da[section][load]))
            cells = []
            for i, name in enumerate(PLAYERS[:shared]):
                was, now = db[section][load][i], da[section][load][i]
                delta = f"{(now / was - 1) * 100:+.1f}%" if was else "n/a"
                cells.append(f"{name} {was:.2f} -> {now:.2f} ({delta})")
            print(f"  {section:<8} {load:>4}: " + "   ".join(cells))

    print()
    plot_compare(db, da, env if env is not None else parse_env_text(after_text), scale, note)


def spec(env, keys):
    parts = []
    for key, fmt in keys:
        if env.get(key):
            parts.append(fmt.format(env[key]))
    return " · ".join(parts)


DRIVER_KEYS = [
    ("cpu", "{}"), ("shape", "shape {}"), ("postgres", "PostgreSQL {}"),
    ("python", "Python {}"), ("pyarrow", "pyarrow {}"), ("adbc", "ADBC {}"),
]

# The client's one object-arena summary line: it is the reason a curve flattens
# past a few hundred thousand rows, so it belongs next to the numbers.
_OBJPOOL = re.compile(r"^objpool: (\S+) cap=(\d+) MiB used=(\d+) MiB objects=(\d+) fallbacks=(\d+)")


def parse_objpool(text):
    """The client's arena summary, one line per benchmark process: the largest
    cap and the largest footprint, and whether any of them ran out."""
    pools = []
    for line in text.splitlines():
        m = _OBJPOOL.match(line)
        if m:
            mode, cap, used, objects, fallbacks = m.groups()
            pools.append({"mode": mode, "cap": int(cap), "used": int(used), "fallbacks": int(fallbacks)})
    if not pools:
        return None
    return {
        "mode": "exhausted" if any(p["mode"] == "exhausted" for p in pools) else pools[0]["mode"],
        "cap": max(p["cap"] for p in pools),
        "used": max(p["used"] for p in pools),
        "fallbacks": sum(p["fallbacks"] for p in pools),
    }


def pool_note(pool):
    if not pool:
        return ""
    if pool["mode"] == "exhausted":
        return f"object arena exhausted at {pool['cap']} MiB"
    if pool["cap"] >= 1024:
        return f"object arena {pool['used'] / 1024:.1f}/{pool['cap'] / 1024:.0f} GiB"
    return f"object arena {pool['used']}/{pool['cap']} MiB"


def plot_drivers(text, env, scale, note=""):
    data = parse_sections(text, CHART_SECTIONS, DRIVER_ROW)
    footer = spec(env, DRIVER_KEYS) or "fast-postgres vs ADBC + Arrow"
    if note:
        footer += " · " + note
    for section, (filename, subtitle) in CHARTS.items():
        path = OUT / filename
        rows = data[section]
        if not rows:
            skip(path, section)
            continue
        # the accent bar carries the comparison (the rival is the baseline), and
        # the headline takes the best load
        gain = max((v[0] / v[1] - 1 for v in rows.values() if v[1] > 0), default=0.0)
        headline = f"fast-postgres up to {gain * 100:.0f}% faster than ADBC + Arrow"
        fig = figure(section, rows, PLAYERS, COLORS, CHART_LABELS[section], subtitle,
                     headline, footer, scale, delta_vs=1)
        save(fig, path, len(rows))


# ---- main -----------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("sizes", nargs="*", help="row counts (default: the benchmark's own)")
    ap.add_argument("--log", help="plot this log instead of running the benchmark")
    ap.add_argument("--compare", metavar="LOG",
                    help="print before/after deltas against this saved log and chart them")
    ap.add_argument("--scale", choices=("auto", "linear", "log"), default="auto")
    args = ap.parse_args()

    text = Path(args.log).read_text() if args.log else run(["just", "rivals", "adbc", *args.sizes])

    print()
    env = parse_env_text(text)
    note = pool_note(parse_objpool(text))
    plot_drivers(text, env, args.scale, note)
    if args.compare:
        compare(Path(args.compare).read_text(), text, env, args.scale, note)


def parse_env_text(text):
    """parse_env() over captured text rather than a file."""
    env, inside = {}, False
    for line in text.splitlines():
        marker = line.strip()
        if marker == "### ENV":
            env, inside = {}, True
        elif marker == "### END ENV":
            inside = False
        elif inside and ":" in line:
            key, value = line.split(":", 1)
            env[key.strip()] = value.strip()
    return env


if __name__ == "__main__":
    main()
