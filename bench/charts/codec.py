#!/usr/bin/env python3
"""Charts for the codec + transport isolation matrix.

Reads the log the matrix runner writes (`bench/results/codec-transport.log` by default)
and renders four figures:

    codec-format.png            encode + decode time and peak RSS per format
    codec-footprint.png         memory held on top of the payload, and every
                                named copy stage per format
    transport.png               throughput and p50/p99 latency per transport
    codec-who-pays.png          CPU and memory per side, and the whole
                                min->p50->p99->max spread, per transport

Same palette as `bench/charts/adbc.py`; the type and the layout come from
`bench/charts/theme.py`: the accent marks this
repository's mechanism (`fpcolumn`, shared memory), the greys the rival (Arrow
IPC, UNIX socket). Every bar carries its number, and the footer carries the
machine, rows, chunk size and commit the log was produced from.

Usage:
    bench/charts/codec.py                          # bench/results/codec-transport.log
    bench/charts/codec.py --log build/quick.log    # any other run
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
    from matplotlib.ticker import FuncFormatter, NullFormatter
except ImportError:
    sys.exit("matplotlib is required (pip install matplotlib)")

import theme  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "docs" / "assets"

# Strategic colour, as in bench/charts/adbc.py: mute the baseline, spend the accent on
# the thing this repository owns.
ACCENT = "#1f77b4"
GREY = ("#c8c8c8", "#8a8a8a")
FADED = 0.42

_RUN = re.compile(r"^# run: (\S+) payload over (\S+) \(chunk=")
_META = re.compile(r"^(mode|rows|reps|chunk|cpu|mem|git): (.+)$")

FORMAT_LABELS = {"fpcolumn": "fpcolumn", "arrow-ipc": "Arrow IPC"}
TRANSPORT_LABELS = {"shm": "shared memory", "uds": "UNIX socket"}


# ---- parsing --------------------------------------------------------------


def parse(text):
    """(meta, cells, transport runs) from the runner's log."""
    meta, cells, runs = {}, [], []
    for line in text.splitlines():
        if m := _META.match(line):
            meta[m.group(1)] = m.group(2)
        elif m := _RUN.match(line):
            runs.append((m.group(1), m.group(2)))
        elif line.strip().startswith("{"):
            cells.append(json.loads(line))
    return meta, cells, runs


def transport_pairs(cells, runs):
    """[(payload, transport, cell)] in the log's order.

    The transport cells move opaque bytes and their JSON does not say which
    payload they carried — the runner's `# run:` labels are where that survives,
    so pair on them and fall back to the payload sizes if the labels are missing.
    """
    transports = [c for c in cells if c["cell"].startswith("transport_")]
    if len(runs) == len(transports):
        return [(p, t, cell) for (p, t), cell in zip(runs, transports)]
    by_size = {c["payload_bytes"]: c["cell"] for c in cells
               if not c["cell"].startswith("transport_")}
    return [(by_size.get(c["payload_bytes"], "?"), c["cell"].removeprefix("transport_"), c)
            for c in transports]


# ---- drawing helpers ------------------------------------------------------


def footer_text(meta, note):
    chunk = re.match(r"(\d+)", meta.get("chunk", ""))
    parts = [meta.get("cpu", ""), meta.get("mem", ""), f"{meta.get('rows', '?')} rows",
             f"{int(chunk.group(1)) // 1024} KiB chunks" if chunk else "",
             f"git {meta.get('git', '?')}"]
    if note:
        parts.append("")
    line = " · ".join(p for p in parts if p)
    if note:
        # the semantics on their own line: appended to the meta line it ran off
        # both edges of the canvas at 150 dpi
        line += "\n" + note
    return line


def save(fig, path):
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"wrote {path}")


# ---- Figure 1: format cost ------------------------------------------------


