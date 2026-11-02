#!/usr/bin/env bash
# fastpostgres against a result cache, with the same memory budget on both sides.
#
# One budget M, spent the same way on each side:
#
#   fastpostgres  the transfer buffer: two alternating data segments of M/2
#                 (FASTPOSTGRES_SEGMENT_BYTES), so a stream's shared memory stays
#                 within M, plus the fixed 8 MiB control ring
#   pgcache       the cache's own budget: the proxy container is capped at M
#                 (--memory, no swap), and PgCache keeps MEMORY_LIMIT=M (the
#                 footprint ceiling that throttles registration) and
#                 MEMO_CACHE_SIZE=M (the in-process result memo) inside it. Its
#                 metrics report the budget it resolved, which is 80% of the cap
#                 by design, so the cache runs with a little less than the ring
#
# swept across the budget a result stops fitting in. The question is not
# tools/pgcache_bench.sh's with smaller numbers: it is what each side buys with
# the same bytes. Every row therefore carries the evidence that its budget was
# real - for the ring, the segment the worker actually mapped (the harness clears
# /dev/shm between budgets, so a stale bigger segment cannot pass for this one's)
# and the timed reads that took the ring; for the cache, the proxy's own memory
# gauges. A ring row whose shm_reads falls short of its timed reps was served by
# the libpq fallback, and the run fails instead of reporting that as the ring.
#
# Containers, as in tools/pgcache_bench.sh:
#
#   fp-mem-origin  the dev image, PostgreSQL with the extension, started with
#                  wal_level=logical so PgCache can follow its logical
#                  replication stream. The players run in it via `docker exec`.
#   fp-mem-proxy   the PgCache image, joined to the origin's network namespace:
#                  it reaches the origin at 127.0.0.1:5432 and the players reach
#                  it at 127.0.0.1:6432, nothing published. Its embedded cache
#                  PostgreSQL binds 5433, so no collision.
#
# Fairness, per budget: the table is built once and never written again, and the
# proxy starts only after it exists (PgCache resets its cache on startup, so an
# entry can only ever hold rows from this run); the ring is timed with the proxy
# stopped; hits are proven by the proxy's own hit/miss counters, not assumed; and
# one read per workload after a proxy restart is timed as the cold fill.
#
# Usage: tools/pgcache_memory_bench.sh [--quick] [--keep] [--rows N]
#                                     [--budgets "8MiB 64MiB 256MiB 1024MiB"]
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
image=${IMAGE:-fastpostgres-dev}
pgcache_image=${PGCACHE_IMAGE:-pgcache/pgcache}
shm=${SHM:-3g}
shape=${SHAPE:-bench}
origin=fp-mem-origin
proxy=fp-mem-proxy
port=6432
player=/src/benchmark/pgcache/memory_player.py
pg_opts="-c wal_level=logical -c max_replication_slots=10 -c max_wal_senders=10"
reps_max_rows=1000000

quick=0
keep=0
rows=1000000
budgets_arg=""
for arg in "$@"; do
  case "$arg" in
    --quick) quick=1 ;;
    --keep) keep=1 ;;
    --rows=*) rows=${arg#--rows=} ;;
    --budgets=*) budgets_arg=${arg#--budgets=} ;;
    *) echo "pgcache_memory_bench: unknown argument $arg" >&2; exit 2 ;;
  esac
done
if [ "$quick" = 1 ]; then
  [ "$rows" = 1000000 ] && rows=10000
  [ -n "$budgets_arg" ] || budgets_arg="8MiB 256MiB"
else
  [ -n "$budgets_arg" ] || budgets_arg="8MiB 64MiB 256MiB 1024MiB"
fi

to_bytes() {  # 8MiB | 512M | 1g | 1048576
  local num unit
  num=${1//[^0-9]/}
  unit=${1//[0-9]/}
  if [ -z "$num" ]; then echo "pgcache_memory_bench: bad size $1" >&2; exit 2; fi
  case "$unit" in
    "" | B | b) echo "$num" ;;
    k | K | KiB | kib | KB | kb) echo $((num * 1024)) ;;
    m | M | MiB | mib | MB | mb) echo $((num * 1024 * 1024)) ;;
    g | G | GiB | gib | GB | gb) echo $((num * 1024 * 1024 * 1024)) ;;
    *) echo "pgcache_memory_bench: bad size $1" >&2; exit 2 ;;
  esac
}

