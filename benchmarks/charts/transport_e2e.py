#!/usr/bin/env python3
"""The two transports, end to end, drawn from the record the harness wrote.

    just charts transport-e2e                        benchmarks/results/transport-e2e.log
    just charts transport-e2e --log /tmp/run.log     any other run of the same harness
    just charts transport-e2e --out docs/assets      where the PNG goes

Three panels, one per question the end-to-end record answers, with both engines
side by side per scenario:

  delivered rate  rows/s for the bulk scan, statements/s for the statement
                  scenarios (each scenario's own unit is its tick label)
  one read        the p50 of a single read, with the p95 above it as a whisker
  the client's    microseconds of CPU this process spent per delivered row -
  CPU per row     where the regipc engine's spinning shows up, since it polls
                  the ring's counters instead of sleeping on a semaphore

Every panel is log scale: the scan's rate and a lookup's are a thousand apart, and
a linear axis would draw the small scenarios as nothing. The footnote says what
the numbers are not: the worker's CPU is not in them (one side of the handoff
only), and the payload path - the data segment the batches are published into -
is the same for both engines, so this compares doorbells, not transports of rows.
"""
import argparse
import json
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
DEFAULT_LOG = ROOT / "benchmarks" / "results" / "transport-e2e.log"
FILENAME = "transport-e2e.png"

ENGINES = (
    ("shm", "#2b6cb0", "shm (the ring's semaphores)"),
    ("regipc", "#dd6b20", "regipc (a kernel call through /dev/regipc)"),
)
LABELS = {"scan": "scan\nrows/s", "lookup": "lookup\nstmts/s", "paged": "paged\nstmts/s"}


def parse(text):
    """The `### RESULTS` block: the machine block and one record per cell."""
    marker = "### RESULTS\n"
    if marker not in text:
        sys.exit("transport-e2e chart: the log has no ### RESULTS block -- run "
                 "`just internals transport-e2e` first")
    payload = json.loads(text.split(marker, 1)[1].split("### END RESULTS", 1)[0])
    cells = {}
    for record in payload.get("scenarios", []):
        cells[(record["scenario"], record["engine"])] = record
    if not cells:
        sys.exit("transport-e2e chart: the record has no cells")
    return payload.get("machine", {}), cells


def fmt_rate(v):
    if v >= 1e6:
        return f"{v / 1e6:.2f}M"
    if v >= 1e3:
        return f"{v / 1e3:.1f}k"
    return f"{v:.0f}"


def fmt_ms(v):
    if v >= 100:
        return f"{v:.0f}"
    if v >= 1:
        return f"{v:.1f}"
    return f"{v:.3f}"


def fmt_us(v):
    if v >= 10:
        return f"{v:.1f}"
    return f"{v:.2f}"