def plot_format(formats, meta):
    if not formats:
        sys.exit("no format cells in the log (fpcolumn / arrow-ipc)")
    fig, (ax_t, ax_m) = plt.subplots(
        1, 2, figsize=(9.6, 4.9), gridspec_kw={"width_ratios": [1.45, 1.0]}
    )
    names = [c["cell"] for c in formats]
    labels = [FORMAT_LABELS.get(n, n) for n in names]
    colors = [ACCENT if n == "fpcolumn" else GREY[1] for n in names]
    xs = list(range(len(names)))
    enc = [c["encode_ms"] for c in formats]
    dec = [c["decode_ms"] for c in formats]

    ax_t.bar(xs, enc, width=0.30, color=colors, zorder=3)
    ax_t.bar(xs, dec, width=0.30, bottom=enc, color=colors, alpha=FADED, zorder=3)
    for x, e, d in zip(xs, enc, dec):
        # beside the bar, not inside it: a bar this slim has no room for text
        ax_t.text(x + 0.17, e / 2, f"encode {e:,.1f}", ha="left", va="center", fontsize=7.4)
        ax_t.text(x + 0.17, e + d / 2, f"decode {d:,.1f}", ha="left", va="center", fontsize=7.4)
        ax_t.text(x, e + d, f"{e + d:,.1f} ms", ha="center", va="bottom", fontsize=8.5)
    ax_t.set_xticks(xs)
    ax_t.set_xticklabels(labels)
    ax_t.set_ylabel("ms per 1M rows")
    ax_t.set_title("encode + decode, transport held out (lower is better)", fontsize=9.5)
    ax_t.set_ylim(0, max(e + d for e, d in zip(enc, dec)) * 1.20)
    ax_t.set_xlim(-0.9, len(xs) - 0.1)
    theme.declutter(ax_t)
    theme.ticks(ax_t)

    rss = [c["rss_kb"] / 1024 for c in formats]
    ax_m.bar(xs, rss, width=0.36, color=colors, zorder=3)
    for x, c, value in zip(xs, formats, rss):
        ax_m.text(x, value, f"{value:,.0f} MB\ncopies {c['copies']}", ha="center",
                  va="bottom", fontsize=8, linespacing=1.25)
    ax_m.set_xticks(xs)
    ax_m.set_xticklabels(labels, fontsize=8.5)
    ax_m.set_ylabel("peak RSS, MB")
    ax_m.set_title("memory and copies", fontsize=9.5)
    ax_m.set_ylim(0, max(rss) * 1.30)
    ax_m.set_xlim(-0.9, len(xs) - 0.1)
    theme.declutter(ax_m)
    theme.ticks(ax_m)

    fig.suptitle("Format cost: the same rows, encoded and decoded", fontsize=12.5, y=0.975)
    theme.footer(fig, footer_text(meta, "one process, no transport"))
    fig.subplots_adjust(left=0.09, right=0.97, top=0.84, bottom=0.14, wspace=0.28)
    return fig


# ---- Figure 2: transport cost ---------------------------------------------


