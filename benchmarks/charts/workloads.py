#!/usr/bin/env python3
"""Render the workload suite from its log.

Two panels, because the suite answers two questions:

    left   throughput per scenario - statements/s for the app shapes, rows/s for
           the analytics ones - fastpostgres against psycopg 3, the driver the
           same application ran on before
    right  the tail latency of the same scenarios (p95, log scale): an app cares
           about the slow statement, not the average one, and the aggregate's
           single expensive read belongs here with its milliseconds

Each scenario carries its own unit in the tick label, since a lookup's
statements/s and a scan's rows/s are not the same quantity. The footer carries
the machine and the run's `git:` stamp, and says whether the per-statement
digests matched across players.

Usage:
    just charts workloads                       plots the committed log
    just charts workloads --log my.log          plots another run's log
    just charts workloads --out docs/assets/         writes the PNG elsewhere
"""
import argparse
import json
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

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LOG = ROOT / "benchmarks" / "results" / "fastpostgres-vs-psycopg.log"
FILENAME = "workloads.png"

ACCENT = "#1f77b4"
ORANGE = "#e1812c"
SERIES = (("fastpostgres", ACCENT, "fastpostgres"), ("psycopg 3", ORANGE, "psycopg 3"))
LABELS = {"lookup": ("lookup", "stmt/s"), "paged": ("paged page", "stmt/s"),
          "scan": ("scan", "rows/s"), "join": ("join", "rows/s"),
          "concurrent": ("concurrent", "stmt/s"), "mixed": ("mixed read", "reads/s"),
          "aggregate": ("aggregate", "ms/read")}

SCENARIO_RE = re.compile(r"^# scenario (\w+) \(")
ROW_RE = re.compile(
    r"^\s+(\w+)\s+(fastpostgres|psycopg 3)\s+([\d.]+)\s+(\S+)"
    r"\s+p50\s+([\d.]+) ms\s+p95\s+([\d.]+) ms\s+p99\s+([\d.]+) ms"
)
ENV_KEYS = ("cpu", "postgres", "psycopg", "git")


def parse_json(text):
    """The log's `### RESULTS` block, when the run wrote one.

    Since the harness prints a machine-readable record of the same run, the chart
    prefers it: the table below it is for reading, the JSON for drawing.
    """
    marker = "### RESULTS\n"
    if marker not in text:
        return None
    block = text.split(marker, 1)[1].split("### END RESULTS", 1)[0]
    try:
        payload = json.loads(block)
    except ValueError:
        return None
    data = {}
    for record in payload.get("scenarios", []):
        data.setdefault(record["scenario"], {}).setdefault(record["player"], {})[
            record["unit"]] = (float(record["value"]), float(record["p95_ms"]))
    return data


def parse(text):
    """{scenario: {player: {unit: (value, p95)}}}, plus the env and a status.

    A scenario can report more than one unit for a player - the mixed one prints
    reads/s and writes/s - so the unit is part of the key, not the row's position.
    """
    data, env = {}, {}
    status = "ok"
    for line in text.splitlines():
        m = SCENARIO_RE.match(line)
        if m:
            data.setdefault(m.group(1), {})
            continue
        m = ROW_RE.match(line)
        if m:
            scenario, player, value, unit, p95 = (
                m.group(1), m.group(2), float(m.group(3)), m.group(4), float(m.group(6)))
            data.setdefault(scenario, {}).setdefault(player, {})[unit] = (value, p95)
            continue
        if "MISMATCH" in line or "INVALID" in line:
            status = "MISMATCH"
        for key in ENV_KEYS:
            if line.startswith(f"{key}: "):
                env[key] = line.split(": ", 1)[1].strip()
    return data, env, status


def metric(data, scenario, player, unit=None):
    """The (value, p95) for a player, by unit or the only one it reported."""
    units = data[scenario][player]
    if unit is None:
        unit = next(iter(units))
    return units[unit]


def fmt_value(v):
    if v >= 1e6:
        return f"{v / 1e6:.1f}M"
    if v >= 1e3:
        return f"{v / 1e3:.1f}K"
    return f"{v:.1f}"


def fmt_ms(v, _pos=None):
    return f"{v:g}"