def style(ax, log_y=True, yfmt=fmt_rate):
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color("#cccccc")
    ax.tick_params(colors="#666666", length=0)
    ax.margins(y=0.28)
    if log_y:
        ax.set_yscale("log")
        ax.yaxis.set_major_locator(mticker.LogLocator(base=10.0, subs=(1.0, 2.0, 5.0), numticks=15))
        ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _p=None: yfmt(v)))
        ax.yaxis.set_minor_formatter(mticker.NullFormatter())
    ax.grid(axis="y", alpha=0.25, which="both")
    ax.set_axisbelow(True)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log", type=Path, default=DEFAULT_LOG, help="the harness's stdout record")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "assets", help="where to write the PNG")
    args = ap.parse_args()

    if not args.log.exists():
        sys.exit(f"{args.log}: no log -- run "
                 f"`benchmarks/harness/transport_e2e_bench.sh > {DEFAULT_LOG.relative_to(ROOT)}` first")
    text = args.log.read_text()
    machine, cells = parse(text)
    scenarios = [s for s in ("scan", "lookup", "paged") if any(k[0] == s for k in cells)]
    present = [e for e, _c, _l in ENGINES if any(k[1] == e for k in cells)]
    if not scenarios or not present:
        sys.exit(f"{args.log}: no cell to draw")
    status = "MISMATCH" if "MISMATCH" in text or "INVALID" in text else "ok"

    figure, axes = plt.subplots(1, 3, figsize=(14.0, 5.6))
    rate_ax, read_ax, cpu_ax = axes

    # 1. what the application gets, in the scenario's own unit
    width = 0.8 / len(present)
    for index, engine in enumerate(present):
        color = next(c for name, c, _label in ENGINES if name == engine)
        xs, ys = [], []
        for slot, scenario in enumerate(scenarios):
            cell = cells[(scenario, engine)]
            xs.append(slot - 0.4 + (index + 0.5) * width)
            ys.append(max(float(cell["value"]), 1e-9))
        bars = rate_ax.bar(xs, ys, width * 0.92, color=color, zorder=3,
                           label=next(label for name, _c, label in ENGINES if name == engine))
        rate_ax.bar_label(bars, labels=[fmt_rate(v) for v in ys], fontsize=6.5, padding=2)
    for slot, scenario in enumerate(scenarios):
        if len(present) == 2:
            shm = float(cells[(scenario, "shm")]["value"])
            regipc = float(cells[(scenario, "regipc")]["value"])
            if shm:
                # above the group: the bars fill the bottom of a log axis, and a
                # ratio printed inside them would be unreadable
                rate_ax.annotate(f"regipc/shm {regipc / shm:.2f}x", (slot, 0.985),
                                 xycoords=("data", "axes fraction"), ha="center", va="top",
                                 fontsize=7.5, color="#4a5568")
    rate_ax.set_xticks(range(len(scenarios)))
    rate_ax.set_xticklabels([LABELS.get(s, s) for s in scenarios], fontsize=8)
    rate_ax.set_title("what the application gets: delivered rows or statements", fontsize=9.5)
    rate_ax.set_ylabel("per second, as each scenario's tick says (log)", fontsize=9)
    style(rate_ax, yfmt=fmt_rate)

    # 2. one read: the p50, with the p95 above it
    width = 0.8 / (2 * len(present))
    for index, engine in enumerate(present):
        color = next(c for name, c, _label in ENGINES if name == engine)
        for tail, offset in (("p50_ms", 0), ("p95_ms", 1)):
            xs, ys = [], []
            for slot, scenario in enumerate(scenarios):
                cell = cells[(scenario, engine)]
                xs.append(slot - 0.4 + (index + offset * len(present) + 0.5) * width)
                ys.append(max(float(cell[tail]), 1e-4))
            bars = read_ax.bar(xs, ys, width * 0.92, color=color,
                               alpha=1.0 if tail == "p50_ms" else 0.45, zorder=3)
            read_ax.bar_label(bars, labels=[fmt_ms(v) for v in ys], fontsize=6.0, padding=2)
    read_ax.set_xticks(range(len(scenarios)))
    read_ax.set_xticklabels([f"{LABELS.get(s, s).splitlines()[0]}\np50/p95 ms" for s in scenarios], fontsize=8)
    read_ax.set_title("one read: the p50, with the p95 beside it", fontsize=9.5)
    read_ax.set_ylabel("milliseconds (log)", fontsize=9)
    style(read_ax, yfmt=fmt_ms)

    # 3. the client's own CPU per delivered row
    width = 0.8 / len(present)
    for index, engine in enumerate(present):
        color = next(c for name, c, _label in ENGINES if name == engine)
        xs, ys = [], []
        for slot, scenario in enumerate(scenarios):
            cell = cells[(scenario, engine)]
            xs.append(slot - 0.4 + (index + 0.5) * width)
            ys.append(max(float(cell["cpu_us_per_row"]), 1e-4))
        bars = cpu_ax.bar(xs, ys, width * 0.92, color=color, zorder=3)
        cpu_ax.bar_label(bars, labels=[fmt_us(v) for v in ys], fontsize=6.5, padding=2)
    cpu_ax.set_xticks(range(len(scenarios)))
    cpu_ax.set_xticklabels([LABELS.get(s, s).splitlines()[0] for s in scenarios], fontsize=8)
    cpu_ax.set_title("what the client spent: CPU per delivered row", fontsize=9.5)
    cpu_ax.set_ylabel("microseconds of CPU per row (log)", fontsize=9)
    style(cpu_ax, yfmt=fmt_us)

    handles = [plt.Rectangle((0, 0), 1, 1, color=c, label=label) for name, c, label in ENGINES
               if name in present]
    handles.append(plt.Rectangle((0, 0), 1, 1, facecolor="#666666", alpha=0.45,
                                 label="lighter bar: p95 of the same reads"))
    figure.legend(handles, [h.get_label() for h in handles], loc="upper center",
                  bbox_to_anchor=(0.5, 0.905), ncol=3, frameon=False, fontsize=9)
    figure.suptitle("the ring's two doorbells, end to end: a real read through PostgreSQL, "
                    "the worker's batches, the client's rows", fontsize=12, y=0.985)
    machine_line = (f"{machine.get('cpu', 'unknown')} · {machine.get('cores', '?')} cores · "
                    f"postgres {machine.get('postgres', '?')} · {machine.get('python', '?')} · "
                    f"image {machine.get('image', '?')} · git {machine.get('git', '?')}")
    figure.text(0.5, 0.925, machine_line, ha="center", fontsize=8.0, color="#4a5568")
    reps = machine.get("reps", "?")
    read_counts = " / ".join(f"{n}" for n in sorted({int(c["reads"]) for c in cells.values()}))
    footer = (f"median of {reps} processes per bar; timed reads per process: {read_counts} "
              f"(a bar is one statement for the lookups and pages, one 1M-row read for the scan, so "
              f"its p50 = p95).\n"
              f"the same rows on both engines (digest {status}); the payload path is shared, so what "
              f"differs is the doorbell - and the worker's CPU is not in these numbers.")
    figure.text(0.5, 0.012, footer, ha="center", va="bottom", fontsize=7.6, color="#4a5568",
                linespacing=1.5)
    figure.subplots_adjust(left=0.055, right=0.99, top=0.76, bottom=0.19, wspace=0.28)

    args.out.mkdir(parents=True, exist_ok=True)
    path = args.out / FILENAME
    figure.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(figure)
    print(f"wrote {path.relative_to(ROOT) if path.is_relative_to(ROOT) else path}")
    for scenario in scenarios:
        row = []
        for engine in present:
            cell = cells[(scenario, engine)]
            row.append(f"{engine} {fmt_rate(float(cell['value']))} {cell['unit']} "
                       f"(p50 {cell['p50_ms']} ms)")
        print(f"  {scenario:8s} " + "  |  ".join(row))
    if status != "ok":
        sys.exit(f"{args.log}: a cell was invalid or the digests did not match")


if __name__ == "__main__":
    main()
