#!/usr/bin/env bash
# fastpostgres against a result cache in front of the same server.
#
# Two containers, because PgCache is one:
#
#   fp-pgcache-origin  the dev image, PostgreSQL with the extension, started
#                      with wal_level=logical because PgCache subscribes to the
#                      origin's logical replication stream. The players run in
#                      it via `docker exec`, so the shm ring stays on one /dev/shm.
#   fp-pgcache         the PgCache image, joined to the origin's *network
#                      namespace*: it reaches the origin at 127.0.0.1:5432 and
#                      the players reach it at 127.0.0.1:6432, nothing published.
#                      Its embedded cache PostgreSQL binds 5433, so no collision.
#
# Fairness, per size: setup(n) builds the same table `just bench` reads; the
# rival (fastpostgres) is measured with the proxy *stopped*; the proxy then
# starts only after the table exists, so its cache (reset on every start) can
# only hold rows from this run; the pgcache player warms until the proxy serves a
# hit and reports what that cost; and one read after a proxy restart is timed as
# the cold fill.
#
# Usage: benchmarks/harness/pgcache_bench.sh [--quick] [--keep] [nrows ...]
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
image=${IMAGE:-fastpostgres-dev}
pgcache_image=${PGCACHE_IMAGE:-pgcache/pgcache}
shm=${SHM:-3g}
shape=${SHAPE:-bench}
origin=fp-pgcache-origin
proxy=fp-pgcache
port=6432
player=/src/benchmarks/rivals/pgcache/player.py
pg_opts="-c wal_level=logical -c max_replication_slots=10 -c max_wal_senders=10"
reps_max_rows=1000000

quick=0
keep=0
sizes=()
for arg in "$@"; do
  case "$arg" in
    --quick) quick=1 ;;
    --keep) keep=1 ;;
    *) sizes+=("$arg") ;;
  esac
