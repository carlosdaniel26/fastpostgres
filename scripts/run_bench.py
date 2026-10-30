#!/usr/bin/env python3
"""Run the fastpostgres benchmarks and render the charts — one shot.

Builds the container, runs both benchmarks, then writes four images:

    bench_chart-read.png / bench_chart-write.png           tcp / unix / shm
    sqlalchemy_chart-orm.png / sqlalchemy_chart-core.png   psycopg2 / fastpostgres

Usage:
    scripts/run_bench.py [nrows ...]        (default: 100000 1000000 10000000)
    scripts/run_bench.py --transport-log bench.log --drivers-log bench-sqlalchemy.log

`--transport-log` / `--drivers-log` skip the corresponding run and plot an
existing log instead. The 10M default takes a few minutes.
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

ROOT = Path(__file__).resolve().parent.parent
OUT = Path.cwd()

# Strategic colour (Knaflic, ch. 4): mute the baselines to grey, spend the one
# accent on the thing that matters (fast-postgres).
ACCENT = "#1f77b4"
GREY = ("#c8c8c8", "#8a8a8a")


# ---- measuring ------------------------------------------------------------


def run(cmd):
    """Run a benchmark harness and return its output."""
    print(f"==> {' '.join(cmd)}", flush=True)
    proc = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if proc.returncode != 0:
        sys.exit(f"{cmd[0]} failed:\n{proc.stdout[-3000:]}")
    print(proc.stdout, flush=True)
    return proc.stdout


# ---- parsing --------------------------------------------------------------


def parse_sections(text, sections, row_re, values_from):
    """{section: {load: (values...)}} for the requested section prefixes."""
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
                    data[section][f"{m.group(1)}{m.group(2)}"] = tuple(
                        float(m.group(i)) for i in values_from
                    )
    return data


TRANSPORT_SECTIONS = {"READ (rows/s)": "read", "WRITE (rows/s)": "write"}
TRANSPORT_ROW = re.compile(
    r"^\s*(\d+)([KM])\s+([\d.]+)M\s+([\d.]+)M\s+([\d.]+)M\s+[\d.]+x\s+[\d.]+x\s*$"
)

DRIVER_SECTIONS = {"SQLALCHEMY ORM READ": "orm", "SQLALCHEMY CORE READ": "core"}
DRIVER_ROW = re.compile(r"^\s*(\d+)([KM])\s+([\d.]+)M\s+([\d.]+)M\s+[\d.]+x\s+\S+\s*$")


# ---- drawing --------------------------------------------------------------


def num_rows(label):
    n = float(label[:-1])
    return n * (1_000 if label.endswith("K") else 1_000_000)


def fmt_rate(v, _pos=None):
    rows = v * 1e6
    if rows >= 1e6:
        return f"{rows / 1e6:g}M"
    if rows >= 1e3:
        return f"{rows / 1e3:g}K"
    return f"{rows:g}"


def bars(ax, data, series, colors, labels, scale):
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
        ax.bar_label(drawn, labels=[fmt_rate(v) for v in vals], fontsize=7, padding=2)

    ax.set_xticks(xs)
    ax.set_xticklabels(loads)
    ax.set_xlabel("rows")
    ax.set_ylabel("rows/s")
    if scale == "log":
        ax.set_yscale("log")
        ax.yaxis.set_minor_formatter(NullFormatter())
    ax.yaxis.set_major_formatter(FuncFormatter(fmt_rate))
    # Declutter (Knaflic, ch. 3): drop the frame, keep only faint axis lines.
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color("#cccccc")
    ax.tick_params(colors="#666666", length=0)
    ax.margins(y=0.16)
    return handles


def figure(section, data, series, colors, labels, title, headline, footer, scale):
    fig, ax = plt.subplots(figsize=(7.8, 5.9))
    handles = bars(ax, data, series, colors, labels, scale)
    ax.set_title(title)
    fig.suptitle(headline, fontsize=13, y=0.975)
    if handles:
        fig.legend(handles, labels, loc="upper center", bbox_to_anchor=(0.5, 0.92),
                   ncol=len(series), frameon=False)
    fig.text(0.5, 0.03, footer, ha="center", fontsize=7.5, color="#888888")
    fig.subplots_adjust(left=0.13, right=0.97, top=0.79, bottom=0.12)
    return fig


def save(fig, path):
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"wrote {path}")


def spec(env, keys):
    parts = []
    for key, fmt in keys:
        if env.get(key):
            parts.append(fmt.format(env[key]))
    return " · ".join(parts)


TRANSPORT_KEYS = [
    ("ghz", "{} GHz"), ("cores", "{} cores"), ("postgres", "PostgreSQL {}"),
    ("python", "Python {}"), ("shm", "/dev/shm {}"),
]
DRIVER_KEYS = TRANSPORT_KEYS[:4] + [("sqlalchemy", "SQLAlchemy {0}")]


def headline_transport(data):
    ratio = max((v[2] / v[0] for v in data.values() if v[0] > 0), default=0.0)
    if ratio >= 1.5:
        return f"fast-postgres — up to {ratio:.0f}× the socket transports"
    return "fast-postgres — throughput by transport"


def plot_transport(text, env, scale):
    data = parse_sections(text, TRANSPORT_SECTIONS, TRANSPORT_ROW, (3, 4, 5))
    footer = spec(env, TRANSPORT_KEYS) or "fastpostgres transport benchmark"
    titles = {"read": "Read: rows fetched per second", "write": "Write: rows inserted per second"}
    for section in ("read", "write"):
        if not data[section]:
            continue
        fig = figure(section, data[section], ("tcp", "unix", "shm"), GREY + (ACCENT,),
                     ("tcp", "unix", "fast-postgres"), titles[section],
                     headline_transport(data[section]), footer, scale)
        save(fig, OUT / f"bench_chart-{section}.png")


def plot_drivers(text, env, scale):
    data = parse_sections(text, DRIVER_SECTIONS, DRIVER_ROW, (3, 4))
    footer = spec(env, DRIVER_KEYS) or "SQLAlchemy driver benchmark"
    titles = {"orm": "ORM read: select(Model) -> Python objects",
              "core": "Core read: select(cols) -> Row tuples"}
    for section in ("orm", "core"):
        if not data[section]:
            continue
        ratio = max((v[1] / v[0] for v in data[section].values() if v[0] > 0), default=0.0)
        label = "ORM" if section == "orm" else "Core"
        if ratio >= 1.15:
            headline = f"SQLAlchemy {label} reads — fast-postgres up to {ratio:.2f}× psycopg2"
        else:
            headline = f"SQLAlchemy {label} reads — driver makes little difference"
        fig = figure(section, data[section], ("psycopg2", "fastpostgres"), (GREY[0], ACCENT),
                     ("psycopg2", "fast-postgres"), titles[section], headline, footer, scale)
        save(fig, OUT / f"sqlalchemy_chart-{section}.png")


# ---- main -----------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("sizes", nargs="*", help="row counts (default: 100000 1000000 10000000)")
    ap.add_argument("--transport-log", help="plot this log instead of running the transport benchmark")
    ap.add_argument("--drivers-log", help="plot this log instead of running the driver benchmark")
    ap.add_argument("--scale", choices=("auto", "linear", "log"), default="auto")
    args = ap.parse_args()

    sizes = args.sizes or ["100000", "1000000", "10000000"]

    transport = Path(args.transport_log).read_text() if args.transport_log else run(
        [str(ROOT / "scripts/bench-docker.sh"), *sizes])
    drivers = Path(args.drivers_log).read_text() if args.drivers_log else run(
        [str(ROOT / "scripts/bench-sqlalchemy-docker.sh"), *sizes])

    print()
    plot_transport(transport, parse_env_text(transport), args.scale)
    plot_drivers(drivers, parse_env_text(drivers), args.scale)


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
