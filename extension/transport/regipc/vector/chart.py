#!/usr/bin/env python3
"""What the two ways of carrying bytes cost, from the sweep's own record.

    python3 chart.py                     # sweep-bulk.jsonl, else sweep.jsonl
    python3 chart.py --log other.jsonl --out .

One figure, three panels, two channels:

  p50 per call   the width axis: the vector call (CALL_V, FP save/restore +
                 image copy) against the copy call (CALL_B, copy_from_user
                 from a caller buffer), hot and cold source
  calls/s        what each channel costs in rate
  GB/s           what each carries; the vector channel is drawn both as arena
                 bytes (what it copies) and as real AVX-512 image bytes (what
                 of that is not zero fill), because only the latter is state

Every number plotted or annotated comes from the jsonl lines. The only outside
number is the plain nine-GPR reference line: 0.521 us, measured in
bench/results/transport-shm-vs-regipc.log (the ABI 2 baseline), as in the
original version of this chart.
"""
import argparse
import json
import statistics
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt  # noqa: E402

HERE = Path(__file__).resolve().parent

sys.path.insert(0, str(HERE.parents[3] / "bench" / "charts"))
import theme  # noqa: E402

DEFAULT_LOGS = [HERE / "sweep-bulk.jsonl", HERE / "sweep.jsonl"]
FILENAME = "regipc-copy-vs-vector.png"
PLAIN_CALL_US = 0.521  # the ABI 2 nine-GPR call, bench/results/transport-shm-vs-regipc.log

# The series, in draw order. (key, colour, label)
CHANNELS = [
    ("vec", "#dd6b20", "vector CALL_V"),
    ("bulk-hot", "#2b6cb0", "copy CALL_B, hot source"),
    ("bulk-cold", "#63b3ed", "copy CALL_B, cold source"),
]
GB_SERIES = [
    ("vec", "#dd6b20", "vector CALL_V, arena bytes"),
    ("vec-real", "#f6ad55", "vector CALL_V, real AVX-512 bytes"),
    ("bulk-hot", "#2b6cb0", "copy CALL_B, hot source"),
    ("bulk-cold", "#63b3ed", "copy CALL_B, cold source"),
]


def series_of(run):
    if run.get("mode") == "vec":
        return "vec"
    if run.get("mode") == "bulk":
        return "bulk-cold" if run.get("cold") else "bulk-hot"
    return None


def load_runs(paths):
    runs = []
    for path in paths:
        if not path.exists():
            sys.exit(f"{path}: no sweep file -- run `make -C extension/transport/regipc/vector bulk-sweep` first")
        for line in path.read_text().splitlines():
            line = line.strip()
            if not line.startswith("{"):
                continue
            run = json.loads(line)
            if "width" in run and series_of(run) is not None:
                runs.append(run)
    if not runs:
        sys.exit("no vec/bulk runs in " + ", ".join(str(p) for p in paths))
    return runs


def grouped_bars(ax, widths, medians, series_defs, scale, label_fmt):
    """Bars grouped by width, one offset per series that has data there."""
    n = len(series_defs)
    slot = 0.8 / n
    for index, (key, colour, label) in enumerate(series_defs):
        xs, values = [], []
        for position, width in enumerate(widths):
            value = medians.get((key, width))
            if value is None:
                continue
            xs.append(position - 0.4 + slot * (index + 0.5))
            values.append(value / scale)
        if not xs:
            continue
        bars = ax.bar(xs, values, slot * 0.92, color=colour, label=label)
        for bar, value in zip(bars, values):
            ax.annotate(label_fmt(value), (bar.get_x() + bar.get_width() / 2, bar.get_height()),
                        textcoords="offset points", xytext=(0, 4), ha="center", fontsize=7.5)