def plot_transport(pairs, meta):
    if not pairs:
        sys.exit("no transport cells in the log (transport_shm / transport_uds)")
    payloads = list(dict.fromkeys(p for p, _t, _c in pairs))
    transports = list(dict.fromkeys(t for _p, t, _c in pairs))

    def cell(payload, transport):
        return next(c for p, t, c in pairs if p == payload and t == transport)

    fig, (ax_r, ax_l) = plt.subplots(
        1, 2, figsize=(10.4, 4.9), gridspec_kw={"width_ratios": [1.12, 1.35]}
    )

    # throughput: one group per payload, one bar per transport
    xs = list(range(len(payloads)))
    width = 0.38
    cpu = {t: [cell(p, t).get("cpu_user_ms", 0) + cell(p, t).get("cpu_sys_ms", 0)
               for p in payloads] for t in transports}
    step = width * 1.12  # a sliver between the bars of a group
    for i, transport in enumerate(transports):
        values = [cell(p, transport)["gb_per_s"] for p in payloads]
        pos = [x + (i - (len(transports) - 1) / 2) * step for x in xs]
        color = ACCENT if transport == "shm" else GREY[1]
        ax_r.bar(pos, values, width=width, color=color, zorder=3,
                 label=TRANSPORT_LABELS.get(transport, transport))
        for px, value in zip(pos, values):
            ax_r.text(px, value, f"{value:.2f} GB/s", ha="center", va="bottom", fontsize=8)
    ax_r.set_xticks(xs)
    ax_r.set_xticklabels([FORMAT_LABELS.get(p, p) + "\npayload" for p in payloads])
    ax_r.set_ylabel("GB/s")
    # CPU goes in the title: a per-bar third line collided with the neighbouring bar
    cpu_note = " · ".join(
        f"{TRANSPORT_LABELS.get(t, t).lower()} {min(v):.1f}–{max(v):.1f} ms" for t, v in cpu.items()
    )
    ax_r.set_title(
        "throughput, same bytes, codec out of the loop\n"
        f"CPU, both sides: {cpu_note}",
        fontsize=9.5,
    )
    ax_r.set_ylim(0, max(cell(p, t)["gb_per_s"] for p in payloads for t in transports) * 1.18)
    ax_r.set_xlim(-0.8, len(payloads) - 0.2)
    theme.legend(ax_r, loc="upper right", fontsize=8)
    theme.declutter(ax_r)
    theme.ticks(ax_r)

    # latency: two bars per cell (p50 solid, p99 faded), log axis - the range is
    # ~100x, and on a bar chart the two bars are read against the same baseline
    rows = [(p, t) for p in payloads for t in transports]
    xs = list(range(len(rows)))
    width = 0.36
    step = width * 1.12  # a sliver between the p50 and p99 bars
    for i, (key, alpha) in enumerate((("p50_ms", 1.0), ("p99_ms", 0.45))):
        values = [cell(p, t)[key] * 1e3 for p, t in rows]
        pos = [x + (i - 0.5) * step for x in xs]
        colors = [ACCENT if t == "shm" else GREY[1] for _p, t in rows]
        drawn = ax_l.bar(pos, values, width=width, color=colors, alpha=alpha, zorder=3)
        ax_l.bar_label(drawn, labels=[f"{v:,.0f}" for v in values], padding=2, fontsize=7.4)
    ax_l.set_xticks(xs)
    ax_l.set_xticklabels(
        [f"{FORMAT_LABELS.get(p, p)}\n{TRANSPORT_LABELS.get(t, t)}" for p, t in rows],
        fontsize=7.6, linespacing=1.3,
    )
    lows = min(cell(p, t)["p50_ms"] for p, t in rows) * 1e3
    highs = max(cell(p, t)["p99_ms"] for p, t in rows) * 1e3
    ax_l.set_yscale("log")
    ax_l.set_ylim(lows * 0.42, highs * 2.4)
    ax_l.yaxis.set_major_formatter(FuncFormatter(lambda v, _p: f"{v:,.0f}"))
    ax_l.yaxis.set_minor_formatter(NullFormatter())
    ax_l.set_ylabel("per-chunk latency, μs (log)")
    ax_l.set_title("latency: p50 solid, p99 faded", fontsize=9.5)
    ax_l.margins(y=0.24)
    theme.declutter(ax_l)
    theme.ticks(ax_l, log=True)

    fig.suptitle("Transport cost: identical payload bytes, producer → consumer", fontsize=12.5, y=0.975)
    theme.footer(fig, footer_text(meta, "one chunk in flight"))
    fig.subplots_adjust(left=0.235, right=0.975, top=0.84, bottom=0.21, wspace=0.42)
    return fig


# ---- Figure 3: footprint and copies ---------------------------------------


def plot_footprint(formats, meta):
    """What each approach holds in memory, and where its copies happen."""
    if not formats:
        sys.exit("no format cells in the log (fpcolumn / arrow-ipc)")
    fig, (ax_m, ax_c) = plt.subplots(
        1, 2, figsize=(10.4, 5.0), gridspec_kw={"width_ratios": [1.0, 1.45]}
    )
    labels = [FORMAT_LABELS.get(c["cell"], c["cell"]) for c in formats]
    colors = [ACCENT if c["cell"] == "fpcolumn" else GREY[1] for c in formats]
    xs = list(range(len(formats)))

    payload = [c["payload_bytes"] / 2**20 for c in formats]
    rss = [c["rss_kb"] / 1024 for c in formats]
    extra = [max(r - p, 0.0) for r, p in zip(rss, payload)]
    ax_m.bar(xs, payload, width=0.38, color=colors, zorder=3)
    ax_m.bar(xs, extra, width=0.38, bottom=payload, color=colors, alpha=FADED, zorder=3)
    for x, p, r in zip(xs, payload, rss):
        ax_m.text(x, r, f"{r:,.0f} MB\n{r / p:.1f}× the payload", ha="center",
                  va="bottom", fontsize=8, linespacing=1.25)
    ax_m.set_xticks(xs)
    ax_m.set_xticklabels(labels)
    ax_m.set_ylabel("peak RSS, MB")
    ax_m.set_title("solid = payload, faded = held on top of it", fontsize=9.5)
    ax_m.set_ylim(0, max(rss) * 1.30)
    ax_m.set_xlim(-0.9, len(xs) - 0.1)
    theme.declutter(ax_m)
    theme.ticks(ax_m)

    # copies by stage: one row per (cell, stage), zero counts annotated
    rows = [(c["cell"], stage, n) for c in formats
            for stage, n in (c.get("copy_stages") or {}).items()]
    ys = list(range(len(rows)))
    for y, (cell, _stage, n) in zip(ys, rows):
        color = ACCENT if cell == "fpcolumn" else GREY[1]
        ax_c.barh([y], [n], height=0.55, color=color, zorder=3)
        ax_c.text(n + 0.06, y, f"{n}" if n else "0 — aliased, not copied",
                  va="center", fontsize=7.4, color="#333333")
    ax_c.set_yticks(ys)
    ax_c.set_yticklabels([f"{FORMAT_LABELS.get(c, c)} · {stage}" for c, stage, _n in rows],
                         fontsize=7.4)
    ax_c.invert_yaxis()
    ax_c.set_xlabel("copies of the payload per run")
    ax_c.set_xlim(0, max((n for _c, _s, n in rows), default=1) + 0.9)
    ax_c.set_title("where the copies happen", fontsize=9.5)
    theme.declutter(ax_c)
    theme.ticks(ax_c, axis="x")

    fig.suptitle("Footprint of each approach: memory and copies", fontsize=12.5, y=0.975)
    theme.footer(fig, footer_text(meta, "one process, no transport"))
    fig.subplots_adjust(left=0.10, right=0.98, top=0.84, bottom=0.20, wspace=0.55)
    return fig