fmt_bytes() {
  awk -v b="$1" 'BEGIN {
    if (b >= 1073741824) printf "%.2f GiB", b / 1073741824;
    else if (b >= 1048576) printf "%.0f MiB", b / 1048576;
    else printf "%.0f KiB", b / 1024;
  }'
}

shm_bytes=$(to_bytes "$shm")
budgets=()
for b in $budgets_arg; do
  bytes=$(to_bytes "$b")
  # 8 MiB: docker refuses a memory limit under 6 MB, and below a frame's worth
  # the ring would be measuring its own floor, not a budget
  if [ "$bytes" -lt $((8 * 1024 * 1024)) ]; then
    echo "pgcache_memory_bench: budget $b is under 8 MiB (docker's own minimum is" >&2
    echo "  6 MB, and PgCache's smallest possible container is far above both)" >&2
    exit 2
  fi
  if [ "$bytes" -gt "$shm_bytes" ]; then
    echo "pgcache_memory_bench: budget $b is larger than /dev/shm ($shm); set SHM=" >&2
    exit 2
  fi
  budgets+=("$bytes")
done

reps_for() {
  if [ "$quick" = 1 ] || [ "$1" -gt "$reps_max_rows" ]; then echo 1; else echo 3; fi
}

cleanup() {
  if [ "$keep" = 1 ]; then return 0; fi
  docker rm -f "$proxy" "$origin" >/dev/null 2>&1 || true
}
trap cleanup EXIT

# ---- the two containers --------------------------------------------------

docker build -q --target dev -t "$image" -f "$root/image/Dockerfile" "$root" >/dev/null 2>&1
docker image inspect "$pgcache_image" >/dev/null 2>&1 || docker pull "$pgcache_image"

docker rm -f "$origin" "$proxy" >/dev/null 2>&1 || true
docker run -d --name "$origin" --shm-size="$shm" \
  -e "PG_OPTS=$pg_opts" -e "SHAPE=$shape" \
  "$image" sleep infinity >/dev/null
for _ in $(seq 60); do docker exec "$origin" pg_isready -h /tmp -q && break; sleep 1; done

probe="import urllib.request, psycopg
psycopg.connect('host=127.0.0.1 port=$port dbname=postgres user=postgres').close()
urllib.request.urlopen('http://127.0.0.1:9090/metrics', timeout=5).read()"

start_proxy() {  # <budget bytes>; non-zero: the cache cannot run at this budget
  docker rm -f "$proxy" >/dev/null 2>&1 || true
  # --memory is what makes the budget M mean the cache's own memory: PgCache
  # compares its footprint against the memory it can detect, which is the host's
  # when the container is uncapped - and on a machine that is already using more
  # than M, that alone would throttle every registration and the cache would
  # never hold anything. --memory-swap equal to it: no swap to exceed M with.
  if ! docker run -d --name "$proxy" --network "container:$origin" \
    --memory "$1" --memory-swap "$1" \
    -e "UPSTREAM_URL=postgres://postgres@127.0.0.1:5432/postgres" \
    -e "LISTEN_PORT=$port" -e "METRICS_PORT=9090" -e PGCACHE_TELEMETRY=off \
    -e "MEMORY_LIMIT=$1" -e "MEMO_CACHE_SIZE=$1" \
    "$pgcache_image" >/dev/null; then
    echo "pgcache_memory_bench: cannot start the proxy container at $(fmt_bytes "$1")" >&2
    exit 1
  fi
  wait_proxy
}

# 0 once the proxy serves; 1 when the container died (PgCache needs more than M
# bytes to exist - a fact about the budget, not a broken run) and sets
# unavailable_reason; a container that stays alive but never becomes ready is a
# real failure and stops the run.
unavailable_reason=""
wait_proxy() {
  local i state
  for i in $(seq 120); do
    docker exec "$origin" python3 -c "$probe" 2>/dev/null && return 0
    state=$(docker inspect -f '{{.State.Running}} {{.State.OOMKilled}}' "$proxy" 2>/dev/null || echo "gone false")
    if [ "$state" = "false true" ]; then
      unavailable_reason="OOM-killed"
      return 1
    fi
    if [ "$state" = "false false" ]; then
      echo "pgcache_memory_bench: the proxy container exited; its log:" >&2
      docker logs "$proxy" --tail 40 >&2 || true
      exit 1
    fi
    sleep 1
  done
  echo "pgcache_memory_bench: the proxy did not come up; its log:" >&2
  docker logs "$proxy" --tail 40 >&2 || true
  exit 1
}