def main():
    ap = argparse.ArgumentParser(description="Chart the vector channel against the copy channel")
    ap.add_argument("--log", type=Path, action="append", default=None,
                    help="jsonl file(s); default: sweep-bulk.jsonl if present, else sweep.jsonl")
    ap.add_argument("--out", type=Path, default=HERE)
    args = ap.parse_args()

    if args.log:
        logs = args.log
    else:
        same_session = [p for p in DEFAULT_LOGS if p.exists()]
        logs = same_session[:1] if same_session else DEFAULT_LOGS[:1]

    runs = load_runs(logs)
    groups = {}
    for run in runs:
        groups.setdefault((series_of(run), run["width"]), []).append(run)

    widths = sorted({run["width"] for run in runs})
    p50 = {key: statistics.median(r["us_per_call_p50"] for r in rs)
           for key, rs in groups.items()}
    rate = {key: statistics.median(r["calls_per_second"] for r in rs)
            for key, rs in groups.items()}
    gbps = {key: statistics.median(r["gb_per_second"] for r in rs)
            for key, rs in groups.items()}

    # The vector's real (non-zero-fill) bytes: scale its arena rate by the
    # fraction of the copy that is actual state, per width, from the record.
    gbps_real = {}
    for (key, width), rs in groups.items():
        if key != "vec" or width == 0:
            continue
        real = rs[0].get("real_bytes", 0)
        gbps_real[key, width] = gbps[key, width] * real / width

    present = [key for key, _, _ in CHANNELS if any(k == key for k, _ in groups)]
    present_gb = [key for key, _, _ in GB_SERIES
                  if any(k == key for k, _ in groups) or any(k == key for k, _ in gbps_real)]
    verified = all(r.get("verified") for r in runs)
    labels = [f"{w} B\n(FP only)" if w == 0 else f"{w} B" for w in widths]

    figure, axes = plt.subplots(1, 3, figsize=(14.5, 5.0))

    ax = axes[0]
    grouped_bars(ax, widths, p50, [s for s in CHANNELS if s[0] in present], 1.0, lambda v: f"{v:.3f}")
    ax.axhline(PLAIN_CALL_US, color="#4a5568", lw=1.2, ls="--")
    ax.annotate(f"plain nine-GPR call {PLAIN_CALL_US:.3f} us",
                (len(widths) - 0.55, PLAIN_CALL_US), textcoords="offset points", xytext=(0, 5),
                ha="right", fontsize=8, color="#4a5568")
    top = max(p50.values()) if p50 else 1.0
    ax.set_ylim(0, top * 1.28)
    ax.set_ylabel("microseconds per call")

    ax = axes[1]
    grouped_bars(ax, widths, rate, [s for s in CHANNELS if s[0] in present], 1e6, lambda v: f"{v:.2f}M")
    top = max(rate.values()) if rate else 1.0
    ax.set_ylim(0, top / 1e6 * 1.28)
    ax.set_ylabel("millions of calls per second")

    ax = axes[2]
    gb_med = dict(gbps)
    gb_med.update(gbps_real)
    grouped_bars(ax, widths, gb_med, [s for s in GB_SERIES if s[0] in present_gb], 1.0, lambda v: f"{v:.2f}")
    ax.set_yscale("log")
    ax.set_ylim(0.05, max(gb_med.values()) * 4)
    ax.set_ylabel("GB/s (log scale)")

    for ax in axes:
        ax.set_xticks(list(range(len(widths))))
        ax.set_xticklabels(labels, fontsize=9)
        ax.set_xlabel("bytes per call")
        theme.declutter(ax)
        theme.legend(ax, loc="upper left", fontsize=7.5)
        ax.set_xlim(-0.6, len(widths) - 0.4)

    axes[0].set_title("what one call costs", fontsize=10.5)
    axes[1].set_title("what that buys in rate", fontsize=10.5)
    axes[2].set_title("what the bytes carry", fontsize=10.5)
    figure.suptitle("regipc: vector state (CALL_V) against a plain copy (CALL_B)", fontsize=12.5, y=0.99)

    # Everything below is computed from the jsonl, never typed.
    def best(key):
        pts = [(w, gb_med[key, w]) for w in widths if (key, w) in gb_med]
        return max(pts, key=lambda item: item[1]) if pts else None

    hot, real = best("bulk-hot"), best("vec-real")
    if hot and real:
        theme.footer(figure,
                     f"best copy {hot[1]:.2f} GB/s at {hot[0]} B vs vector {real[1]:.2f} GB/s of real state "
                     f"at {real[0]} B; median of {len(runs)} runs, every run verified"
                     if verified else
                     f"best copy {hot[1]:.2f} GB/s at {hot[0]} B vs vector {real[1]:.2f} GB/s of real state "
                     f"at {real[0]} B; median of {len(runs)} runs (WARNING: unverified runs included)",
                     y=0.925)
    theme.footer(figure,
                 f"source: {', '.join(str(p) for p in logs)} - bars are medians of the per-run p50 / rate; "
                 "the vector channel's FP save/restore is paid at every width, the copy channel's is zero.",
                 y=0.015)
    figure.subplots_adjust(left=0.055, right=0.985, top=0.78, bottom=0.17, wspace=0.26)

    args.out.mkdir(parents=True, exist_ok=True)
    path = args.out / FILENAME
    figure.savefig(path, dpi=150, bbox_inches="tight")
    print(f"wrote {path}" + ("" if verified else "  (WARNING: some run is unverified)"))


if __name__ == "__main__":
    main()
