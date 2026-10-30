#!/usr/bin/env python3
"""Plot the fastpostgres benchmark as comparison bars — one image per phase.

Emits two images: ``<out stem>-read<ext>`` and ``<out stem>-write<ext>``. Each
groups bars by load size, with one bar per transport (tcp / unix socket / shared
memory); bar height is throughput in millions of rows/s, so the transports can
be compared at a glance.

Data comes from a benchmark log (`scripts/bench-docker.sh` output) when one is
passed with --log or found as ./bench.log; otherwise an embedded sample run is
used.

Usage:
    scripts/bench-docker.sh 100000 1000000 > bench.log
    scripts/plot_bench.py --log bench.log --out bench_chart.png
      -> writes bench_chart-read.png and bench_chart-write.png
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

SERIES = ("tcp", "unix", "shm")
# Strategic colour (Knaflic, ch. 4): mute the baseline transports to grey and
# spend the one accent colour on the thing that matters (fast-postgres).
COLORS = {"tcp": "#c8c8c8", "unix": "#8a8a8a", "shm": "#1f77b4"}
LABELS = {"tcp": "tcp", "unix": "unix", "shm": "fast-postgres"}
TITLES = {
    "read": "Read: rows fetched per second",
    "write": "Write: rows inserted per second",
}

# "<load>  <tcp>M  <unix>M  <shm>M  <speedup>x  <speedup>x"
ROW_RE = re.compile(
    r"^\s*(\d+)([KM])\s+([\d.]+)M\s+([\d.]+)M\s+([\d.]+)M\s+[\d.]+x\s+[\d.]+x\s*$"
)

# rows/s (millions) from a sample run, used when no benchmark log is available
SAMPLE = {
    "read": {"100K": (0.82, 0.83, 7.54), "1M": (0.82, 0.84, 8.41)},
    "write": {"100K": (1.02, 0.96, 2.88), "1M": (0.90, 0.90, 2.54)},
}


def parse_log(path):
    """Extract {read|write: {load: (tcp, unix, shm)}} rates in millions of rows/s."""
    data = {"read": {}, "write": {}}
    section = None
    for line in Path(path).read_text().splitlines():
        if line.startswith("READ (rows/s)"):
            section = "read"
        elif line.startswith("WRITE (rows/s)"):
            section = "write"
        elif section:
            m = ROW_RE.match(line)
            if m:
                data[section][f"{m.group(1)}{m.group(2)}"] = tuple(
                    float(m.group(i)) for i in (3, 4, 5)
                )
    return data


def parse_env(path):
    """Read the last '### ENV' block (environment specs) from a benchmark log."""
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
    """Format a throughput value (in millions of rows/s) with a K/M suffix."""
    rows = v * 1e6
    if rows >= 1e6:
        return f"{rows / 1e6:g}M"
    if rows >= 1e3:
        return f"{rows / 1e3:g}K"
    return f"{rows:g}"


def choose_scale(section, data, requested):
    """auto: log only when the values span a wide range (keeps small bars visible)."""
    if requested != "auto":
        return requested
    vals = [v for tup in data.get(section, {}).values() for v in tup if v > 0]
    if not vals:
        return "linear"
    return "log" if max(vals) / min(vals) >= 20 else "linear"


def draw(ax, section, data, title, scale):
    loads = sorted(data[section], key=num_rows)
    if not loads:
        ax.set_visible(False)
        return []

    scale = choose_scale(section, data, scale)
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
    # Declutter (Knaflic, ch. 3): drop the frame, keep only faint axis lines.
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color("#cccccc")
    ax.tick_params(colors="#666666", length=0)
    ax.margins(y=0.16)
    return handles


def headline(section, data):
    """A title that states the takeaway for this section (Knaflic, ch. 7)."""
    sec = data.get(section, {})
    ratio = max((v[2] / v[0] for v in sec.values() if v[0] > 0), default=0.0)
    if ratio >= 1.5:
        return f"fast-postgres — up to {ratio:.0f}× the socket transports"
    return "fast-postgres — throughput by transport"


def make_figure(section, data, args, source, env):
    fig, ax = plt.subplots(figsize=(7.8, 5.9))
    handles = draw(ax, section, data, TITLES[section], args.scale)
    fig.suptitle(headline(section, data), fontsize=13, y=0.975)
    if handles:
        fig.legend(handles, [LABELS[s] for s in SERIES], loc="upper center",
                   bbox_to_anchor=(0.5, 0.92), ncol=len(SERIES), frameon=False)

    # Environment the numbers were measured in (captured by the bench script):
    # clock, cores, PostgreSQL, Python and the shared-memory transport size.
    spec = " · ".join(filter(None, [
        f"{env['ghz']} GHz" if env.get("ghz") else None,
        f"{env['cores']} cores" if env.get("cores") else None,
        f"PostgreSQL {env['postgres']}" if env.get("postgres") else None,
        f"Python {env['python']}" if env.get("python") else None,
        f"/dev/shm {env['shm']}" if env.get("shm") else None,
    ]))
    footer = spec or f"{source} · {args.scale} scale"
    fig.text(0.5, 0.03, footer, ha="center", fontsize=7.5, color="#888888")

    fig.subplots_adjust(left=0.13, right=0.97, top=0.79, bottom=0.12)
    return fig


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--log", help="benchmark log to parse (default: ./bench.log if present)")
    ap.add_argument("--out", default="bench_chart.png",
                    help="output image base path; -read/-write are appended (default: %(default)s)")
    ap.add_argument("--scale", choices=("auto", "linear", "log"), default="auto",
                    help="y-axis scale; auto uses log for wide ranges (default: %(default)s)")
    args = ap.parse_args()

    if args.log:
        log_path = args.log
    elif Path("bench.log").exists():
        log_path = "bench.log"
    else:
        log_path = None
    if log_path:
        data, source, env = parse_log(log_path), log_path, parse_env(log_path)
    else:
        data, source, env = SAMPLE, "embedded sample", {}
    if not data["read"] and not data["write"]:
        sys.exit(f"no benchmark rows found in {source}")

    out = Path(args.out)
    suffix = out.suffix or ".png"
    for section in ("read", "write"):
        if not data.get(section):
            continue
        fig = make_figure(section, data, args, source, env)
        path = out.with_name(f"{out.stem}-{section}{suffix}")
        fig.savefig(path, dpi=150)
        plt.close(fig)
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