measure() {  # <player> <workload> <segment bytes|-> [--cold]
  local player_name=$1 workload=$2 segment=$3
  shift 3
  local args=(-u postgres -e "SHAPE=$shape")
  [ "$segment" != "-" ] && args+=(-e "FASTPOSTGRES_SEGMENT_BYTES=$segment")
  docker exec "${args[@]}" "$origin" python3 "$player" read "$player_name" "$workload" "$rows" "$reps" "$@"
}

fmt_rows() { awk -v n="$1" -v ms="$2" 'BEGIN{printf "%.2fM", n / (ms / 1000) / 1e6}'; }
vs_fpg() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%+.1f%%", (b / a - 1) * 100}'; }
ms_of() { echo "$1" | awk '{printf "%.3f ms", $1}'; }
shm_reads_of() { sed -n 's/.*shm_reads=\([0-9]*\)\/.*/\1/p' <<< "$1"; }
evidence_of() { sed -n '2p' <<< "$1" | sed 's/^ring  *//; s/^cache  *//'; }

# ---- the run -------------------------------------------------------------

echo "### ENV"
awk -F': ' '/^model name/ {print "cpu: " $2; exit}' /proc/cpuinfo
awk '/MemTotal:/ {printf "mem: %.0f GiB\n", $2 / 1048576}' /proc/meminfo
echo "cores: $(nproc)"
echo "postgres: $(docker exec "$origin" psql --version | awk '{print $3}')"
docker exec "$origin" python3 -c 'import sys, psycopg, importlib.metadata as m
print("python:", sys.version.split()[0])
print("psycopg:", psycopg.__version__.split()[0])
print("sqlalchemy:", m.version("sqlalchemy"))'
echo "fastpostgres image: $image $(docker image inspect --format '{{.Id}}' "$image" | cut -c1-19)"
echo "pgcache image: $pgcache_image $(docker image inspect --format '{{.Id}}' "$pgcache_image" | cut -c1-19)"
echo "origin: $pg_opts"
echo "shm: $shm"
echo "shape: $shape"
echo "rows: $rows"
printf 'budgets:'
for b in "${budgets[@]}"; do printf ' %s' "$(fmt_bytes "$b")"; done
echo ""
echo "git: $(git -C "$root" describe --always --dirty 2>/dev/null || echo unknown)"
echo "### END ENV"

# The table is built once, before any proxy start: every budget reads the same
# rows, and no cache entry can predate this run's table.
docker exec -u postgres -e "SHAPE=$shape" "$origin" python3 "$player" setup "$rows"