# ---- Figure 4: who pays on the transport ----------------------------------


def plot_who_pays(pairs, meta):
    """CPU per side, memory per side, and the whole latency spread."""
    if not pairs:
        sys.exit("no transport cells in the log (transport_shm / transport_uds)")
    fig, (ax_cpu, ax_mem, ax_lat) = plt.subplots(
        1, 3, figsize=(11.6, 5.0), gridspec_kw={"width_ratios": [1.0, 0.95, 1.5]}
    )
    rows = [(p, t) for p in dict.fromkeys(p for p, _t, _c in pairs)
            for t in dict.fromkeys(t for _p, t, _c in pairs)]
    ys = list(range(len(rows)))
    cell = {(p, t): c for p, t, c in pairs}
    colors = [ACCENT if t == "shm" else GREY[1] for _p, t in rows]
    # short row names: the full ones pushed into the neighbouring panel
    names = [f"{FORMAT_LABELS.get(p, p)} · {t}" for p, t in rows]

    # CPU: stacked producer + consumer
    prod = [cell[r].get("producer_cpu_ms", 0.0) for r in rows]
    cons = [cell[r].get("consumer_cpu_ms", 0.0) for r in rows]
    ax_cpu.barh(ys, prod, height=0.55, color=colors, zorder=3)
    ax_cpu.barh(ys, cons, height=0.55, left=prod, color=colors, alpha=FADED, zorder=3)
    for y, p, c in zip(ys, prod, cons):
        ax_cpu.text(p + c, y, f"{p + c:.1f} ms", va="center", fontsize=7.6)
    ax_cpu.set_yticks(ys)
    ax_cpu.set_yticklabels(names, fontsize=7.8)
    ax_cpu.invert_yaxis()
    ax_cpu.set_xlabel("producer + consumer")
    ax_cpu.set_xlim(0, max(p + c for p, c in zip(prod, cons)) * 1.30)
    ax_cpu.set_title("CPU, ms", fontsize=9.5)
    theme.declutter(ax_cpu)
    theme.ticks(ax_cpu, axis="x")

    # memory per side
    p_rss = [cell[r].get("producer_rss_kb", 0) / 1024 for r in rows]
    c_rss = [cell[r].get("consumer_rss_kb", 0) / 1024 for r in rows]
    ax_mem.barh([y - 0.21 for y in ys], p_rss, height=0.40, color=colors, zorder=3)
    ax_mem.barh([y + 0.21 for y in ys], c_rss, height=0.40, color=colors, alpha=FADED, zorder=3)
    for y, p, c in zip(ys, p_rss, c_rss):
        ax_mem.text(max(p, c), y, f"{p:,.0f} / {c:,.0f} MB", va="center", fontsize=7.6)
    ax_mem.set_yticks(ys)
    ax_mem.set_yticklabels(names, fontsize=7.8)
    ax_mem.invert_yaxis()
    ax_mem.set_xlabel("producer / consumer")
    ax_mem.set_xlim(0, max(max(p, c) for p, c in zip(p_rss, c_rss)) * 1.55)
    ax_mem.set_title("peak RSS, MB", fontsize=9.5)
    theme.declutter(ax_mem)
    theme.ticks(ax_mem, axis="x")

    # latency: p50 and p99 per cell, horizontal bars like the panels above
    p50 = [cell[r]["p50_ms"] * 1e3 for r in rows]
    p99 = [cell[r]["p99_ms"] * 1e3 for r in rows]
    ax_lat.barh([y - 0.21 for y in ys], p50, height=0.40, color=colors, zorder=3)
    ax_lat.barh([y + 0.21 for y in ys], p99, height=0.40, color=colors, alpha=FADED, zorder=3)
    for y, median, tail in zip(ys, p50, p99):
        ax_lat.text(median * 1.08, y - 0.18, f"{median:,.0f}", va="center", fontsize=7.4)
        ax_lat.text(tail * 1.08, y + 0.18, f"{tail:,.0f}", va="center", fontsize=7.4)
    ax_lat.set_yticks(ys)
    ax_lat.set_yticklabels(names, fontsize=7.8)
    ax_lat.invert_yaxis()
    ax_lat.set_xscale("log")
    lo_all = min(p50) * 0.45
    hi_all = max(p99) * 2.2
    ax_lat.set_xlim(lo_all, hi_all)
    ax_lat.set_xticks([t for t in (1, 3, 10, 30, 100, 300, 1000) if lo_all <= t <= hi_all])
    ax_lat.xaxis.set_major_formatter(FuncFormatter(lambda v, _p: f"{v:,.0f}"))
    ax_lat.xaxis.set_minor_formatter(NullFormatter())
    ax_lat.set_xlabel("p50 / p99, μs")
    ax_lat.set_title("per-chunk latency, μs (log)", fontsize=9.5)
    theme.declutter(ax_lat)
    theme.ticks(ax_lat, axis="x", log=True)

    fig.suptitle("Who pays: CPU, memory and latency per side", fontsize=12.5, y=0.975)
    theme.footer(fig, footer_text(
        meta, "solid = producer / p50, faded = consumer / p99 · "
              "shm = shared memory, uds = UNIX socket"))
    fig.subplots_adjust(left=0.115, right=0.975, top=0.83, bottom=0.16, wspace=0.85)
    return fig