done
if [ ${#sizes[@]} -eq 0 ]; then
  if [ "$quick" = 1 ]; then sizes=(1000); else sizes=(100000 1000000); fi
fi

reps_for() {
  if [ "$quick" = 1 ] || [ "$1" -gt "$reps_max_rows" ]; then echo 1; else echo 3; fi
}

# shellcheck disable=SC2329 # invoked by the EXIT trap
cleanup() {
  if [ "$keep" = 1 ]; then return 0; fi
  docker rm -f "$proxy" "$origin" >/dev/null 2>&1 || true
}
trap cleanup EXIT

# ---- the two containers --------------------------------------------------

docker build -q --target dev -t "$image" -f "$root/docker/Dockerfile" "$root" >/dev/null 2>&1
docker image inspect "$pgcache_image" >/dev/null 2>&1 || docker pull "$pgcache_image"

docker rm -f "$origin" "$proxy" >/dev/null 2>&1 || true
docker run -d --name "$origin" --shm-size="$shm" \
  -e "PG_OPTS=$pg_opts" -e "SHAPE=$shape" \
  "$image" sleep infinity >/dev/null
for _ in $(seq 60); do docker exec "$origin" pg_isready -h /tmp -q && break; sleep 1; done

probe="import urllib.request, psycopg
psycopg.connect('host=127.0.0.1 port=$port dbname=postgres user=postgres').close()
urllib.request.urlopen('http://127.0.0.1:9090/metrics', timeout=5).read()"

start_proxy() {
  docker rm -f "$proxy" >/dev/null 2>&1 || true
  docker run -d --name "$proxy" --network "container:$origin" \
    -e "UPSTREAM_URL=postgres://postgres@127.0.0.1:5432/postgres" \
    -e "LISTEN_PORT=$port" -e "METRICS_PORT=9090" -e PGCACHE_TELEMETRY=off \
    "$pgcache_image" >/dev/null
  wait_proxy
}

wait_proxy() {
  for _ in $(seq 120); do
    docker exec "$origin" python3 -c "$probe" 2>/dev/null && return 0
    sleep 1
  done
  echo "pgcache_bench: the proxy did not come up; its log:" >&2
  docker logs "$proxy" --tail 40 >&2 || true
  exit 1
}

measure() {  # player workload n reps [--cold]
  docker exec -u postgres -e "SHAPE=$shape" "$origin" python3 "$player" read "$@"
}

fmt_rows() { awk -v n="$1" -v ms="$2" 'BEGIN{printf "%.2fM", n / (ms / 1000) / 1e6}'; }
vs_fpg() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%+.1f%%", (b / a - 1) * 100}'; }
ms_of() { echo "$1" | awk '{printf "%.3f ms", $1}'; }

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
echo "shape: $shape"
echo "sizes: ${sizes[*]}"
echo "git: $(git -C "$root" describe --always --dirty 2>/dev/null || echo unknown)"
echo "### END ENV"

mismatch=0
declare -A seen
for n in "${sizes[@]}"; do
  reps=$(reps_for "$n")
  echo ""
  echo "# $n rows, $reps timed rep(s) per player, one process each"
  docker exec -u postgres -e "SHAPE=$shape" "$origin" python3 "$player" setup "$n"

  declare -A res
  seen=()  # the checksums are size-specific
  for workload in large small through; do
    res["fastpostgres/$workload"]=$(measure fastpostgres "$workload" "$n" "$reps")
  done

  start_proxy
  for workload in large small through; do
    res["pgcache/$workload"]=$(measure pgcache "$workload" "$n" "$reps")
  done
  # cache reset: one read per workload is a miss, timed
  docker restart "$proxy" >/dev/null
  wait_proxy
  res["pgcache/fill-large"]=$(measure pgcache large "$n" 1 --cold)
  res["pgcache/fill-small"]=$(measure pgcache small "$n" 1 --cold)
  [ "$keep" = 1 ] || docker rm -f "$proxy" >/dev/null 2>&1 || true

  for workload in large small through; do
    for player_name in fastpostgres pgcache; do
      read -r med _ _ wmiss rmiss wreads wms count idsum psum ssum \
        <<< "${res["$player_name/$workload"]}"
      # the same output: count and the sums over the columns, across players
      check="$count $idsum $psum $ssum"
      if [ -z "${seen[$workload]:-}" ]; then
        seen[$workload]="$check"
      elif [ "${seen[$workload]}" != "$check" ]; then
        echo "MISMATCH $workload at $n: ${seen[$workload]} vs $check ($player_name)"
        mismatch=1
      fi
      if [ "$player_name" = pgcache ]; then
        pgcache_med=$med
        if [ "$workload" = through ]; then
          cache_note="forwarded (views are never cached): $wmiss warm miss, $rmiss rep miss"
        else
          cache_note="warm $wreads reads/${wms} ms to a stable hit ($wmiss miss), $reps reps ($rmiss miss)"
        fi
      else
        fpg_med=$med
      fi
    done
    if [ "$workload" = small ]; then
      fpg_cell=$(ms_of "$fpg_med")
      pg_cell=$(ms_of "$pgcache_med")
    else
      fpg_cell=$(fmt_rows "$n" "$fpg_med")
      pg_cell=$(fmt_rows "$n" "$pgcache_med")
    fi
    printf '  %-8s fastpostgres %-9s pgcache %-9s  pgcache/fastpostgres %-9s %s\n' \
      "$workload" "$fpg_cell" "$pg_cell" \
      "$(vs_fpg "$fpg_med" "$pgcache_med")" "$cache_note"
  done
  read -r fill_large _ _ _ _ _ _ _ _ _ <<< "${res["pgcache/fill-large"]}"
  read -r fill_small _ _ _ _ _ _ _ _ _ <<< "${res["pgcache/fill-small"]}"
  read -r fpg_large _ _ _ _ _ _ _ _ _ <<< "${res["fastpostgres/large"]}"
  read -r fpg_small _ _ _ _ _ _ _ _ _ <<< "${res["fastpostgres/small"]}"
  printf '  %-8s pgcache %-10s fastpostgres %s   (one read, cache reset)\n' \
    "fill-large" "$(ms_of "$fill_large")" "$(ms_of "$fpg_large")"
  printf '  %-8s pgcache %-10s fastpostgres %s   (one read, cache reset)\n' \
    "fill-small" "$(ms_of "$fill_small")" "$(ms_of "$fpg_small")"
  unset res
done

echo ""
echo "# the same output across players: $([ "$mismatch" = 0 ] && echo ok || echo MISMATCH)"
exit "$mismatch"
