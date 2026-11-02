#!/usr/bin/env bash
# Runs on the HOST, not in the image: the counters come from the kernel's PMU and
# `perf_event_paranoid=2` allows a user to count its own processes, while the
# units being measured are freestanding C++ (no libpq, no image, no root).
#
#   cache-stat.sh [rows] [reps]
#
# Three tables, three questions:
#
#   1. the decodes, per row — where the frame's memory traffic lands;
#   2. the transports, per KiB of payload — what the same bytes cost to move;
#   3. a working-set sweep (44 KiB, 4.6 MiB, 47 MiB) that shows the level
#      boundaries in the counters themselves.
#
# Two measurement decisions worth knowing before reading the numbers:
#
# * The decode cells are **differentials**. `fpg_bench` builds its dataset (about
#   1.4 s at 1M rows) before the timed region (4.8 ms), so counting the process
#   measures the generator: 96% of its cycles. Each cell is run twice, once with
#   one repetition and once with 1+R, and the difference is the codec's own work.
# * Only counters that validate are used. `L1-dcache-*` and `l2_request_g1.rd_blk_l`
#   reproduce a known 512 MB stream exactly; the Zen 4 `ls_*_fills_from_sys.dram_*`
#   fills report 2.18M where the truth is 16.8M (a ~8x undercount), so DRAM traffic
#   is not claimed anywhere. Likewise `dTLB-loads` is not "all loads" (537M loads,
#   14k counted), so only `dTLB-load-misses` — page walks — is reported.
set -euo pipefail

rows=${1:-1000000}
reps=${2:-3}
here=$(cd "$(dirname "$0")/../.." && pwd)
build="$here/benchmarks/internals/codec_transport/build"
bench="$build/fpg_bench"
transport="$build/transport_bench"

command -v perf >/dev/null || {
    echo "cache-stat: perf is not on PATH (linux-tools / perf on this distro)" >&2
    exit 1
}
[ -x "$bench" ] || {
    echo "cache-stat: $bench is missing: run 'just codec-transport' (or build_fpg.sh) first" >&2
    exit 1
}
[ -x "$transport" ] || {
    echo "cache-stat: $transport is missing: run 'just codec-transport' (or build_transport.sh) first" >&2
    exit 1
}

ghz=$(lscpu 2>/dev/null | awk -F: '/CPU max MHz/ {printf "%.1f", $2 / 1000; exit}')
cpu=$(awk -F': ' '/^model name/ {print $2; exit}' /proc/cpuinfo)
l3=$(lscpu 2>/dev/null | awk -F: '/^L3 cache/ {print $2; exit}' | tr -d ' ')

echo "### CACHE-STAT"
echo "cpu: ${cpu:-unknown}"
echo "ghz: ${ghz:-unknown}"
echo "l3: ${l3:-unknown}"
echo "rows: $rows"
echo "reps: $reps (median per counter)"
echo "git: $(git -C "$here" describe --always --dirty 2>/dev/null || echo unknown)"
echo "### END CACHE-STAT"
echo

# Six events: few enough that the PMU does not multiplex, so these are counts.
events="cycles,instructions,L1-dcache-loads,L1-dcache-load-misses,l2_request_g1.rd_blk_l,dTLB-load-misses"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# One perf run of one command: stdout is the tool's JSON, stderr the counters.
measure() {
    local name=$1
    shift

    local i
    for i in $(seq "$reps"); do
        # counts and warnings both land on stderr; the parser ignores anything
        # that is not `integer,,event,...` (this perf has no -q, only --quiet)
        perf stat -x, -e "$events" -- "$@" >"$work/$name.json" 2>"$work/$name.$i.counts" || {
            echo "cache-stat: $name failed under perf" >&2
            exit 1
        }
    done
}

median_event() {
    local name=$1 event=$2
    local vals=$work/$name.vals

    : >"$vals"
    local i
    for i in $(seq "$reps"); do
        # perf tags user-space events `cycles:u`; strip the qualifier before matching
        awk -F, -v e="$event" '{
            ev = $3
            sub(/:.*$/, "", ev)
            if (ev == e && $1 ~ /^[0-9]+$/) print $1
        }' "$work/$name.$i.counts" >>"$vals"
    done

    sort -n "$vals" | awk '{v[NR] = $1} END {
        if (NR == 0) { print "?"; exit }
        print (NR % 2 ? v[(NR + 1) / 2] : int((v[NR / 2] + v[NR / 2 + 1]) / 2))
    }'
}

