#!/usr/bin/env bash
# fastpostgres against ADBC + Arrow under one memory budget: the same rows, and
# what each side holds while it delivers them.
#
# The read benchmark (`just bench`) compares how fast the two transports hand over
# the same rows. This one fixes the ring's transfer buffer (--mib, 512 by default,
# split into two data segments of M/2) and reports what the Arrow side needed on
# its own terms: the record-batch reader holds one Arrow batch, the table path
# holds the whole result in Arrow buffers, and both report the bytes they carried
# next to the ring's fixed ones. Every player also reports its peak RSS.
#
# One player per process, one at a time (METHODOLOGY.md): interleaving them
# measures the neighbours' cache footprint. The table is rebuilt per size, so
# every read returns exactly n rows, and all three players must return the same
# column sum - a faster path that returned something else fails the run. The ring
# player cannot fall back to a socket (stream() raises instead), and the segment
# bytes it reports are the ring worker's own files, so a cell whose segment is not
# the one asked for fails the run too.
#
# Usage: bench/harness/arrow_memory_bench.sh [--quick] [--keep] [--mib N] [nrows ...]
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
image=${IMAGE:-fastpostgres-dev}
shm=${SHM:-3g}
shape=${SHAPE:-bench}
origin=fp-arrow-mem
player_py=/src/bench/rivals/adbc-arrow/arrow_memory_bench.py

quick=0
keep=0
mib=512
sizes=()
for arg in "$@"; do
  case "$arg" in
    --quick) quick=1 ;;
    --keep) keep=1 ;;
    --mib=*) mib=${arg#--mib=} ;;
    *) sizes+=("$arg") ;;
  esac
done
if [ ${#sizes[@]} -eq 0 ]; then
  if [ "$quick" = 1 ]; then sizes=(100000); else sizes=(1000000 5000000 10000000); fi
fi

to_bytes() {  # 3g | 512M | 1048576
  local num unit
  num=${1//[^0-9]/}
  unit=${1//[0-9]/}
  if [ -z "$num" ]; then echo "arrow_memory_bench: bad size $1" >&2; exit 2; fi
  case "$unit" in
    "" | B | b) echo "$num" ;;
    k | K | KiB | kib | KB | kb) echo $((num * 1024)) ;;
    m | M | MiB | mib | MB | mb) echo $((num * 1024 * 1024)) ;;
    g | G | GiB | gib | GB | gb) echo $((num * 1024 * 1024 * 1024)) ;;
    *) echo "arrow_memory_bench: bad size $1" >&2; exit 2 ;;
  esac
}

fmt_bytes() {
  awk -v b="$1" 'BEGIN {
    if (b >= 1073741824) printf "%.2f GiB", b / 1073741824;
    else if (b >= 1048576) printf "%.0f MiB", b / 1048576;
    else printf "%.0f KiB", b / 1024;
  }'
}

budget=$((mib * 1024 * 1024))
# stream() maps one data segment and pulls batch by batch, so the whole budget
# goes into that one segment (the cursor path, which alternates two, would split
# it in half instead). The 8 MiB control ring is fixed and on top of M.
segment=$budget
shm_bytes=$(to_bytes "$shm")
if [ "$budget" -gt "$shm_bytes" ]; then
  echo "arrow_memory_bench: a ${mib} MiB ring needs /dev/shm at least that big (SHM=$shm)" >&2
  exit 2
fi

# These players build no per-row Python objects, so the read benchmark's
# one-rep-past-1M rule (its object arena) does not apply: three reps throughout.
reps_for() {
  if [ "$quick" = 1 ]; then echo 1; else echo 3; fi
}

# shellcheck disable=SC2329 # invoked by the EXIT trap
cleanup() {
  if [ "$keep" = 1 ]; then return 0; fi
  docker rm -f "$origin" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker build -q --target dev -t "$image" -f "$root/docker/Dockerfile" "$root" >/dev/null 2>&1
docker rm -f "$origin" >/dev/null 2>&1 || true
docker run -d --name "$origin" --shm-size="$shm" -e "SHAPE=$shape" \
  "$image" sleep infinity >/dev/null
for _ in $(seq 60); do docker exec "$origin" pg_isready -h /tmp -q && break; sleep 1; done

measure() {  # <player> <n> <segment bytes|-> <reps>
  local player_name=$1 n=$2 seg=$3 reps=$4
  local args=(-u postgres -e "SHAPE=$shape")
  [ "$seg" != "-" ] && args+=(-e "FASTPOSTGRES_SEGMENT_BYTES=$seg")
  docker exec "${args[@]}" "$origin" python3 "$player_py" read "$player_name" "$n" "$reps"
}

fmt_rate() { awk -v n="$1" -v ms="$2" 'BEGIN{printf "%.2fM", n / (ms / 1000) / 1e6}'; }
vs_ring() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%+.1f%%", (b / a - 1) * 100}'; }
spread_of() {
  awk -v lo="$1" -v hi="$2" -v reps="$3" 'BEGIN{
    if (reps < 2 || lo == hi) { print "n=1"; exit }
    printf "±%.1f%%", 100 * (hi - lo) / 2 / ((lo + hi) / 2);
  }'
}
same_sum() {
  awk -v a="$1" -v b="$2" 'BEGIN{
    d = a - b; if (d < 0) d = -d;
    m = (a < 0 ? -a : a) + (b < 0 ? -b : b);
    exit !((m == 0 && d == 0) || d <= 1e-6 * m);
  }'
}
shm_check() {  # the ring's own segment: what it mapped, not what was asked
  local evidence=$1 got
  got=$(sed -n 's/.*seg_bytes=\([0-9]*\).*/\1/p' <<< "$evidence")
  [ "$got" = "$segment" ]
}

