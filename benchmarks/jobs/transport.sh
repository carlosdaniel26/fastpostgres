#!/usr/bin/env bash
# transport.sh — the HOST-side record for the ring's two doorbells.
#
# Runs on the host, not in the dev image: one of the two transports is the
# regipc kernel module's /dev/regipc, which only exists where the module was
# loaded, and the measuring binary needs no PostgreSQL (benchmarks/internals/
# transport builds the product's engines with plain gcc). The guards below have
# named remedies; the module itself is never built or loaded here — insmod is a
# deliberate human step.
#
#   transport.sh [counts] [reps] [batch_rows]
#
#   counts      comma-separated hop counts, one row per count (default 100000,1000000)
#   reps        runs per cell, one JSON record each (default 3)
#   batch_rows  rows a real batch would have carried (default 100000)
#
# What a hop is: the client posts the product's request descriptor — op = NEXT,
# seq, batch, data_bytes — through the engine and waits for the worker's
# resp_seq/status; nothing else moves. There is no data segment and no payload,
# so hops/s is the doorbell's rate, rows/s is hops/s x batch_rows (the count a
# real batch would have carried), and MB/s is the descriptor's 72 bytes per hop
# (nine 64-bit carriers), not bytes of payload.
#
# stdout is the record: the ### TRANSPORT block (the machine and the module, the
# counts, the caveats), one table per method, and finally ### RESULTS: one line
# of JSON carrying the machine block and every run's own JSON record. A cell
# that cannot run or whose records are not verified is printed as FAILED and the
# script exits non-zero — a method is never silently skipped.
set -euo pipefail

counts=${1:-100000,1000000}
reps=${2:-3}
batch_rows=${3:-100000}

case $reps in
*[!0-9]* | '')
    echo "transport: reps '$reps' is not a positive integer" >&2
    exit 2
    ;;
esac
case $batch_rows in
*[!0-9]* | '')
    echo "transport: batch_rows '$batch_rows' is not a positive integer" >&2
    exit 2
    ;;
esac
[ "$reps" -gt 0 ] || {
    echo "transport: reps must be positive" >&2
    exit 2
}
[ "$batch_rows" -gt 0 ] || {
    echo "transport: batch_rows must be positive" >&2
    exit 2
}
IFS=',' read -r -a count_list <<<"$counts"
for count in "${count_list[@]}"; do
    case $count in
    *[!0-9]* | '' | 0)
        echo "transport: counts '$counts' must be comma-separated positive integers" >&2
        exit 2
        ;;
    esac
done

here=$(cd "$(dirname "$0")/../.." && pwd)
bench="$here/benchmarks/internals/transport/build/peer"

# The two hosts this record cannot run on, with the one-line fix each. The
# module's ABI is asked through the module itself (REGIPC_IOC_STATS, by
# `peer --abi`) after the binary guard, because that probe needs the binary.
if [ ! -e /dev/regipc ]; then
    echo "transport: /dev/regipc is missing: the regipc module is not loaded: sudo insmod extension/transport/regipc/module/regipc.ko mode=0666" >&2
    exit 1
fi
if [ ! -w /dev/regipc ]; then
    echo "transport: /dev/regipc is not writable by $(id -un): the module was loaded with the wrong mode; reload it: sudo rmmod regipc && sudo insmod extension/transport/regipc/module/regipc.ko mode=0666" >&2
    exit 1
fi
if [ ! -x "$bench" ]; then
    echo "transport: $bench is missing: run make -C benchmarks/internals/transport" >&2
    exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

if ! "$bench" --abi >"$work/abi.json" 2>"$work/abi.err"; then
    echo "transport: /dev/regipc did not answer REGIPC_IOC_STATS:" >&2
    sed 's/^/  /' "$work/abi.err" >&2
    echo "  if the loaded module is not the vendored ABI 2, reload it: sudo rmmod regipc && sudo insmod extension/transport/regipc/module/regipc.ko mode=0666" >&2
    exit 1
fi
abi=""
abi=$(python3 -c 'import json,sys; print(json.load(sys.stdin).get("abi",""))' <"$work/abi.json" 2>/dev/null) || abi=""
if [ -z "$abi" ]; then
    echo "transport: /dev/regipc did not report an ABI (peer --abi printed no usable JSON); if the loaded module is not the vendored ABI 2, reload it: sudo rmmod regipc && sudo insmod extension/transport/regipc/module/regipc.ko mode=0666" >&2
    exit 1
fi
if [ "$abi" != "2" ]; then
    echo "transport: /dev/regipc answers ABI $abi, the vendored ABI is 2: reload the module: sudo rmmod regipc && sudo insmod extension/transport/regipc/module/regipc.ko mode=0666" >&2
    exit 1
fi

