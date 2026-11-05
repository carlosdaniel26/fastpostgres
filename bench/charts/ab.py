#!/usr/bin/env python3
"""Before/after charts for the improvements, from the logs the runs leave behind.

Reads pairs of logs and renders three figures:

    improve-codec.png      the columnar cell before/after, the Arrow control, and
                           the non-temporal-store A/B
    improve-system.png     query-level rows/s and the client's cycles/row
    improve-transport.png  throughput and p50 before/after, and the in-flight curve

Every bar carries its number, the control is drawn in the rival's grey so it is
obvious when it did not move, and the footer names the machine, the commit and
the logs. Same palette as `bench/charts/adbc.py`; the type and the layout come
from `bench/charts/theme.py`.

Usage:
    bench/charts/ab.py                     # the docs/improve-*.log defaults
    bench/charts/ab.py --nt-log x.log --inflight-log y.log
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
    from matplotlib.ticker import NullFormatter
except ImportError:
    sys.exit("matplotlib is required (pip install matplotlib)")

import theme  # noqa: E402

OUT = Path.cwd()

ACCENT = "#1f77b4"
ACCENT_AFTER = "#0d4f7c"
GREY = ("#c8c8c8", "#8a8a8a")
FADED = 0.42

FORMAT_LABELS = {"fpcolumn": "fpcolumn", "arrow-ipc": "Arrow IPC"}
TRANSPORT_LABELS = {"shm": "shared memory", "uds": "UNIX socket"}
BEFORE, AFTER = "before", "after"


def read_json_lines(path):
    """Every JSON object in a log, in order (non-JSON lines are ignored)."""
    cells = []
    for line in Path(path).read_text(errors="replace").splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            cells.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return cells


def cell_map(path):
    """name -> last cell with that name (the runner prints one line per cell)."""
    return {c["cell"]: c for c in read_json_lines(path) if "cell" in c}


def bench_rows(path):
    """The `ZERO-COPY READ` 1M numbers from a query-level log.

    Parsed inside its own block: a regex spanning the whole file would pick up
    the first section's numbers for whichever section it is looking for.
    """
    text = Path(path).read_text(errors="replace")
    out = {}
    start = text.find("ZERO-COPY READ")
    if start >= 0:
        m = re.search(r"^\s+1M\s+([\d.]+)M\s+([\d.]+)M", text[start:start + 600], re.M)
        if m:
            out["zerocopy"] = (float(m.group(1)), float(m.group(2)))
    return out


def decode_cycles(path):
    m = re.search(r"read_rows:\s*([\d.]+) cycles/row", Path(path).read_text(errors="replace"))
    return float(m.group(1)) if m else None


def meta(path):
    """cpu / rows / git from a log, for the footer."""
    text = Path(path).read_text(errors="replace")
    out = {}
    for key in ("cpu", "mem", "rows", "git"):
        m = re.search(rf"^{key}: (.*)$", text, re.M)
        if m:
            out[key] = m.group(1).strip()
    return out


def save(fig, path):
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"wrote {path}")


# ---- Figure 1: the columnar cell ----------------------------------------------------------


def plot_codec(before, after, nt, meta_text):
    fig, (ax_t, ax_nt) = plt.subplots(
        1, 2, figsize=(11.0, 4.9), gridspec_kw={"width_ratios": [1.2, 1.0]}
    )
    names = [n for n in ("fpcolumn", "arrow-ipc") if n in before and n in after]
    xs = list(range(len(names)))

    for x, name in zip(xs, names):
        enc_b, dec_b = before[name]["encode_ms"], before[name]["decode_ms"]
        enc_a, dec_a = after[name]["encode_ms"], after[name]["decode_ms"]
        moved = name == "fpcolumn"
        for j, (enc, dec) in enumerate(((enc_b, dec_b), (enc_a, dec_a))):
            off = (j - 0.5) * 0.36
            color = (ACCENT if j == 0 else ACCENT_AFTER) if moved else GREY[0]
            ax_t.bar(x + off, enc, width=0.33, color=color, zorder=3)
            ax_t.bar(x + off, dec, width=0.33, bottom=enc, color=color, alpha=FADED, zorder=3)
            ax_t.text(x + off, enc + dec, f"{enc + dec:.1f}", ha="center", va="bottom",
                      fontsize=7.6)
    # the deltas belong in the title: under the bars they collided with the ticks
    deltas = ", ".join(
        f"{FORMAT_LABELS.get(n, n)} {100 * (after[n]['encode_ms'] + after[n]['decode_ms'])
                                  / (before[n]['encode_ms'] + before[n]['decode_ms']):.0f}%"
        for n in names
    )
    ax_t.set_xticks(xs)
    ax_t.set_xticklabels([FORMAT_LABELS.get(n, n) for n in names])
    ax_t.set_ylabel("ms per 1M rows")
    ax_t.set_title(f"encode + decode at the defaults: {deltas}", fontsize=9.5)
    ax_t.set_ylim(0, max(c["encode_ms"] + c["decode_ms"]
                         for c in list(before.values()) + list(after.values())) * 1.20)
    ax_t.margins(x=0.2)
    theme.declutter(ax_t)

    # the non-temporal-store A/B, if it ran
    if nt:
        plain = [c for c in nt if not c.get("nt_store")]
        stream = [c for c in nt if c.get("nt_store")]
        groups = [("plain stores", plain, GREY[1]), ("non-temporal", stream, ACCENT_AFTER)]
        for i, (label, cells, color) in enumerate(groups):
            if not cells:
                continue
            encs = sorted(c["encode_ms"] for c in cells)
            med = encs[len(encs) // 2]
            ax_nt.bar(i, med, width=0.5, color=color, zorder=3)
            ax_nt.text(i, med, f"{med:.2f} ms encode\n{med / cells[0]['rows'] * 1e6:.1f} ns/row",
                       ha="center", va="bottom", fontsize=8, linespacing=1.3)
        if plain and stream:
            base = sorted(c["encode_ms"] for c in plain)[len(plain) // 2]
            new = sorted(c["encode_ms"] for c in stream)[len(stream) // 2]
            dec_plain = sorted(c["decode_ms"] for c in plain)[len(plain) // 2]
            dec_new = sorted(c["decode_ms"] for c in stream)[len(stream) // 2]
            ax_nt.set_title(
                f"non-temporal stores: encode {100 * new / base:.0f}%, "
                f"decode {100 * dec_new / dec_plain:.0f}%",
                fontsize=9.5,
            )
        ax_nt.set_xticks([0, 1])
        ax_nt.set_xticklabels([g[0] for g in groups])
        ax_nt.set_ylabel("encode, ms per 1M rows")
        ax_nt.set_ylim(0, max(c["encode_ms"] for c in nt) * 1.35)
        ax_nt.margins(x=0.35)
        theme.declutter(ax_nt)
    else:
        ax_nt.axis("off")
        ax_nt.text(0.5, 0.5, "no non-temporal-store log", ha="center", va="center",
                   fontsize=9, color="#888888", transform=ax_nt.transAxes)

    fig.suptitle("Codec: the store instruction, and everything else held still",
                 fontsize=12.5, y=0.975)
    theme.footer(fig, meta_text)
    fig.subplots_adjust(left=0.075, right=0.98, top=0.83, bottom=0.16, wspace=0.30)
    return fig


# ---- Figure 2: the system level -----------------------------------------------------------


def plot_system(before, after, cycles_before, cycles_after, meta_text):
    fig, (ax_r, ax_c) = plt.subplots(
        1, 2, figsize=(10.6, 4.9), gridspec_kw={"width_ratios": [1.35, 1.0]}
    )
    labels = [("tuple", "tuple read\n(same output)"), ("zerocopy", "columnar read\n(one sum)")]
    present = [k for k, _l in labels if k in before and k in after]
    xs = list(range(len(present)))

    for i, x in enumerate(xs):
        fp_b, ad_b = before[present[i]]
        fp_a, ad_a = after[present[i]]
        for off, value, color, alpha in (
            (-0.30, fp_b, ACCENT, FADED),      # this repo, before
            (-0.06, fp_a, ACCENT_AFTER, 1.0),  # this repo, after
            (0.22, ad_b, GREY[0], 1.0),        # the rival, before
            (0.46, ad_a, GREY[0], 1.0),        # the rival, after
        ):
            ax_r.bar(x + off, value, width=0.22, color=color, alpha=alpha, zorder=3)
            ax_r.text(x + off, value, f"{value:.2f}", ha="center", va="bottom", fontsize=7.0)
        ax_r.text(x + 0.34, max(ad_b, ad_a) / 2, "ADBC\ncontrol", ha="center", va="center",
                  fontsize=6.4, color="#555555")
    deltas = ", ".join(f"{dict(labels)[k]}: {100 * after[k][0] / before[k][0]:.0f}%"
                       for k in present)
    ax_r.set_xticks(xs)
    ax_r.set_xticklabels([label for k, label in labels if k in present], fontsize=8)
    ax_r.set_ylabel("M rows/s, 1M rows")
    ax_r.set_title(f"light = before, dark = after · {deltas}", fontsize=9.5)
    ax_r.set_ylim(0, max([v for pair_ in list(before.values()) + list(after.values())
                          for v in pair_]) * 1.18)
    ax_r.margins(x=0.12)
    theme.declutter(ax_r)

    if cycles_before and cycles_after:
        for i, (value, color, alpha) in enumerate(
            ((cycles_before, ACCENT, FADED), (cycles_after, ACCENT_AFTER, 1.0))
        ):
            ax_c.bar(i, value, width=0.45, color=color, alpha=alpha, zorder=3)
            ax_c.text(i, value, f"{value:.0f}", ha="center", va="bottom", fontsize=8.5)
        ax_c.set_xticks([0, 1])
        ax_c.set_xticklabels(["before", "after"])
        ax_c.set_ylabel("cycles/row (client pricer)")
        ax_c.set_title(
            f"the client's row decode: {100 * cycles_after / cycles_before:.0f}%",
            fontsize=9.5,
        )
        ax_c.set_ylim(0, max(cycles_before, cycles_after) * 1.22)
        ax_c.margins(x=0.3)
        theme.declutter(ax_c)
    else:
        ax_c.axis("off")
        ax_c.text(0.5, 0.5, "no read_rows: line in one of the logs", ha="center",
                  va="center", fontsize=9, color="#888888", transform=ax_c.transAxes)

    fig.suptitle("System level: rows per second, and what the client spends per row",
                 fontsize=12.5, y=0.975)
    theme.footer(fig, meta_text + "\ncaveat: the ADBC control moved ±2%, this repo's two paths moved "
                            "-7% and +11% — this harness cannot resolve a 3% client-side change")
    fig.subplots_adjust(left=0.075, right=0.975, top=0.82, bottom=0.20, wspace=0.3)
    return fig


# ---- Figure 3: the transport --------------------------------------------------------------


def plot_transport(before_list, after_list, before, after, inflight, meta_text):
    fig = plt.figure(figsize=(12.0, 4.9))
    gs = fig.add_gridspec(1, 3, width_ratios=[1.0, 1.0, 1.5], wspace=0.38)
    ax_g = fig.add_subplot(gs[0])
    ax_l = fig.add_subplot(gs[1])
    ax_i = fig.add_subplot(gs[2])

    # Transport cells are named transport_<t> and each transport runs twice (once
    # per payload), so they must be read as a *list*: keying them by cell name
    # keeps only the last payload. Their payload is identified by its size — the
    # format cells say how big fpcolumn and arrow-ipc are.
    def payload_names(cells):
        return {c["payload_bytes"]: name for name, c in cells.items()
                if name in ("fpcolumn", "arrow-ipc") and "payload_bytes" in c}

    def pairs_of(cell_list, format_cells):
        names = payload_names(format_cells)
        out = {}
        for c in cell_list:
            if "gb_per_s" not in c or not c.get("transport"):
                continue
            out[(names.get(c["payload_bytes"], "?"), c["transport"])] = c
        return out

    before_c = pairs_of(before_list, before)
    after_c = pairs_of(after_list, after)
    keys = [(p, t) for p in ("fpcolumn", "arrow-ipc") for t in ("shm", "uds")]
    rows = [(k, before_c[k], after_c[k]) for k in keys if k in before_c and k in after_c]
    ys = list(range(len(rows)))
    for y, (k, b, a) in zip(ys, rows):
        color = ACCENT if k[1] == "shm" else GREY[1]
        ax_g.barh([y - 0.18], [b["gb_per_s"]], height=0.34, color=color, alpha=FADED, zorder=3)
        ax_g.barh([y + 0.18], [a["gb_per_s"]], height=0.34, color=color, zorder=3)
        ax_g.text(max(b["gb_per_s"], a["gb_per_s"]), y,
                  f"{a['gb_per_s']:.2f} GB/s", va="center", fontsize=7.4)
        ax_l.barh([y - 0.18], [b["p50_ms"] * 1e3], height=0.34, color=color, alpha=FADED, zorder=3)
        ax_l.barh([y + 0.18], [a["p50_ms"] * 1e3], height=0.34, color=color, zorder=3)
        ax_l.text(max(b["p50_ms"], a["p50_ms"]) * 1e3, y,
                  f"{a['p50_ms'] * 1e3:.1f} μs", va="center", fontsize=7.4)
    labels = [f"{FORMAT_LABELS.get(k[0], k[0])}\n{TRANSPORT_LABELS.get(k[1], k[1])}"
              for k, _b, _a in rows]
    for ax, title, xlabel, key, scale in (
        (ax_g, "throughput", "GB/s", "gb_per_s", 1.0),
        (ax_l, "median latency", "p50, μs", "p50_ms", 1e3),
    ):
        ax.set_yticks(ys)
        ax.set_yticklabels(labels, fontsize=7.4)
        ax.invert_yaxis()
        ax.set_xlabel(f"{xlabel} (light = before, dark = after)")
        ax.set_title(f"{title}, same bytes, default protocol", fontsize=9.5)
        if rows:
            hi = max(max(b[key], a[key]) * scale for _k, b, a in rows)
            ax.set_xlim(0, hi * 1.30)
        theme.declutter(ax)

    # The in-flight sweep: median of the repeats per N, on a log2 axis, so one
    # point per setting (the raw log has every run and they overlapped).
    if inflight:
        by_n = {}
        for c in inflight:
            if c.get("transport") == "shm" and c.get("gb_per_s") is not None:
                by_n.setdefault(int(c.get("in_flight", 1)), []).append(c)
        ns = sorted(by_n)
        if ns:
            def median(cells, key):
                vals = sorted(c[key] for c in cells)
                return vals[len(vals) // 2]

            gb = [median(by_n[n], "gb_per_s") for n in ns]
            p50 = [median(by_n[n], "p50_ms") * 1e3 for n in ns]
            base = gb[0]
            ax_i.plot(ns, gb, marker="o", color=ACCENT_AFTER, linewidth=1.9, zorder=3)
            for idx, (point, value, p) in enumerate(zip(ns, gb, p50)):
                # alternate above/below: five three-line labels in a row collided
                up = idx % 2 == 0
                ax_i.annotate(f"{value:.1f} GB/s\n({100 * value / base:.0f}%)\n"
                              f"p50 {p:,.0f} μs",
                              (point, value), textcoords="offset points",
                              xytext=(0, 10 if up else -10), ha="center",
                              va="bottom" if up else "top", fontsize=7.2, linespacing=1.3)
            ax_i.set_xscale("log", base=2)
            ax_i.set_xticks(ns)
            ax_i.set_xticklabels([str(v) for v in ns])
            ax_i.xaxis.set_minor_formatter(NullFormatter())
            ax_i.set_xlim(ns[0] * 0.72, ns[-1] * 1.45)
            ax_i.set_xlabel("chunks in flight (median of the runs at each setting)")
            ax_i.set_ylabel("GB/s over the payload")
            ax_i.set_title("shared memory: overlapping the two sides", fontsize=9.5)
            ax_i.set_ylim(0, max(gb) * 1.45)
            theme.declutter(ax_i)
        else:
            ax_i.axis("off")
    else:
        ax_i.axis("off")
        ax_i.text(0.5, 0.5, "no --in-flight sweep log", ha="center", va="center",
                  fontsize=9, color="#888888", transform=ax_i.transAxes)

    fig.suptitle("Transport: unchanged bytes at the default, and what a window buys",
                 fontsize=12.5, y=0.975)
    theme.footer(fig, meta_text)
    fig.subplots_adjust(left=0.155, right=0.985, top=0.83, bottom=0.16)
    return fig


# ---- Figure 4: the cache fix, before and after --------------------------------------------


def plot_cache_fix(nt, counters, meta_text):
    """The non-temporal-store change: the time, and the cache traffic behind it."""
    if not nt or not counters:
        fig = plt.figure(figsize=(6, 3))
        fig.text(0.5, 0.5, "needs docs/improve-nt.log and docs/improve-cache-fix.log",
                 ha="center", va="center", fontsize=9, color="#888888")
        return fig

    def median(cells, key):
        vals = sorted(c[key] for c in cells)
        return vals[len(vals) // 2]

    plain = [c for c in nt if not c.get("nt_store")]
    stream = [c for c in nt if c.get("nt_store")]
    by_variant = {c["variant"]: c for c in counters}
    if not plain or not stream or "plain" not in by_variant or "nt" not in by_variant:
        fig = plt.figure(figsize=(6, 3))
        fig.text(0.5, 0.5, "incomplete logs", ha="center", va="center", fontsize=9)
        return fig

    fig, (ax_t, ax_c, ax_m) = plt.subplots(
        1, 3, figsize=(12.2, 4.9), gridspec_kw={"width_ratios": [1.05, 1.1, 1.0]}
    )
    groups = [("before the fix", ACCENT, FADED), ("after the fix", ACCENT_AFTER, 1.0)]
    xs = [0, 1]

    # (a) the time: encode + decode per 1M rows, medians of the alternating pairs
    enc = [median(plain, "encode_ms"), median(stream, "encode_ms")]
    dec = [median(plain, "decode_ms"), median(stream, "decode_ms")]
    for x, (label, color, alpha) in zip(xs, groups):
        ax_t.bar(x, enc[x], width=0.5, color=color, alpha=alpha, zorder=3)
        ax_t.bar(x, dec[x], width=0.5, bottom=enc[x], color=color, alpha=alpha * FADED,
                 zorder=3)
        ax_t.text(x, enc[x] / 2, f"encode {enc[x]:.2f}", ha="center", va="center",
                  fontsize=8, color="white" if x else "#333333")
        ax_t.text(x, enc[x] + dec[x] / 2, f"decode {dec[x]:.2f}", ha="center", va="center",
                  fontsize=7.6, color="white" if x else "#333333")
        ax_t.text(x, enc[x] + dec[x], f"{enc[x] + dec[x]:.2f} ms", ha="center", va="bottom",
                  fontsize=8.4)
    ax_t.set_xticks(xs)
    ax_t.set_xticklabels([g[0] for g in groups])
    ax_t.set_ylabel("ms per 1M rows")
    ax_t.set_title(f"the time: encode {100 * enc[1] / enc[0]:.0f}%, "
                   f"pair {100 * (enc[1] + dec[1]) / (enc[0] + dec[0]):.0f}%",
                   fontsize=9.5)
    ax_t.set_ylim(0, max(e + d for e, d in zip(enc, dec)) * 1.25)
    ax_t.margins(x=0.3)
    theme.declutter(ax_t)

    # (b) the mechanism: RFO requests and L2 reads per row
    p, n = by_variant["plain"], by_variant["nt"]
    for i, x in enumerate(xs):
        c = p if i == 0 else n
        color = groups[i][1]
        ax_c.bar(x - 0.17, c["rfo_requests_per_row"], width=0.30, color=color, zorder=3)
        ax_c.bar(x + 0.17, c["l2_reads_per_row"], width=0.30, color=color, alpha=FADED,
                 zorder=3)
        ax_c.text(x - 0.17, c["rfo_requests_per_row"], f"{c['rfo_requests_per_row']:.2f}",
                  ha="center", va="bottom", fontsize=8)
        ax_c.text(x + 0.17, c["l2_reads_per_row"], f"{c['l2_reads_per_row']:.2f}",
                  ha="center", va="bottom", fontsize=8)
    ax_c.set_xticks(xs)
    ax_c.set_xticklabels([g[0] for g in groups])
    ax_c.set_ylabel("requests per row")
    ax_c.set_title(f"the mechanism: RFO "
                   f"{100 * n['rfo_requests_per_row'] / p['rfo_requests_per_row']:.0f}%"
                   f" (solid), L2 reads (faded)", fontsize=9.5)
    ax_c.set_ylim(0, max(p["rfo_requests_per_row"], p["l2_reads_per_row"]) * 1.30)
    ax_c.margins(x=0.3)
    theme.declutter(ax_c)

    # (c) the consequence in the counters: L1 miss rate, cycles per row
    ax_m.bar([0 - 0.17, 1 - 0.17], [p["l1_miss_pct"], n["l1_miss_pct"]], width=0.30,
             color=[groups[0][1], groups[1][1]], zorder=3)
    for x, c in zip(xs, (p, n)):
        ax_m.text(x - 0.17, c["l1_miss_pct"], f"{c['l1_miss_pct']:.1f}%", ha="center",
                  va="bottom", fontsize=8)
    ax_m.set_ylabel("L1 miss rate, %")
    ax_m.set_xticks(xs)
    ax_m.set_xticklabels([g[0] for g in groups], fontsize=8)
    ax_m.set_title(f"L1 misses {100 * n['l1_miss_pct'] / p['l1_miss_pct']:.0f}%; "
                   f"cycles/row {p['cycles_per_row']:.1f} → {n['cycles_per_row']:.1f}",
                   fontsize=9.5)
    ax_m.set_ylim(0, max(p["l1_miss_pct"], n["l1_miss_pct"]) * 1.25)
    ax_m.margins(x=0.3)
    theme.declutter(ax_m)

    fig.suptitle("The cache fix: the destination's read-for-ownership, removed",
                 fontsize=12.5, y=0.975)
    theme.footer(fig, meta_text + "\nRFO = a store that had to read the cache line first; "
                            "it is the traffic non-temporal stores avoid")
    fig.subplots_adjust(left=0.075, right=0.985, top=0.82, bottom=0.20, wspace=0.30)
    return fig


# ---- main ---------------------------------------------------------------------------------


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--before-matrix", default="bench/results/ab/improve-before-matrix.log")
    p.add_argument("--after-matrix", default="bench/results/codec-transport.log")
    p.add_argument("--before-bench", default="bench/results/ab/improve-before-bench.log")
    p.add_argument("--after-bench", default="bench/results/ab/improve-after-bench.log")
    p.add_argument("--before-decode", default="bench/results/ab/improve-before-decode.log")
    p.add_argument("--after-decode", default="bench/results/ab/improve-after-decode.log")
    p.add_argument("--nt-log", default="bench/results/ab/improve-nt.log")
    p.add_argument("--cache-fix-log", default="bench/results/ab/improve-cache-fix.log")
    p.add_argument("--inflight-log", default="bench/results/ab/improve-inflight.log")
    args = p.parse_args()

    before_list = read_json_lines(args.before_matrix)
    after_list = read_json_lines(args.after_matrix)
    before = {c["cell"]: c for c in before_list if "cell" in c}
    after = {c["cell"]: c for c in after_list if "cell" in c}
    if not before or not after:
        sys.exit(f"need both matrix logs; got before={len(before)} after={len(after)} cells")

    nt = read_json_lines(args.nt_log) if Path(args.nt_log).exists() else []
    counters = read_json_lines(args.cache_fix_log) if Path(args.cache_fix_log).exists() else []
    inflight = read_json_lines(args.inflight_log) if Path(args.inflight_log).exists() else []

    m = meta(args.after_matrix) or meta(args.before_matrix)
    meta_text = " · ".join(x for x in (
        m.get("cpu", ""), m.get("rows", "") + " rows" if m.get("rows") else "",
        f"git {m.get('git', '?')}", "before -> after",
    ) if x)

    save(plot_codec(before, after, nt, meta_text), OUT / "improve-codec.png")
    save(plot_cache_fix(nt, counters, meta_text), OUT / "improve-cache-fix.png")
    save(plot_system(bench_rows(args.before_bench) if Path(args.before_bench).exists() else {},
                     bench_rows(args.after_bench) if Path(args.after_bench).exists() else {},
                     decode_cycles(args.before_decode) if Path(args.before_decode).exists() else None,
                     decode_cycles(args.after_decode) if Path(args.after_decode).exists() else None,
                     meta_text), OUT / "improve-system.png")
    save(plot_transport(before_list, after_list, before, after, inflight, meta_text),
         OUT / "improve-transport.png")


if __name__ == "__main__":
    main()