# ---- the run -------------------------------------------------------------

echo "### ENV"
awk -F': ' '/^model name/ {print "cpu: " $2; exit}' /proc/cpuinfo
awk '/MemTotal:/ {printf "mem: %.0f GiB\n", $2 / 1048576}' /proc/meminfo
echo "cores: $(nproc)"
echo "postgres: $(docker exec "$origin" psql --version | awk '{print $3}')"
docker exec "$origin" python3 -c 'import sys, importlib.metadata as m
print("python:", sys.version.split()[0])
print("sqlalchemy:", m.version("sqlalchemy"))
print("pyarrow:", m.version("pyarrow"))
print("adbc:", m.version("adbc_driver_postgresql"))'
echo "fastpostgres image: $image $(docker image inspect --format '{{.Id}}' "$image" | cut -c1-19)"
echo "shm: $shm"
echo "ring budget: $(fmt_bytes "$budget") (one data segment, stream() pulls from it; +8 MiB control ring)"
echo "shape: $shape"
echo "sizes: ${sizes[*]}"
echo "reps: $(reps_for)"
echo "git: $(git -C "$root" describe --always --dirty 2>/dev/null || echo unknown)"
echo "### END ENV"

mismatch=0
fallback=0
for n in "${sizes[@]}"; do
  reps=$(reps_for)
  echo ""
  echo "# $n rows, $reps timed rep(s) per player, one process each"
  docker exec -u postgres -e "SHAPE=$shape" "$origin" python3 "$player_py" setup "$n"
  # fresh segments, so the ring's evidence is this size's own files
  docker exec "$origin" sh -c 'rm -f /dev/shm/fp_*' || true

  declare -A rate med_of spread evidence sum
  for player_name in ring arrow-batch arrow-table; do
    seg=-
    [ "$player_name" = ring ] && seg=$segment
    out=$(measure "$player_name" "$n" "$seg" "$reps")
    read -r med lo hi _ total_sum <<< "$(sed -n 1p <<< "$out")"
    evidence[$player_name]=$(sed -n 2p <<< "$out")
    rate[$player_name]=$(fmt_rate "$n" "$med")
    med_of[$player_name]=$med
    spread[$player_name]=$(spread_of "$lo" "$hi" "$reps")
    sum[$player_name]=$total_sum
  done

  sum_ok=ok
  for player_name in arrow-batch arrow-table; do
    same_sum "${sum[ring]}" "${sum[$player_name]}" || sum_ok=MISMATCH
  done
  [ "$sum_ok" = ok ] || mismatch=1

  if ! shm_check "${evidence[ring]}"; then
    echo "  WARNING the ring mapped $(sed -n 's/.*seg_bytes=\([0-9]*\).*/\1/p' <<< "${evidence[ring]}")B, wanted ${segment}B"
    fallback=1
  fi

  printf '  %-13s %8s %9s %8s\n' "player" "rows/s" "vs ring" "spread"
  for player_name in ring arrow-batch arrow-table; do
    delta="-"
    [ "$player_name" != ring ] && delta=$(vs_ring "${med_of[ring]}" "${med_of[$player_name]}")
    printf '  %-13s %8s %9s %8s\n' "$player_name" "${rate[$player_name]}" "$delta" "${spread[$player_name]}"
    printf '  %-13s %s\n' "" "${evidence[$player_name]}"
  done
  printf '  %-13s %s (%s)\n' "sum" "${sum[ring]}" "$sum_ok"
  unset rate med_of spread evidence sum
done

echo ""
echo "# the same sum across players: $([ "$mismatch" = 0 ] && echo ok || echo MISMATCH)"
if [ "$fallback" = 1 ]; then
  echo "# the ring did not map the asked-for segment: those timings are not the budget's"
fi
exit "$((mismatch + fallback))"