# The differential: one repetition, then 1+R. Writes `cycles insn L1lds L1miss L2refs walks`.
diff_runs() {
    local name=$1 reps_add=$2
    shift 2

    measure "${name}_a" "$@" --reps 1
    measure "${name}_b" "$@" --reps "$((1 + reps_add))"

    local line="" e a b
    for e in cycles instructions L1-dcache-loads L1-dcache-load-misses \
        l2_request_g1.rd_blk_l dTLB-load-misses; do
        a=$(median_event "${name}_a" "$e")
        b=$(median_event "${name}_b" "$e")
        case $a$b in
        *"?"* | "")
            echo "cache-stat: counter '$e' missing for $name — the event list changed" >&2
            exit 1
            ;;
        esac
        line="$line $((b - a))"
    done
    echo "$line" >"$work/$name.diff"
}

# awk prints one table row from a file of six counters (label + 6 numbers).
row_awk='BEGIN {
    getline < f
    cy = $1; ins = $2; l1l = $3; l1m = $4; l2r = $5; walk = $6
    n = work + 0
    if (n == 0) n = 1
    printf "%-24s %9.1f %5.2f %8.2f %9.1f %9.2f %9.2f", \
        label, cy / n, (cy > 0 ? ins / cy : 0), (l1l > 0 ? 100 * l1m / l1l : 0),
        l1l / n, l1m / n, l2r / n
    if (walks) printf " %9.2f", walk / n
    printf "\n"
}'

row_line() { # label, work, file
    awk -v label="$1" -v work="$2" -v f="$3" -v walks="" "$row_awk"
}

row_line_walks() { # label, work, file
    awk -v label="$1" -v work="$2" -v f="$3" -v walks=1 "$row_awk"
}

# --nt-ab [rows] [R]: the non-temporal-store A/B at the *counter* level.
# The timing win was -33% on encode; this is the mechanism — the destination's
# read-for-ownership requests, which NT stores remove. Same differential as above
# (reps 1 vs 1+R) so the dataset generator cancels, and the same event set as
# `just cache-stat`, plus the two L2 RFO events. Prints one JSON object per
# variant so a chart can read it.
nt_ab() {
    local rows=${1:-1000000} reps_add=${2:-400}
    local nt_events="cycles,instructions,L1-dcache-loads,L1-dcache-load-misses"
    nt_events="$nt_events,l2_request_g1.rd_blk_l,l2_request_g1.rd_blk_x,l2_request_g1.change_to_x"

    echo "### CACHE-NT-AB"
    echo "cpu: ${cpu:-unknown}"
    echo "rows: $rows"
    echo "reps: 1 vs 1+$reps_add (differential, so the generator cancels)"
    echo "events: $nt_events"
    echo "### END CACHE-NT-AB"

    reps=1
    events=$nt_events
    local variant flag
    for variant in plain nt; do
        case $variant in
        plain) flag="" ;;
        nt) flag="--nt-store" ;;
        esac
        measure "ntab_${variant}_a" "$bench" --rows "$rows" --format fpcolumn --reps 1 $flag
        measure "ntab_${variant}_b" "$bench" --rows "$rows" --format fpcolumn --reps "$((1 + reps_add))" $flag

        local line="" e a b
        for e in cycles instructions L1-dcache-loads L1-dcache-load-misses \
            l2_request_g1.rd_blk_l l2_request_g1.rd_blk_x l2_request_g1.change_to_x; do
            a=$(median_event "ntab_${variant}_a" "$e")
            b=$(median_event "ntab_${variant}_b" "$e")
            case $a$b in
            *"?"* | "")
                echo "cache-stat: counter '$e' missing for $variant — the event list changed" >&2
                exit 1
                ;;
            esac
            line="$line $((b - a))"
        done
        awk -v variant="$variant" -v rows="$rows" -v reps="$reps_add" -v l="$line" 'BEGIN {
            split(l, v, " ")
            n = rows * reps
            printf "{\"variant\": \"%s\", \"rows\": %d, \"reps\": %d, " \
                   "\"cycles_per_row\": %.2f, \"l1_loads_per_row\": %.2f, " \
                   "\"l1_miss_pct\": %.2f, \"l2_reads_per_row\": %.2f, " \
                   "\"rfo_requests_per_row\": %.2f}\n",
                variant, rows, reps,
                v[1] / n,
                (v[3] > 0 ? v[3] / n : 0),
                (v[3] > 0 ? 100 * v[4] / v[3] : 0),
                v[5] / n,
                (v[6] + v[7]) / n
        }'
    done
}

# 1: the decodes, per row. -----------------------------------------------------------------
# The mode check comes before the numeric use of $rows (which is the mode string
# when --nt-ab is given). A failing `[` inside `if` does not trip set -e.
if [ "${1:-}" = "--nt-ab" ]; then
    nt_ab "${2:-1000000}" "${3:-400}"
    exit 0
