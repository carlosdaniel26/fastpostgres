#!/usr/bin/env python3
"""regipc against shm, in one figure: the doorbell, the read, and the bill.

    just charts transport-summary
    just charts transport-summary --e2e /tmp/run.log --isolated /tmp/hops.log

It reads the two committed records and nothing else - the isolated experiment
(bench/results/transport-shm-vs-regipc.log: one hop, no payload) and the
end-to-end one (bench/results/transport-e2e.log: a real read through
PostgreSQL) - and draws the three panels that together say where the kernel call
wins and what it charges:

  the doorbell    hops/s and us of CPU per hop, one hop at a time
  the read        the same rows delivered, regipc relative to shm, by scenario
  the bill        the client's CPU per delivered row, the price of the win

The third panel is the honest half: a read whose doorbell is 35% of its time
gets faster by removing it, and the process pays for that in spin CPU.
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

import theme  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
ISOLATED = ROOT / "bench" / "results" / "transport-shm-vs-regipc.log"
E2E = ROOT / "bench" / "results" / "transport-e2e.log"
FILENAME = "transport-summary.png"

COLORS = {"shm": "#2b6cb0", "regipc": "#dd6b20"}
LABELS = {"shm": "shm (semaphores)", "regipc": "regipc (kernel call)"}
SCENARIOS = [("scan", "scan\n1M rows"), ("lookup", "lookup\n1 row"), ("paged", "paged\n100 rows")]


def block(text):
    if "### RESULTS\n" not in text:
        sys.exit("transport-summary: a record has no ### RESULTS block")
    return json.loads(text.split("### RESULTS\n", 1)[1].split("### END RESULTS", 1)[0])


def stamp(text):
    found = re.search(r"^git: (\S+)$", text, re.M)
    return found.group(1) if found else "?"


def isolated_numbers(path):
    payload = block(path.read_text())
    runs = [r for r in payload["runs"] if r.get("verified")]
    out = {}
    for method in ("shm", "regipc"):
        rows = [r for r in runs if r["method"] == method and int(r["count"]) == 1_000_000]
        if not rows:
            sys.exit(f"transport-summary: no verified 1M-hop {method} run in {path}")
        cpu = [(float(r["producer_cpu_s"]) + float(r["consumer_cpu_s"])) / float(r["count"]) * 1e6 for r in rows]
        out[method] = {
            "hops": statistics.median(float(r["hops_per_second"]) for r in rows),
            "p50": statistics.median(float(r["us_per_hop_p50"]) for r in rows),
            "cpu": statistics.median(cpu),
        }
    return out, stamp(path.read_text())


def e2e_numbers(path):
    payload = block(path.read_text())
    records = [r for r in payload["scenarios"] if r.get("valid", True)]
    out = {}
    for scenario, _ in SCENARIOS:
        for engine in ("shm", "regipc"):
            rows = [r for r in records if r["scenario"] == scenario and r["engine"] == engine]
            if not rows:
                sys.exit(f"transport-summary: no {engine} record for {scenario} in {path}")
            out[(scenario, engine)] = {
                "value": statistics.median(float(r["value"]) for r in rows),
                "p50": statistics.median(float(r["p50_ms"]) for r in rows),
                "cpu": statistics.median(float(r["cpu_us_per_row"]) for r in rows),
                "unit": rows[0]["unit"],
            }
    return out, stamp(path.read_text())


def main():
    ap = argparse.ArgumentParser(description="regipc against shm, from the two records")
    ap.add_argument("--isolated", type=Path, default=ISOLATED)
    ap.add_argument("--e2e", type=Path, default=E2E)
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "assets")
    args = ap.parse_args()

    hops, hop_stamp = isolated_numbers(args.isolated)
    read, e2e_stamp = e2e_numbers(args.e2e)

    figure, axes = plt.subplots(1, 3, figsize=(14.5, 5.0))

    # 1: the doorbell itself
    ax = axes[0]
    methods = ["shm", "regipc"]
    bars = ax.bar([0, 1], [hops[m]["hops"] for m in methods], 0.55, color=[COLORS[m] for m in methods])
    for bar, method in zip(bars, methods):
        stats = hops[method]
        ax.annotate(f"{stats['hops'] / 1e6:.2f}M hops/s\n{stats['p50']:.2f} us  ·  {stats['cpu']:.2f} us CPU",
                    (bar.get_x() + bar.get_width() / 2, bar.get_height()),
                    textcoords="offset points", xytext=(0, 9), ha="center", fontsize=9)
    ax.set_yscale("log")
    ax.set_ylim(hops["shm"]["hops"] / 4, hops["regipc"]["hops"] * 4)
    ax.set_xticks([0, 1])
    ax.set_xticklabels([LABELS[m] for m in methods], fontsize=9)
    ax.set_ylabel("hops per second (log scale)")
    ax.set_title(f"the doorbell: {hops['regipc']['hops'] / hops['shm']['hops']:.1f}x more hops per second",
                 fontsize=10.5)
    theme.declutter(ax)

    # 2: the read, regipc relative to shm
    ax = axes[1]
    ratios, latency = [], []
    for scenario, _ in SCENARIOS:
        shm, regipc = read[(scenario, "shm")], read[(scenario, "regipc")]
        ratios.append(regipc["value"] / shm["value"])
        latency.append(shm["p50"] / regipc["p50"])
    xs = list(range(len(SCENARIOS)))
    width = 0.38
    left = ax.bar([x - width / 2 for x in xs], ratios, width, label="delivered per second", color="#2f855a")
    right = ax.bar([x + width / 2 for x in xs], latency, width, label="p50 per read", color="#9ae6b4")
    for bars in (left, right):
        for bar in bars:
            ax.annotate(f"{bar.get_height():.2f}x", (bar.get_x() + bar.get_width() / 2, bar.get_height()),
                        textcoords="offset points", xytext=(0, 7), ha="center", fontsize=9)
    ax.axhline(1.0, color="#4a5568", lw=1.0, ls="--")
    ax.annotate("shm", (len(SCENARIOS) - 0.5, 1.0), textcoords="offset points", xytext=(0, -13),
                ha="right", fontsize=8.5, color="#4a5568")
    ax.set_ylim(0, max(ratios + latency) * 1.30)
    ax.set_xticks(xs)
    ax.set_xticklabels([label for _, label in SCENARIOS], fontsize=9)
    ax.set_ylabel("regipc relative to shm")
    ax.set_title("the read: the same rows, delivered", fontsize=10.5)
    theme.legend(ax, loc="upper left", fontsize=8.5)

    # 3: the bill
    ax = axes[2]
    for index, engine in enumerate(methods):
        values = [read[(scenario, engine)]["cpu"] for scenario, _ in SCENARIOS]
        bars = ax.bar([x + (index - 0.5) * width for x in xs], values, width * 0.9,
                      label=LABELS[engine], color=COLORS[engine])
        for bar in bars:
            ax.annotate(f"{bar.get_height():.2f}", (bar.get_x() + bar.get_width() / 2, bar.get_height()),
                        textcoords="offset points", xytext=(0, 6), ha="center", fontsize=8.5)
    ax.set_yscale("log")
    ax.set_ylim(0.1, 500)
    ax.set_xticks(xs)
    ax.set_xticklabels([label for _, label in SCENARIOS], fontsize=9)
    ax.set_ylabel("client CPU per delivered row, us (log scale)")
    ax.set_title("the bill: what the win costs in CPU", fontsize=10.5)
    theme.legend(ax, loc="upper right", fontsize=8.5)

    for ax in axes:
        theme.declutter(ax)

    figure.suptitle("regipc against shm: the doorbell is 12x cheaper, the read is not", fontsize=12, y=0.99)
    theme.footer(figure,
                 f"isolated record {hop_stamp} (1M hops) · end-to-end record {e2e_stamp} (3 processes per cell, "
                 f"the engine proven per read) · both engines deliver the same digest",
                 y=0.915)
    theme.footer(figure,
                 "bulk: 13 hops in 400 ms, so the doorbell is noise (0.99x). small reads: 4 hops per read, ~35% of the "
                 "time, so removing it lands 1.3-1.55x - and the client pays for it in spin CPU, which is the trade.",
                 y=0.015)
    figure.subplots_adjust(left=0.055, right=0.985, top=0.78, bottom=0.19, wspace=0.28)

    args.out.mkdir(parents=True, exist_ok=True)
    path = args.out / FILENAME
    figure.savefig(path, dpi=150, bbox_inches="tight")
    print(f"wrote {path}")

    print(f"  doorbell   {hops['shm']['hops'] / 1e3:7.1f}k vs {hops['regipc']['hops'] / 1e6:5.2f}M hops/s  "
          f"({hops['regipc']['hops'] / hops['shm']['hops']:.2f}x)")
    for scenario, _ in SCENARIOS:
        shm, regipc = read[(scenario, "shm")], read[(scenario, "regipc")]
        print(f"  {scenario:<10} {shm['value']:9.1f} vs {regipc['value']:9.1f} {shm['unit']:<8} "
              f"({regipc['value'] / shm['value']:.2f}x)   p50 {shm['p50']:.3f} vs {regipc['p50']:.3f} ms   "
              f"CPU/row {shm['cpu']:.3f} vs {regipc['cpu']:.3f} us")


if __name__ == "__main__":
    main()
