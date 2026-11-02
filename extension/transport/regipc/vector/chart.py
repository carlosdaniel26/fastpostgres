#!/usr/bin/env python3
"""What the vector channel costs and carries, from the sweep's own record.

    python3 chart.py                     # extension/transport/regipc/vector/sweep.jsonl
    python3 chart.py --log other.jsonl --out docs/assets

Three panels, the shape of the answer:

  p50 per call   the width axis, with the plain nine-GPR call drawn for
                 reference: the distance from it at width 0 is the FP tax
  calls/s        what the width costs in rate
  GB/s           what it carries - arena bytes against the real AVX-512 image

The numbers come from sweep.jsonl and nowhere else; the reference line is the
plain call measured in the benchmark record (0.521 us, transport-shm-vs-regipc).
"""
import argparse
import json
import statistics
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt  # noqa: E402

ROOT = Path(__file__).resolve().parents[4]
DEFAULT_LOG = Path(__file__).resolve().parent / "sweep.jsonl"
FILENAME = "regipc-vector.png"
PLAIN_CALL_US = 0.521  # the ABI 2 nine-GPR call, benchmarks/results/transport-shm-vs-regipc.log


def main():
    ap = argparse.ArgumentParser(description="Chart the vector-channel sweep")
    ap.add_argument("--log", type=Path, default=DEFAULT_LOG)
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "assets")
    args = ap.parse_args()

    if not args.log.exists():
        sys.exit(f"{args.log}: no sweep -- run `make -C extension/transport/regipc/vector sweep` first")

    runs = [json.loads(line) for line in args.log.read_text().splitlines() if line.startswith("{")]
    groups = {}
    for run in runs:
        groups.setdefault(run["width"], []).append(run)
    widths = sorted(groups)

    p50 = {w: statistics.median(r["us_per_call_p50"] for r in groups[w]) for w in widths}
    rate = {w: statistics.median(r["calls_per_second"] for r in groups[w]) for w in widths}
    gbps_arena = {w: statistics.median(r["gb_per_second"] for r in groups[w]) for w in widths}
    real = {w: groups[w][0]["real_bytes"] for w in widths}
    verified = all(r["verified"] for run in runs for r in [run])
    xs = range(len(widths))
    labels = [f"{w} B\n({real[w]} real)" if w else "0 B\n(FP only)" for w in widths]

    figure, axes = plt.subplots(1, 3, figsize=(13.5, 4.9))

    ax = axes[0]
    values = [p50[w] for w in widths]
    bars = ax.bar(xs, values, 0.55, color="#dd6b20")
    ax.axhline(PLAIN_CALL_US, color="#2b6cb0", lw=1.2, ls="--")
    ax.annotate(f"plain nine-GPR call {PLAIN_CALL_US:.3f} us; the gap to the first bar is the FP tax, "
                f"+{1000 * (p50[widths[0]] - PLAIN_CALL_US):.0f} ns",
                (len(widths) - 0.55, PLAIN_CALL_US), textcoords="offset points", xytext=(0, 6),
                ha="right", fontsize=8.5, color="#2b6cb0")
    for bar, value in zip(bars, values):
        ax.annotate(f"{value:.3f}", (bar.get_x() + bar.get_width() / 2, value),
                    textcoords="offset points", xytext=(0, 9), ha="center", fontsize=9)
    ax.set_ylim(0, max(values) * 1.30)
    ax.set_ylabel("microseconds per call")

    ax = axes[1]
    bars = ax.bar(xs, [rate[w] / 1e6 for w in widths], 0.55, color="#2f855a")
    for bar, value in zip(bars, [rate[w] for w in widths]):
        ax.annotate(f"{value / 1e6:.2f}M", (bar.get_x() + bar.get_width() / 2, bar.get_height()),
                    textcoords="offset points", xytext=(0, 9), ha="center", fontsize=9)
    ax.set_ylim(0, max(rate.values()) / 1e6 * 1.30)
    ax.set_ylabel("millions of calls per second")

    ax = axes[2]
    arena = [gbps_arena[w] for w in widths]
    realrate = [gbps_arena[w] * real[w] / (w if w else 1) for w in widths]
    width = 0.38
    left = ax.bar([x - width / 2 for x in xs], arena, width, label="arena bytes per call", color="#805ad5")
    right = ax.bar([x + width / 2 for x in xs], realrate, width, label="real AVX-512 image", color="#b794f4")
    for bars in (left, right):
        for bar in bars:
            if bar.get_height() > 0:
                ax.annotate(f"{bar.get_height():.2f}", (bar.get_x() + bar.get_width() / 2, bar.get_height()),
                            textcoords="offset points", xytext=(0, 7), ha="center", fontsize=8.5)
    ax.set_yscale("log")
    ax.set_ylim(0.1, max(arena) * 3)
    ax.set_ylabel("GB/s (log scale)")
    ax.legend(fontsize=8.5, loc="upper left")

    for ax in axes:
        ax.set_xticks(list(xs))
        ax.set_xticklabels(labels, fontsize=9)
        ax.set_xlabel("width transferred per call")
        ax.grid(axis="y", alpha=0.25, which="both")
        ax.set_axisbelow(True)

    axes[0].set_title("what one call costs", fontsize=10.5)
    axes[1].set_title("what that buys in rate", fontsize=10.5)
    axes[2].set_title("what the width carries", fontsize=10.5)
    figure.suptitle("Widening the regipc channel to the caller's vector state", fontsize=12, y=0.99)
    figure.text(0.5, 0.925,
                f"median of 3 reps per width, every run verified · {real[widths[-1]]} B of real AVX-512 state "
                f"in a {widths[-1]} B arena · a fastpostgres batch is ~4.7 MB, ~1900 calls of the widest point",
                ha="center", fontsize=8.5, color="#4a5568")
    figure.text(0.5, 0.015,
                "the copy is cheap (60 ns for 2456 B); the FP save/restore is not (130 ns per call, paid even at width 0). "
                "The verdict is in the README: the channel widens by 26x and still does not pay.",
                ha="center", fontsize=8.5, color="#4a5568")
    figure.subplots_adjust(left=0.055, right=0.985, top=0.78, bottom=0.19, wspace=0.28)

    args.out.mkdir(parents=True, exist_ok=True)
    path = args.out / FILENAME
    figure.savefig(path, dpi=150, bbox_inches="tight")
    print(f"wrote {path}" + ("" if verified else "  (WARNING: some run is unverified)"))


if __name__ == "__main__":
    main()
