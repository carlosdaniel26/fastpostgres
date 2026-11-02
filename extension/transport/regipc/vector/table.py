#!/usr/bin/env python3
"""The two README tables, computed from the session's own record.

    make -C extension/transport/regipc/vector bulk-sweep   # writes sweep-bulk.jsonl
    python3 extension/transport/regipc/vector/table.py     # paste into README.md

One JSON line per run in, one markdown table per channel out: medians of the
per-run p50 / p99, calls/s and GB/s. Nothing is typed by hand; the README's
copy-path section quotes exactly this output.
"""
import argparse
import json
import statistics
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def median(runs, field):
    return statistics.median(r[field] for r in runs)


def table1(bulk):
    print("### Table 1 — the copy channel, from `sweep-bulk.jsonl` (mode `bulk`)")
    print()
    print("| length | p50 hot | p50 cold | calls/s hot | calls/s cold | GB/s hot | GB/s cold | verified |")
    print("|---|---|---|---|---|---|---|---|")
    for w in sorted({r["width"] for r in bulk}):
        hot = [r for r in bulk if r["width"] == w and not r.get("cold")]
        cold = [r for r in bulk if r["width"] == w and r.get("cold")]
        note = " (the largest the one-page arena accepts)" if w == max(r["width"] for r in bulk) else ""
        print(f"| {w} B{note} | {median(hot, 'us_per_call_p50'):.3f} µs | "
              f"{median(cold, 'us_per_call_p50'):.3f} µs | "
              f"{median(hot, 'calls_per_second') / 1e6:.3f} M | "
              f"{median(cold, 'calls_per_second') / 1e6:.3f} M | "
              f"{median(hot, 'gb_per_second'):.3f} | {median(cold, 'gb_per_second'):.3f} | "
              f"{'yes' if all(r['verified'] for r in hot + cold) else 'NO'} |")


def table2(vec):
    print("### Table 2 — the vector channel re-run in the same session, from `sweep-bulk.jsonl` (mode `vec`)")
    print()
    print("| width | real bytes | p50 | p99 | calls/s | arena GB/s | verified |")
    print("|---|---|---|---|---|---|---|")
    for w in sorted({r["width"] for r in vec}):
        rs = [r for r in vec if r["width"] == w]
        gbps = f"{median(rs, 'gb_per_second'):.3f}" if w else "-"
        print(f"| {w}{' (FP tax only)' if w == 0 else ''} | {rs[0]['real_bytes']} | "
              f"{median(rs, 'us_per_call_p50'):.3f} µs | {median(rs, 'us_per_call_p99'):.3f} µs | "
              f"{median(rs, 'calls_per_second') / 1e6:.3f} M | {gbps} | "
              f"{'yes' if all(r['verified'] for r in rs) else 'NO'} |")


def main():
    ap = argparse.ArgumentParser(description="Print the two README tables from the session jsonl")
    ap.add_argument("--log", type=Path, default=HERE / "sweep-bulk.jsonl")
    args = ap.parse_args()

    if not args.log.exists():
        sys.exit(f"{args.log}: no session yet -- run `make -C {HERE} bulk-sweep` first")

    runs = [json.loads(line) for line in args.log.read_text().splitlines() if line.startswith("{")]
    bulk = [r for r in runs if r.get("mode") == "bulk"]
    vec = [r for r in runs if r.get("mode") == "vec"]
    if not bulk or not vec:
        sys.exit(f"{args.log}: needs both bulk and vec runs (found {len(bulk)} bulk, {len(vec)} vec)")

    table1(bulk)
    print()
    table2(vec)


if __name__ == "__main__":
    main()