mismatch=0
fallback=0
unavailable=()
for budget in "${budgets[@]}"; do
  segment=$((budget / 2))
  reps=$(reps_for "$rows")
  echo ""
  echo "# budget $(fmt_bytes "$budget"), segment $(fmt_bytes "$segment") x2, $rows rows, $reps timed rep(s) per player, one process each"
  echo "# (the ring is measured with the proxy stopped; /dev/shm is cleared first so the evidence is this budget's)"

  # fresh segments: a stale bigger one would not shrink (the worker only grows a
  # segment file), and the evidence line must report this budget's bytes
  docker exec "$origin" sh -c 'rm -f /dev/shm/fp_*' || true

  declare -A res evidence
  for workload in large small through; do
    out=$(measure fastpostgres "$workload" "$segment")
    res["fastpostgres/$workload"]=$(sed -n 1p <<< "$out")
    evidence["fastpostgres/$workload"]=$(evidence_of "$out")
    got=$(shm_reads_of "$out")
    if [ -n "$got" ] && [ "$got" -lt "$reps" ]; then
      echo "  WARNING fastpostgres/$workload: $got of $reps timed reads took the ring (libpq fallback)" >&2
      fallback=1
    fi
  done

  cache_up=1
  if ! start_proxy "$budget"; then
    cache_up=0
    unavailable+=("$(fmt_bytes "$budget")")
  fi
  fills_up=0
  if [ "$cache_up" = 1 ]; then
    for workload in large small through; do
      out=$(measure pgcache "$workload" -)
      res["pgcache/$workload"]=$(sed -n 1p <<< "$out")
      evidence["pgcache/$workload"]=$(evidence_of "$out")
    done
    # cache reset: one read per workload is a miss, timed
    docker restart "$proxy" >/dev/null
    if wait_proxy; then
      fills_up=1
      for workload in large small; do
        out=$(measure pgcache "$workload" - --cold)
        res["pgcache/fill-$workload"]=$(sed -n 1p <<< "$out")
      done
    else
      echo "  (the cache did not come back at this budget: ${unavailable_reason})"
    fi
    [ "$keep" = 1 ] || docker rm -f "$proxy" >/dev/null 2>&1 || true
  fi

  for workload in large small through; do
    read -r med lo hi wmiss rmiss wreads wms count idsum psum ssum \
      <<< "${res["fastpostgres/$workload"]}"
    fpg_med=$med
    fpg_check="$count $idsum $psum $ssum"

    if [ "$cache_up" = 1 ]; then
      read -r med lo hi wmiss rmiss wreads wms count idsum psum ssum \
        <<< "${res["pgcache/$workload"]}"
      pgcache_med=$med
      if [ "$fpg_check" != "$count $idsum $psum $ssum" ]; then
        echo "MISMATCH $workload: $fpg_check vs $count $idsum $psum $ssum (pgcache)"
        mismatch=1
      fi
      if [ "$workload" = through ]; then
        cache_note="forwarded (views are never cached): $wmiss warm miss, $rmiss rep miss"
      else
        cache_note="warm $wreads reads/${wms} ms to a stable hit ($wmiss miss), $reps reps ($rmiss miss)"
      fi
      if [ "$workload" = small ]; then
        pg_cell=$(ms_of "$pgcache_med")
      else
        pg_cell=$(fmt_rows "$rows" "$pgcache_med")
      fi
      pg_delta=$(vs_fpg "$fpg_med" "$pgcache_med")
    else
      pg_cell="-"
      pg_delta="-"
      cache_note="the cache does not run at this budget (${unavailable_reason})"
    fi

    if [ "$workload" = small ]; then
      fpg_cell=$(ms_of "$fpg_med")
    else
      fpg_cell=$(fmt_rows "$rows" "$fpg_med")
    fi
    printf '  %-8s fastpostgres %-9s pgcache %-9s  pgcache/fastpostgres %-9s %s\n' \
      "$workload" "$fpg_cell" "$pg_cell" "$pg_delta" "$cache_note"
    printf '  %-8s ring  %s\n' "" "${evidence["fastpostgres/$workload"]}"
    if [ "$cache_up" = 1 ]; then
      printf '  %-8s cache %s\n' "" "${evidence["pgcache/$workload"]}"
    fi
  done

  if [ "$fills_up" = 1 ]; then
    read -r fill_large _ _ _ _ _ _ _ _ _ <<< "${res["pgcache/fill-large"]}"
    read -r fill_small _ _ _ _ _ _ _ _ _ <<< "${res["pgcache/fill-small"]}"
    read -r fpg_large _ _ _ _ _ _ _ _ _ <<< "${res["fastpostgres/large"]}"
    read -r fpg_small _ _ _ _ _ _ _ _ _ <<< "${res["fastpostgres/small"]}"
    printf '  %-8s pgcache %-10s fastpostgres %s   (one read, cache reset)\n' \
      "fill-large" "$(ms_of "$fill_large")" "$(ms_of "$fpg_large")"
    printf '  %-8s pgcache %-10s fastpostgres %s   (one read, cache reset)\n' \
      "fill-small" "$(ms_of "$fill_small")" "$(ms_of "$fpg_small")"
  elif [ "$cache_up" = 1 ]; then
    printf '  %-8s no fill timed: the cache did not restart at this budget (%s)\n' \
      "fill" "$unavailable_reason"
  else
    printf '  %-8s unavailable: the proxy container was %s at %s\n' \
      "cache" "$unavailable_reason" "$(fmt_bytes "$budget")"
  fi
  unset res evidence
done

echo ""
echo "# the same output across players: $([ "$mismatch" = 0 ] && echo ok || echo MISMATCH)"
if [ ${#unavailable[@]} -gt 0 ]; then
  echo "# budgets the cache could not run at: ${unavailable[*]} (${unavailable_reason})"
fi
if [ "$fallback" = 1 ]; then
  echo "# at least one ring row did not take the ring: its timing is not the ring's"
fi
exit "$((mismatch + fallback))"