fi

# R is large enough that the codec's own work outweighs the one-time generator.
if [ "$rows" -ge 100000 ]; then
    decode_reps=400
else
    decode_reps=2000
fi

for cell in fpcolumn fprows; do
    diff_runs "$cell" "$decode_reps" "$bench" --rows "$rows" --format "$cell"
done

echo "--- decodes, per row (differential, R=$decode_reps; payload 47.2 MiB at 1M rows) ---"
printf "%-24s %9s %5s %8s %9s %9s %9s\n" \
    "cell" "cycles/r" "IPC" "L1miss%" "L1lds/r" "L1miss/r" "L2->L3/r"
row_line "fpcolumn (decode+agg)" "$((rows * decode_reps))" "$work/fpcolumn.diff"
row_line "fprows (tuple bytes)" "$((rows * decode_reps))" "$work/fprows.diff"
echo "  (page walks are absent by construction: the differential counts repeats, and"
echo "   the payload is already faulted in by the first one)"

# 2: the transports, per KiB of payload. ---------------------------------------------------
payload=$work/payload.bin
"$bench" --rows "$rows" --format fpcolumn --reps 1 --emit-payload "$payload" >/dev/null

echo "--- transports, per KiB of payload (same bytes, codec out of the loop) ---"
printf "%-24s %11s %5s %8s %10s %10s %10s %9s %8s\n" \
    "cell" "cycles/KiB" "IPC" "L1miss%" "L1lds/KiB" "L1miss/KiB" "L2->L3/KiB" "walks/KiB" "p50 us"
for which in shm uds; do
    name="tr_$which"
    # no differential here: this path has no dataset generator, and both
    # transports pay the same one-time payload read
    measure "$name" "$transport" --which "$which" --payload "$payload"

    line=""
    for e in cycles instructions L1-dcache-loads L1-dcache-load-misses \
        l2_request_g1.rd_blk_l dTLB-load-misses; do
        line="$line $(median_event "$name" "$e")"
    done
    echo "$line" >"$work/$name.diff"

    kib=$(python3 -c 'import json,sys; print(int(round(json.load(open(sys.argv[1]))["payload_bytes"] / 1024)))' "$work/$name.json")
    p50=$(python3 -c 'import json,sys; print("{:.1f}".format(1000 * json.load(open(sys.argv[1]))["p50_ms"]))' "$work/$name.json")
    case $which in
    shm) label="shared memory" ;;
    uds) label="UNIX socket" ;;
    esac
    row_line_walks "$label" "$kib" "$work/$name.diff" | sed "s/$/ $p50/"
done
echo "  (per KiB, not per row: this experiment moves bytes and never decodes them."
echo "   User-space counters only (the events carry a :u qualifier): the socket's"
echo "   kernel copy and the shared segment's page faults are outside them, which is"
echo "   the whole caveat here — shm's two user-space passes over the bytes are"
echo "   visible, the socket's second pass happens in the kernel, unseen at paranoid=2.)"
echo

# 3: the working-set sweep, where the levels show up. ---------------------------------------
echo "--- working-set sweep, fpcolumn (44 KiB / 4.6 MiB / 47 MiB of payload) ---"
printf "%-10s %11s %9s %8s %9s %9s %8s\n" \
    "rows" "payload KiB" "cycles/r" "L1miss%" "L1miss/r" "L2->L3/r" "reps R"
for sweep_rows in 1000 100000 "$rows"; do
    if [ "$sweep_rows" -ge 100000 ]; then
        sweep_reps=400
    else
        sweep_reps=2000
    fi

    reps_saved=$reps
    reps=1 # the differential already repeats the work inside the tool
    diff_runs "sweep$sweep_rows" "$sweep_reps" "$bench" --rows "$sweep_rows" --format fpcolumn
    reps=$reps_saved

    kib=$(python3 -c 'import json,sys; print(round(json.load(open(sys.argv[1]))["payload_bytes"] / 1024, 1))' "$work/sweep${sweep_rows}_b.json")
    row_line "$sweep_rows" "$((sweep_rows * sweep_reps))" "$work/sweep$sweep_rows.diff" |
        awk '{printf "%-10s %11s %9s %8s %9s %9s %8s\n", $1, p, $2, $4, $6, $7, r}' \
            p="$kib" r="$sweep_reps"
done
echo "  (44 KiB fits L1, 4.6 MiB fits L3, 47 MiB does not: compare L1miss% and L2->L3/r across the three)"