def style(ax, log_y=False, yfmt=None):
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color("#cccccc")
    ax.tick_params(colors="#666666", length=0)
    ax.margins(y=0.18)
    if log_y:
        ax.set_yscale("log")
        ax.yaxis.set_major_locator(mticker.LogLocator(base=10.0, subs=(1.0, 2.0, 5.0), numticks=15))
        ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _p=None: yfmt(v)))
        ax.yaxis.set_minor_formatter(mticker.NullFormatter())


def grouped(ax, data, scenarios, index, fmt, ticks, log_y=False, yfmt=None):
    """Grouped bars: `index` 0 is the metric, 1 the p95 of the same rows."""
    width = 0.8 / len(SERIES)
    for i, (player, color, label) in enumerate(SERIES):
        values = []
        for scenario in scenarios:
            value, p95 = metric(data, scenario, player)
            values.append((value, p95)[index])
        pos = [x - 0.4 + (i + 0.5) * width for x in range(len(scenarios))]
        drawn = ax.bar(pos, values, width=width, color=color, label=label, zorder=3)
        ax.bar_label(drawn, labels=[fmt(v) for v in values], fontsize=6.5, padding=2)
    ax.set_xticks(range(len(scenarios)))
    ax.set_xticklabels(ticks, fontsize=8)
    style(ax, log_y=log_y, yfmt=yfmt or fmt)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log", type=Path, default=DEFAULT_LOG, help="the workload log to plot")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "assets", help="where to write the PNG")
    args = ap.parse_args()

    if not args.log.exists():
        sys.exit(f"{args.log}: no log -- run `just bench > {DEFAULT_LOG.relative_to(ROOT)}` first")
    text = args.log.read_text()
    data = parse_json(text)
    env, status = {}, "ok"
    if data is None:
        data, env, status = parse(text)
    else:
        _, env, status = parse(text)
    if not data:
        sys.exit(f"{args.log}: no scenario blocks found -- was this written by `just bench`?")

    throughput = [s for s in ("lookup", "paged", "scan", "join", "concurrent", "mixed") if s in data]
    latency = [s for s in ("lookup", "paged", "aggregate", "concurrent") if s in data]

    fig, (ax_rate, ax_tail) = plt.subplots(1, 2, figsize=(12.0, 5.4))
    # statements/s and rows/s differ by three orders of magnitude here: a log
    # axis keeps the small scenarios visible instead of flat against zero
    grouped(ax_rate, data, throughput, 0, fmt_value,
            [f"{LABELS[s][0]}\n{LABELS[s][1]}" for s in throughput], log_y=True, yfmt=fmt_value)
    ax_rate.set_ylabel("per second, as each scenario's tick says (log)")
    ax_rate.set_title("throughput: what the app asks for, fastpostgres vs psycopg 3", fontsize=9.5)

    grouped(ax_tail, data, latency, 1, fmt_ms,
            [f"{LABELS[s][0]}\np95 ms" for s in latency], log_y=True, yfmt=fmt_ms)
    ax_tail.set_ylabel("ms, p95 of the statements")
    ax_tail.set_title("tail latency: the slow statement, p95, log scale", fontsize=9.5)

    handles = [plt.Rectangle((0, 0), 1, 1, color=color, label=label) for _p, color, label in SERIES]
    fig.suptitle("realistic workloads over a commerce schema, 1M orders", fontsize=12.5, y=0.975)
    fig.legend(handles, [h.get_label() for h in handles], loc="upper center",
               bbox_to_anchor=(0.5, 0.925), ncol=2, frameon=False, fontsize=9)

    footer = " · ".join(env.get(k, "?") for k in ENV_KEYS) + f" · digests {status}"
    fig.text(0.5, 0.03, footer, ha="center", fontsize=7.5, color="#888888")
    fig.subplots_adjust(left=0.075, right=0.985, top=0.80, bottom=0.15)

    path = args.out / FILENAME
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"wrote {path}")
    for scenario in throughput:
        fpg, _ = metric(data, scenario, "fastpostgres")
        pg, _ = metric(data, scenario, "psycopg 3")
        unit = LABELS[scenario][1]
        ratio = f"{fpg / pg:.2f}x" if pg else "-"
        print(f"  {scenario:12s} fastpostgres {fmt_value(fpg):>7s} {unit:8s}"
              f" psycopg 3 {fmt_value(pg):>7s} {unit:8s} ({ratio})")
    if status != "ok":
        sys.exit(f"{args.log}: the digests did not match across players")


if __name__ == "__main__":
    main()
