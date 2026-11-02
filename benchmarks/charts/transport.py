#!/usr/bin/env python3
"""The ring's two doorbells, drawn from the record the harness wrote.

    just charts transport                        benchmarks/results/transport-shm-vs-regipc.log
    just charts transport --log /tmp/run.log     any other run of the same harness
    just charts transport --out docs/assets      where the PNG goes

Three panels, one per question the record answers:

  hops/s        how many request/response handoffs each doorbell carries
  us per hop    the p50 round trip of one handoff (the p99 is in the record)
  us CPU/hop    what the two processes together spent on it

The third is the one the first two hide: the regipc engine spins while it waits
(both sides, until its fallback semaphore), so its hops are cheap but they are
paid for in CPU; the shm engine sleeps on a futex and hands the core back. Both
panels are log scale because a 10x gap in a linear axis draws one bar as nothing.

The log is the harness's stdout, so the machine block and the `git:` stamp come
from the run itself, like every other chart here.
"""
import argparse
import json
import re
import statistics
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LOG = ROOT / "benchmarks" / "results" / "transport-shm-vs-regipc.log"
FILENAME = "transport.png"

METHOD_LABELS = {
    "shm": "shm (semaphores: a futex wake per handoff)",
    "regipc": "regipc (a kernel call per handoff)",
}
COLORS = {"shm": "#2b6cb0", "regipc": "#dd6b20"}
ENV_KEYS = ("cpu", "cores", "kernel", "regipc", "batch_rows", "git")


def parse_env(text):
    """The harness's `### TRANSPORT` block, as key -> value."""
    block = text.split("### END TRANSPORT", 1)[0]
    env = {}
    for key in ENV_KEYS:
        match = re.search(rf"^{re.escape(key)}: (.+)$", block, re.M)
        if match:
            env[key] = match.group(1).strip()
    return env


def parse_runs(text):
    """The `### RESULTS` block: the machine block and one record per run."""
    if "### RESULTS\n" not in text:
        sys.exit("transport chart: the log has no ### RESULTS block -- run `just internals transport` first")
    payload = json.loads(text.split("### RESULTS\n", 1)[1].split("### END RESULTS", 1)[0])
    runs = [run for run in payload.get("runs", []) if run.get("verified")]
    if not runs:
        sys.exit("transport chart: no verified run in the record")
    return payload.get("machine", {}), runs


def hops_per_second(run):
    return float(run.get("hops_per_second") or 0.0)


def us_per_hop(run, which="p50"):
    return float(run.get(f"us_per_hop_{which}") or 0.0)


def us_cpu_per_hop(run):
    cpu = float(run.get("producer_cpu_s") or 0.0) + float(run.get("consumer_cpu_s") or 0.0)
    count = float(run.get("count") or 0.0)
    return cpu / count * 1e6 if count else 0.0


def group(runs, extract):
    """{(method, count): (median, min, max, reps)} over the repetitions."""
    buckets = {}
    for run in runs:
        buckets.setdefault((run["method"], int(run["count"])), []).append(extract(run))
    return {key: (statistics.median(v), min(v), max(v)) for key, v in buckets.items()}


def human(value):
    for unit, scale in (("G", 1e9), ("M", 1e6), ("k", 1e3)):
        if value >= scale:
            return f"{value / scale:.2f}{unit}".rstrip("0").rstrip(".")
    return f"{value:.0f}"


def panel(ax, groups, title, ylabel, formatter, log=True):
    methods = [m for m in ("shm", "regipc") if any(key[0] == m for key in groups)]
    counts = sorted({count for _, count in groups})
    width = 0.8 / max(len(methods), 1)

    for index, method in enumerate(methods):
        xs, ys, lows, highs = [], [], [], []
        for slot, count in enumerate(counts):
            point = groups.get((method, count))
            if not point:
                continue
            median, low, high = point
            xs.append(slot + (index - (len(methods) - 1) / 2) * width)
            ys.append(median)
            lows.append(max(median - low, 0.0))
            highs.append(max(high - median, 0.0))
        bars = ax.bar(xs, ys, width * 0.9, yerr=[lows, highs], capsize=4,
                      label=METHOD_LABELS[method], color=COLORS[method],
                      error_kw={"elinewidth": 1.0, "ecolor": "#4a5568"})
        for bar, value in zip(bars, ys):
            ax.annotate(formatter(value), (bar.get_x() + bar.get_width() / 2, value),
                        textcoords="offset points", xytext=(0, 9), ha="center", fontsize=9)

    if log:
        ax.set_yscale("log")
        ax.set_ylabel(f"{ylabel} (log scale)")
    else:
        ax.set_ylabel(ylabel)
    ax.set_title(title, fontsize=10.5)
    ax.set_xticks(range(len(counts)))
    ax.set_xticklabels([f"{human(c)} hops" for c in counts])
    ax.grid(axis="y", alpha=0.25, which="both")
    ax.set_axisbelow(True)
    ax.margins(y=0.30)


def main():
    ap = argparse.ArgumentParser(description="Chart the two doorbells from their record")
    ap.add_argument("--log", type=Path, default=DEFAULT_LOG, help="the harness's stdout record")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "assets", help="where to write the PNG")
    args = ap.parse_args()

    if not args.log.exists():
        sys.exit(f"{args.log}: no log -- run `just internals transport > {DEFAULT_LOG.relative_to(ROOT)}` first")

    text = args.log.read_text()
    env = parse_env(text)
    machine, runs = parse_runs(text)

    figure, axes = plt.subplots(1, 3, figsize=(13.5, 5.2))
    panel(axes[0], group(runs, hops_per_second), "Handoffs per second", "hops/s", human)
    panel(axes[1], group(runs, us_per_hop), "One handoff, end to end",
          "microseconds", lambda value: f"{value:.2f}")
    panel(axes[2], group(runs, us_cpu_per_hop), "What the handoff cost the machine",
          "microseconds of CPU", lambda value: f"{value:.2f}")

    axes[1].legend(loc="upper center", bbox_to_anchor=(0.5, -0.14), ncol=2, fontsize=9)
    footer = "median of 3 runs per bar, whiskers = min..max. Doorbell only: no data segment and no payload."
    figure.suptitle("The ring's two doorbells: the semaphores against the regipc kernel call",
                    fontsize=12, y=0.99)
    figure.text(0.5, 0.015, footer, ha="center", fontsize=8.5, color="#4a5568")
    figure.text(0.5, 0.925,
                f"{env.get('cpu', 'unknown')} · {env.get('cores', '?')} cores · "
                f"kernel {env.get('kernel', '?')} · batch {env.get('batch_rows', '?')} rows · "
                f"git {machine.get('git') or env.get('git', '?')}",
                ha="center", fontsize=8.5, color="#4a5568")
    figure.subplots_adjust(left=0.06, right=0.985, top=0.78, bottom=0.20, wspace=0.30)

    args.out.mkdir(parents=True, exist_ok=True)
    path = args.out / FILENAME
    figure.savefig(path, dpi=150, bbox_inches="tight")
    print(f"wrote {path.relative_to(ROOT) if path.is_relative_to(ROOT) else path}")


if __name__ == "__main__":
    main()