# The machine and the module, as the record should carry them.
cpu=$(awk -F': ' '/^model name/ {print $2; exit}' /proc/cpuinfo)
ghz=$(lscpu 2>/dev/null | awk -F: '/CPU max MHz/ {printf "%.1f", $2 / 1000; exit}')
cores=$(nproc)
kernel=$(uname -r)
regipc_mode=$(stat -c '0%a' /dev/regipc)
regipc_version=$(cat /sys/module/regipc/version 2>/dev/null || echo unknown)
regipc_srcversion=$(cat /sys/module/regipc/srcversion 2>/dev/null || echo unknown)
regipc_params=""
for p in /sys/module/regipc/parameters/*; do
    [ -e "$p" ] || continue
    regipc_params="${regipc_params:+$regipc_params,}${p##*/}=$(<"$p")"
done
gitrev=$(git -C "$here" describe --always --dirty 2>/dev/null || echo unknown)

echo "### TRANSPORT"
echo "cpu: ${cpu:-unknown}"
echo "ghz: ${ghz:-unknown}"
echo "cores: $cores"
echo "kernel: $kernel"
echo "regipc: /dev/regipc mode $regipc_mode; module version ${regipc_version}; srcversion $regipc_srcversion; abi $abi; params ${regipc_params:-none}"
echo "counts: $counts"
echo "reps: $reps (one JSON record each; the tables report the median over the reps)"
echo "batch_rows: $batch_rows"
echo "git: $gitrev"
echo "# doorbell only: the hop is the product's request descriptor (op=NEXT, seq, batch,"
echo "# data_bytes) and the worker's resp_seq/status. No data segment exists and no"
echo "# payload moves; MB/s is the descriptor's 72 bytes per hop, not payload."
echo "# rows/s = hops/s x batch_rows: --batch-rows is what a real batch would have"
echo "# carried, so the conversion is honest arithmetic, not rows produced."
echo "# CPU/hop is user+sys over both processes for the timed hops. The regipc engine"
echo "# spins up to 100 us per side before falling back to its semaphore, so its spin"
echo "# is in the CPU; the shm engine sleeps on a futex per handoff."
echo "# The worker stops through the engine's post_request (the Python client's"
echo "# Ring_stop shape); the semaphore-only stop (fastpostgres_ring_stop) is noticed"
echo "# by a regipc worker only at its 100 ms wait_request timeout. teardown_ms in the"
echo "# per-run records is the stop's real cost, measured outside the timed hops."
echo "### END TRANSPORT"
echo

for method in shm regipc; do
    for count in "${count_list[@]}"; do
        stem="$work/cell_${method}_${count}"
        peer_rc=0
        "$bench" --method "$method" --count "$count" --reps "$reps" --batch-rows "$batch_rows" --json \
            >"$stem.jsonl" 2>"$stem.err" || peer_rc=$?
        printf '%s\n' "$peer_rc" >"$stem.rc"
    done
done

export BENCH_WORK="$work" BENCH_COUNTS="$counts" BENCH_REPS="$reps" BENCH_BATCH_ROWS="$batch_rows"
export BENCH_CPU="$cpu" BENCH_GHZ="$ghz" BENCH_CORES="$cores" BENCH_KERNEL="$kernel"
export BENCH_REGIPC_MODE="$regipc_mode" BENCH_REGIPC_VERSION="$regipc_version"
export BENCH_REGIPC_SRCVERSION="$regipc_srcversion" BENCH_REGIPC_PARAMS="$regipc_params"
export BENCH_REGIPC_ABI="$abi" BENCH_GIT="$gitrev"

# The tables and the one-line RESULTS record. Everything here is formatting and
# medians over the runs the peer already reported; nothing is timed twice.
python3 <<'PY'
import json
import os
import statistics
import sys

work = os.environ["BENCH_WORK"]
counts = [int(c) for c in os.environ["BENCH_COUNTS"].split(",")]
reps = int(os.environ["BENCH_REPS"])
batch_rows = int(os.environ["BENCH_BATCH_ROWS"])
methods = ("shm", "regipc")


