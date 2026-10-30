#!/usr/bin/env python3
"""Plot the SQLAlchemy driver benchmark — one image per phase.

Emits ``<out stem>-orm<ext>`` and ``<out stem>-core<ext>``: bars grouped by
load size, one bar per driver (psycopg2 vs fastpostgres), height in millions of
rows/s. The two phases are the point of the chart — the ORM hydrates Python
objects and dilutes the transport, the Core phase does not.

Usage:
    scripts/bench-sqlalchemy-docker.sh 10000 50000 200000 > bench-sqlalchemy.log
    scripts/plot_drivers.py --log bench-sqlalchemy.log --out sqlalchemy_chart.png
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

SERIES = ("psycopg2", "fastpostgres")
COLORS = {"psycopg2": "#c8c8c8", "fastpostgres": "#1f77b4"}
LABELS = {"psycopg2": "psycopg2", "fastpostgres": "fast-postgres"}
TITLES = {
    "orm": "ORM read: select(Model) -> Python objects",
    "core": "Core read: select(cols) -> Row tuples",
}
SECTIONS = {"SQLALCHEMY ORM READ": "orm", "SQLALCHEMY CORE READ": "core"}

ROW_RE = re.compile(
    r"^\s*(\d+)([KM])\s+([\d.]+)M\s+([\d.]+)M\s+([\d.]+)x\s+\S+\s*$"
)


def parse_log(path):
    data = {"orm": {}, "core": {}}
    section = None
    for line in Path(path).read_text().splitlines():
        for prefix, name in SECTIONS.items():
            if line.startswith(prefix):
                section = name
                break
        else:
            if section:
                m = ROW_RE.match(line)
                if m:
                    data[section][f"{m.group(1)}{m.group(2)}"] = (
                        float(m.group(3)),
                        float(m.group(4)),
                    )
    return data


def parse_env(path):
    env, inside = {}, False
    for line in Path(path).read_text().splitlines():
        marker = line.strip()
        if marker == "### ENV":
            env, inside = {}, True
        elif marker == "### END ENV":
            inside = False
        elif inside and ":" in line:
            key, value = line.split(":", 1)
            env[key.strip()] = value.strip()
    return env


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


def draw(ax, section, data, title, scale):
    loads = sorted(data[section], key=num_rows)
    if not loads:
        ax.set_visible(False)
        return []

    vals_all = [v for tup in data[section].values() for v in tup if v > 0]
    if scale == "auto":
        scale = "log" if vals_all and max(vals_all) / min(vals_all) >= 20 else "linear"

    n = len(SERIES)
    width = 0.8 / n
    xs = list(range(len(loads)))
    handles = []
    for i, name in enumerate(SERIES):
        vals = [data[section][load][i] for load in loads]
        pos = [x - 0.4 + (i + 0.5) * width for x in xs]
        bars = ax.bar(pos, vals, width=width, color=COLORS[name], label=LABELS[name], zorder=3)
        handles.append(bars[0])
        ax.bar_label(bars, labels=[fmt_rate(v) for v in vals], fontsize=7, padding=2)

    ax.set_xticks(xs)
    ax.set_xticklabels(loads)
    ax.set_xlabel("rows")
    ax.set_ylabel("rows/s")
    ax.set_title(title)
    if scale == "log":
        ax.set_yscale("log")
    ax.yaxis.set_major_formatter(FuncFormatter(fmt_rate))
    if scale == "log":
        ax.yaxis.set_minor_formatter(NullFormatter())
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color("#cccccc")
    ax.tick_params(colors="#666666", length=0)
    ax.margins(y=0.16)
    return handles


def headline(section, data):
    sec = data.get(section, {})
    ratio = max((v[1] / v[0] for v in sec.values() if v[0] > 0), default=0.0)
    label = "ORM" if section == "orm" else "Core"
    if ratio >= 1.15:
        return f"SQLAlchemy {label} reads — fast-postgres up to {ratio:.2f}× psycopg2"
    return f"SQLAlchemy {label} reads — driver makes little difference"


def make_figure(section, data, args, env):
    fig, ax = plt.subplots(figsize=(7.8, 5.9))
    handles = draw(ax, section, data, TITLES[section], args.scale)
    fig.suptitle(headline(section, data), fontsize=13, y=0.975)
    if handles:
        fig.legend(handles, [LABELS[s] for s in SERIES], loc="upper center",
                   bbox_to_anchor=(0.5, 0.92), ncol=len(SERIES), frameon=False)

    spec = " · ".join(filter(None, [
        f"{env['ghz']} GHz" if env.get("ghz") else None,
        f"{env['cores']} cores" if env.get("cores") else None,
        f"PostgreSQL {env['postgres']}" if env.get("postgres") else None,
        f"Python {env['python']}" if env.get("python") else None,
        f"SQLAlchemy {env['sqlalchemy']}" if env.get("sqlalchemy") else None,
    ]))
    fig.text(0.5, 0.03, spec or "SQLAlchemy driver benchmark", ha="center", fontsize=7.5,
             color="#888888")
    fig.subplots_adjust(left=0.13, right=0.97, top=0.79, bottom=0.12)
    return fig


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--log", help="benchmark log (default: ./bench-sqlalchemy.log if present)")
    ap.add_argument("--out", default="sqlalchemy_chart.png",
                    help="output base path; -orm/-core are appended (default: %(default)s)")
    ap.add_argument("--scale", choices=("auto", "linear", "log"), default="auto")
    args = ap.parse_args()

    if args.log:
        log_path = args.log
    elif Path("bench-sqlalchemy.log").exists():
        log_path = "bench-sqlalchemy.log"
    else:
        sys.exit("no benchmark log: pass --log or run scripts/bench-sqlalchemy-docker.sh")

    data, env = parse_log(log_path), parse_env(log_path)
    if not data["orm"] and not data["core"]:
        sys.exit(f"no benchmark rows found in {log_path}")

    out = Path(args.out)
    suffix = out.suffix or ".png"
    for section in ("orm", "core"):
        if not data.get(section):
            continue
        fig = make_figure(section, data, args, env)
        path = out.with_name(f"{out.stem}-{section}{suffix}")
        fig.savefig(path, dpi=150)
        plt.close(fig)
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
