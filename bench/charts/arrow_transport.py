#!/usr/bin/env python3
"""The Arrow read under the ring's two engines, drawn from the record the harness wrote.

    just charts arrow-transport                        bench/results/arrow-transport.log
    just charts arrow-transport --log /tmp/run.log     any other run of the same harness
    just charts arrow-transport --out docs/assets      where the PNG goes

Three panels, one per question the record answers:

  delivered rows/s   the three families per size, each at its best single-process
                     player: fastpostgres over shm, fastpostgres over regipc, and
                     ADBC + Arrow (columnar read against columnar read, one
                     column summed in place). The record carries every player;
                     the 4-process path is deliberately not a headline - four
                     processes against one would price parallelism, not the read
  vs adbc+arrow      the delivered rows/s ratio of each ring engine against
                     adbc+arrow at that same kind of read; 1.0 is Arrow
  one read, p50      what the engine costs at that read: the median read's wall
                     time, derived from the printed rate (the script prints no
                     per-player times, and the players expose no per-cell CPU,
                     so this is the engine-cost proxy the record carries)

The fastpostgres rows in panels 1 and 3 are the zero-copy player because it is
the like-for-like counterpart of ADBC's arrow+sum - both land the columnar form
and sum one column in place; the DBAPI read is in the record. Panel 2 is log-free
because the ratios sit inside one order of magnitude; panels 1 and 3 are log.

The footnote says what the numbers are not: the script prints a median, not a
percentile, so p50 is derived (one read = rows / rate); the ADBC players do not
depend on the engine and were measured once per size; and the digest of the
record (count + column sums, read by the harness's own probe) is the reason the
three cells really do deliver the same rows.
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
DEFAULT_LOG = ROOT / "bench" / "results" / "arrow-transport.log"
FILENAME = "arrow-transport.png"

ENGINES = (
    ("shm", "#2b6cb0", "fastpostgres, shm"),
    ("regipc", "#dd6b20", "fastpostgres, regipc"),
    ("adbc", "#8a8a8a", "adbc+arrow"),
)
# The headline pool per family: single-process players only, and on the
# fastpostgres side every path a like-for-like read could take.
FPG_SINGLE = ("dbapi", "stream", "core", "sqla")
ADBC_PLAYERS = ("arrow_sum", "fetchall", "to_pylist")
PLAYER_NAMES = {"dbapi": "DBAPI", "stream": "stream()+sum", "core": "core interface",
                "sqla": "SQLAlchemy CORE", "par": "4 processes", "arrow_sum": "arrow+sum",
                "fetchall": "fetchall()", "to_pylist": "table+to_pylist"}


def parse(text):
    """The `### RESULTS` block: the machine block and one record per cell."""
    marker = "### RESULTS\n"
    if marker not in text:
        sys.exit("arrow-transport chart: the log has no ### RESULTS block -- run "
                 "`just rivals arrow-transport` first")
    payload = json.loads(text.split(marker, 1)[1].split("### END RESULTS", 1)[0])
    records = payload.get("records", [])
    if not records:
        sys.exit("arrow-transport chart: the record has no records")
    return payload.get("machine", {}), payload, records


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


def fmt_size(size):
    if size >= 1_000_000:
        return f"{size // 1_000_000}M"
    return f"{size // 1_000}k"


def style(ax, log_y=True, yfmt=fmt_rate):
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color("#cccccc")
    ax.tick_params(colors="#666666", length=0)
    ax.margins(y=0.30)
    if log_y:
        ax.set_yscale("log")
        ax.yaxis.set_major_locator(mticker.LogLocator(base=10.0, subs=(1.0, 2.0, 5.0), numticks=15))
        ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _p=None: yfmt(v)))
        ax.yaxis.set_minor_formatter(mticker.NullFormatter())
    ax.grid(axis="y", alpha=0.25, which="both")
    ax.set_axisbelow(True)


def best(records, size, engine, players):
    """The family's headline for one size: its fastest single-process player."""
    found = [records[(size, engine, key)] for key in players
             if (size, engine, key) in records and records[(size, engine, key)]["value"]]
    return max(found, key=lambda record: record["value"]) if found else None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log", type=Path, default=DEFAULT_LOG, help="the harness's stdout record")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "assets", help="where to write the PNG")
    args = ap.parse_args()

    if not args.log.exists():
        sys.exit(f"{args.log}: no log -- run "
                 f"`bench/harness/arrow_transport_bench.sh > {DEFAULT_LOG.relative_to(ROOT)}` first")
    machine, payload, flat = parse(args.log.read_text())
    records = {(r["size"], r["engine"], r["player"]): r for r in flat}
    sizes = sorted({r["size"] for r in flat})
    present = [name for name, _color, _label in ENGINES
               if any(r["engine"] == name for r in flat)]
    chosen = {}
    for size in sizes:
        for engine in present:
            pool = ADBC_PLAYERS if engine == "adbc" else FPG_SINGLE
            head = best(records, size, engine, pool)
            if head:
                chosen[(size, engine)] = head
    missing = [(size, engine) for size in sizes for engine in present
               if (size, engine) not in chosen]
    if missing:
        sys.exit(f"{args.log}: no headline player for {missing}")
    invalid = [r for r in flat if not r["valid"] or not r.get("digest_ok")]
    status = "ok" if not invalid else "INVALID"

    figure, axes = plt.subplots(1, 3, figsize=(14.0, 5.6))
    rate_ax, ratio_ax, read_ax = axes

    # 1. delivered rows/s, the three families
    width = 0.8 / len(present)
    for index, engine in enumerate(present):
        color = next(c for name, c, _label in ENGINES if name == engine)
        xs, ys = [], []
        for slot, size in enumerate(sizes):
            head = chosen[(size, engine)]
            xs.append(slot - 0.4 + (index + 0.5) * width)
            ys.append(max(float(head["value"]), 1e-9))
        bars = rate_ax.bar(xs, ys, width * 0.92, color=color, zorder=3,
                           label=next(label for name, _c, label in ENGINES if name == engine))
        rate_ax.bar_label(bars, labels=[fmt_rate(v) for v in ys], fontsize=6.5, padding=2)
    rate_ax.set_xticks(range(len(sizes)))
    rate_ax.set_xticklabels([fmt_size(s) for s in sizes], fontsize=8)
    rate_ax.set_title("delivered rows/s: each family's best single-process read", fontsize=9.5)
    rate_ax.set_ylabel("rows/s (log)", fontsize=9)
    style(rate_ax, yfmt=fmt_rate)

    # 2. the ratio of each ring engine against adbc+arrow
    ring = [engine for engine in present if engine != "adbc"]
    width = 0.8 / max(len(ring), 1)
    for index, engine in enumerate(ring):
        color = next(c for name, c, _label in ENGINES if name == engine)
        xs, ys = [], []
        for slot, size in enumerate(sizes):
            if "adbc" not in present:
                continue
            ratio = float(chosen[(size, engine)]["value"]) / float(chosen[(size, "adbc")]["value"])
            xs.append(slot - 0.4 + (index + 0.5) * width)
            ys.append(ratio)
        bars = ratio_ax.bar(xs, ys, width * 0.92, color=color, zorder=3)
        ratio_ax.bar_label(bars, labels=[f"{v:.2f}x" for v in ys], fontsize=6.5, padding=2)
    ratio_ax.axhline(1.0, color="#4a5568", linewidth=1.0, linestyle=(0, (4, 3)), zorder=2)
    ratio_ax.annotate("1.00 = adbc+arrow", (0.985, 1.0), xycoords=("axes fraction", "data"),
                      ha="right", va="bottom", fontsize=7.5, color="#4a5568")
    ratio_ax.set_xticks(range(len(sizes)))
    ratio_ax.set_xticklabels([fmt_size(s) for s in sizes], fontsize=8)
    ratio_ax.set_title("delivered rows/s ratio against adbc+arrow", fontsize=9.5)
    ratio_ax.set_ylabel("ratio (1.0 = Arrow)", fontsize=9)
    style(ratio_ax, log_y=False)

    # 3. one read: the p50 the record derives (what the engine costs there)
    width = 0.8 / len(present)
    for index, engine in enumerate(present):
        color = next(c for name, c, _label in ENGINES if name == engine)
        xs, ys = [], []
        for slot, size in enumerate(sizes):
            head = chosen[(size, engine)]
            xs.append(slot - 0.4 + (index + 0.5) * width)
            ys.append(max(float(head["p50_ms"]), 1e-3))
        bars = read_ax.bar(xs, ys, width * 0.92, color=color, zorder=3)
        read_ax.bar_label(bars, labels=[fmt_ms(v) for v in ys], fontsize=6.5, padding=2)
    read_ax.set_xticks(range(len(sizes)))
    read_ax.set_xticklabels([f"{fmt_size(s)}\nms" for s in sizes], fontsize=8)
    read_ax.set_title("one read: p50, derived from the printed median rate", fontsize=9.5)
    read_ax.set_ylabel("milliseconds (log)", fontsize=9)
    style(read_ax, yfmt=fmt_ms)

    handles = [plt.Rectangle((0, 0), 1, 1, color=color, label=label) for name, color, label in ENGINES
               if name in present]
    figure.legend(handles, [h.get_label() for h in handles], loc="upper center",
                  bbox_to_anchor=(0.5, 0.905), ncol=len(handles), frameon=False, fontsize=9)
    figure.suptitle("the Arrow read under the ring's two engines, and the Arrow ecosystem's own", fontsize=12, y=0.985)
    machine_line = (f"{machine.get('cpu', 'unknown')} · {machine.get('cores', '?')} cores · "
                    f"kernel {machine.get('kernel', '?')} · postgres {machine.get('postgres', '?')} · "
                    f"image {machine.get('image', '?')} · git {machine.get('git', '?')}")
    figure.text(0.5, 0.925, machine_line, ha="center", fontsize=8.0, color="#4a5568")
    heads_fpg = sorted({f"fastpostgres over {e}: {PLAYER_NAMES[h['player']]}"
                       for (size, e), h in chosen.items() if e != "adbc"})
    heads_adbc = sorted({PLAYER_NAMES[h["player"]] for (size, e), h in chosen.items() if e == "adbc"})
    footer = (
        f"each family at its fastest single-process player: {'; '.join(heads_fpg)}; "
        f"adbc+arrow: {', '.join(heads_adbc)} - the same columnar read, one column summed in place.\n"
        "the DBAPI read and the 4-process path are in the record; adbc players do not depend on the "
        "engine: measured once per size and recorded once.\n"
        f"p50 is derived (one read = rows / rate; the script prints medians, not percentiles). "
        f"digest {status}: the same rows in every cell (count + column sums)."
    )
    figure.text(0.5, 0.012, footer, ha="center", va="bottom", fontsize=7.6, color="#4a5568",
                linespacing=1.5)
    figure.subplots_adjust(left=0.055, right=0.99, top=0.76, bottom=0.19, wspace=0.28)

    args.out.mkdir(parents=True, exist_ok=True)
    path = args.out / FILENAME
    figure.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(figure)
    print(f"wrote {path.relative_to(ROOT) if path.is_relative_to(ROOT) else path}")
    for size in sizes:
        row = []
        for engine in present:
            head = chosen[(size, engine)]
            row.append(f"{engine} {fmt_rate(float(head['value']))} rows/s "
                       f"(p50 {fmt_ms(float(head['p50_ms']))} ms)")
        print(f"  {fmt_size(size):>4s}  " + "  |  ".join(row))
    if status != "ok":
        sys.exit(f"{args.log}: a record was invalid or the digests did not match")


if __name__ == "__main__":
    main()