# ---- main -----------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--log", default="bench/results/codec-transport.log",
                    help="the matrix log to plot (default: %(default)s)")
    args = ap.parse_args()

    path = Path(args.log)
    if not path.is_file():
        sys.exit(f"no log at {path} — run `just codec-transport` first")
    meta, cells, runs = parse(path.read_text())
    if meta.get("mode", "").startswith("--quick"):
        print("note: this log is a --quick smoke run, not a measurement", file=sys.stderr)

    formats = [c for c in cells if c["cell"] in FORMAT_LABELS]
    pairs = transport_pairs(cells, runs)

    print(f"{'format':<12} {'payload':>10} {'rows/s':>10} {'encode ms':>10} "
          f"{'decode ms':>10} {'copies':>7} {'RSS MB':>8}")
    for c in formats:
        print(f"{c['cell']:<12} {c['payload_bytes'] / 2**20:>9.1f}M {c['rows_per_s'] / 1e6:>9.2f}M "
              f"{c['encode_ms']:>10.1f} {c['decode_ms']:>10.1f} {c['copies']:>7} "
              f"{c['rss_kb'] / 1024:>8.0f}")
    print()
    print(f"{'payload + transport':<28} {'GB/s':>7} {'p50 μs':>8} {'p99 μs':>8} "
          f"{'CPU ms':>7} {'copies':>7}")
    for payload, transport, c in pairs:
        cpu = c.get("cpu_user_ms", 0) + c.get("cpu_sys_ms", 0)
        print(f"{FORMAT_LABELS.get(payload, payload) + ' + ' + TRANSPORT_LABELS.get(transport, transport):<28} "
              f"{c['gb_per_s']:>7.2f} {c['p50_ms'] * 1e3:>8.1f} {c['p99_ms'] * 1e3:>8.1f} "
              f"{cpu:>7.1f} {c['copies']:>7}")

    save(plot_format(formats, meta), OUT / "codec-format.png")
    save(plot_footprint(formats, meta), OUT / "codec-footprint.png")
    save(plot_transport(pairs, meta), OUT / "transport.png")
    save(plot_who_pays(pairs, meta), OUT / "codec-who-pays.png")


if __name__ == "__main__":
    main()