def read_cell(method, count):
    stem = os.path.join(work, "cell_%s_%d" % (method, count))
    recs, rc, err = [], None, ""
    try:
        with open(stem + ".jsonl", encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    recs.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
    except OSError:
        pass
    try:
        with open(stem + ".rc", encoding="utf-8") as fh:
            rc = int(fh.read().strip())
    except (OSError, ValueError):
        pass
    try:
        with open(stem + ".err", encoding="utf-8") as fh:
            err = fh.read().strip()
    except OSError:
        pass
    return {"recs": recs, "rc": rc, "err": err}


cells = {(m, c): read_cell(m, c) for m in methods for c in counts}
failures = []
for key, cell in cells.items():
    ok = (cell["rc"] == 0 and len(cell["recs"]) == reps and
          all(r.get("verified") is True for r in cell["recs"]))
    cell["ok"] = ok
    if not ok:
        failures.append(key)


def median(values):
    return statistics.median(values) if values else None


def fmt_rate(v):
    if v is None:
        return "--"
    if v >= 1e9:
        return "%.2fG" % (v / 1e9)
    if v >= 1e6:
        return "%.2fM" % (v / 1e6)
    if v >= 1e3:
        return "%.1fk" % (v / 1e3)
    return "%.0f" % v


def fmt_num(v, digits):
    return "--" if v is None else "%.*f" % (digits, v)


print("# one table per method, the doorbell's rate")
print("# median over %d rep(s); min..max over the reps; us CPU/hop = "
      "(producer_cpu_s + consumer_cpu_s) x 1e6 / count" % reps)
for method in methods:
    first = next((r for c in counts for r in cells[(method, c)]["recs"]), None)
    mechanism = first.get("mechanism", "no record") if first else "no record"
    print()
    print("--- %s (%s) ---" % (method, mechanism))
    print("%9s  %12s  %24s  %8s  %8s  %9s  %7s  %10s  %s" % (
        "count", "hops/s", "(min..max)", "p50 us", "p99 us", "rows/s", "MB/s", "us CPU/hop", "verified"))
    for count in counts:
        cell = cells[(method, count)]
        recs = cell["recs"]
        if not recs:
            reason = cell["err"].splitlines()[-1] if cell["err"] else "peer exited %s" % cell["rc"]
            print("%9d  %12s  %24s  %8s  %8s  %9s  %7s  %10s  FAILED" % (
                count, "--", "--", "--", "--", "--", "--", "--"))
            print("# FAILED %s count=%d: %s" % (method, count, reason))
            continue
        hops = [r["hops_per_second"] for r in recs]
        p50 = statistics.median([r["us_per_hop_p50"] for r in recs])
        p99 = statistics.median([r["us_per_hop_p99"] for r in recs])
        rps = statistics.median([r["rows_per_second"] for r in recs])
        mbs = statistics.median([r["mb_per_second"] for r in recs])
        cpu = statistics.median([(r["producer_cpu_s"] + r["consumer_cpu_s"]) * 1e6 / r["count"]
                                 for r in recs])
        span = "%s..%s" % (fmt_rate(min(hops)), fmt_rate(max(hops)))
        print("%9d  %12s  %24s  %8s  %8s  %9s  %7s  %10s  %s" % (
            count, fmt_rate(statistics.median(hops)), span, fmt_num(p50, 3), fmt_num(p99, 3),
            fmt_rate(rps), fmt_num(mbs, 1), fmt_num(cpu, 2), "yes" if cell["ok"] else "NO"))
    for count in counts:
        cell = cells[(method, count)]
        recs = cell["recs"]
        if recs and not cell["ok"]:
            bad = [r for r in recs if r.get("verified") is not True]
            detail = ", ".join("rep %s" % r.get("rep") for r in bad) or "missing reps"
            print("# FAILED %s count=%d: %s unverified (rc=%s)" % (method, count, detail, cell["rc"]))

machine = {
    "cpu": os.environ["BENCH_CPU"],
    "ghz": os.environ["BENCH_GHZ"],
    "cores": int(os.environ["BENCH_CORES"]),
    "kernel": os.environ["BENCH_KERNEL"],
    "regipc": {
        "device": "/dev/regipc",
        "mode": os.environ["BENCH_REGIPC_MODE"],
        "module_version": os.environ["BENCH_REGIPC_VERSION"],
        "srcversion": os.environ["BENCH_REGIPC_SRCVERSION"],
        "params": os.environ["BENCH_REGIPC_PARAMS"],
        "abi": int(os.environ["BENCH_REGIPC_ABI"]),
    },
    "counts": counts,
    "reps": reps,
    "batch_rows": batch_rows,
    "git": os.environ["BENCH_GIT"],
}
doc = {
    "machine": machine,
    "runs": [r for m in methods for c in counts for r in cells[(m, c)]["recs"]],
}
if failures:
    doc["failed_cells"] = [{"method": m, "count": c, "rc": cells[(m, c)]["rc"],
                            "records": len(cells[(m, c)]["recs"]),
                            "error": cells[(m, c)]["err"].splitlines()[-1] if cells[(m, c)]["err"] else ""}
                           for m, c in failures]
print()
print("### RESULTS")
print(json.dumps(doc, separators=(",", ":")))
if failures:
    print("transport: %d cell(s) failed (see the FAILED lines above and failed_cells in the record)"
          % len(failures), file=sys.stderr)
    sys.exit(1)
PY
