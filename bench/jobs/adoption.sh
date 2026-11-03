#!/usr/bin/env bash
# The adopted suite in the dev image: MagicStack's pgbench over its Python
# roster, with fastpostgres as one more cell in that roster.
#
# The suite is vendored verbatim under /src/bench/adoption/magicstack-pgbench/
# (PIN holds the commit it came from; the one addition beside it is the
# pgbench_fastpostgres symlink, in the suite's own idiom for its runners);
# /src/bench/adoption/run.py appends our cell to its roster and hands it the
# arguments this script was given; the cell itself is
# /src/bench/adoption/players/pgbench_fastpostgres.
#
# Everything about the measurement - warmup, the timed loop, the latency buckets,
# the JSON, one process per cell - is the suite's. This script adds the record's
# frame: the machine, the versions, the pin, the caveats, then the suite's output.
#
# The suite's Python wrapper resolves its own symlink against the working
# directory, so this script runs from the suite's root, as its README does.
#
# players/sitecustomize.py goes on PYTHONPATH for the whole run: the suite's 2023
# runner expects `asyncio.get_event_loop()` to create a loop (uvloop 0.21+ raises
# instead), and the shim restores that one call so its async cells still run on
# uvloop. It is scoped to this class and changes nothing the suite measures.
#
# Usage: adoption.sh [--quick] [--duration=N] [--warmup-time=N] [--concurrency=N]
#                    [impl ...]
#
#   --quick          warmup 1s, duration 3s, for a smoke run
#   impl             any of the suite's Python roster names, or
#                    fastpostgres-dbapi; default: all four
set -euo pipefail

suite=/src/bench/adoption/magicstack-pgbench
runner=/src/bench/adoption/run.py

quick=0
duration=10
warmup=3
concurrency=10
impls=()
for arg in "$@"; do
  case "$arg" in
    --quick) quick=1 ;;
    --duration=*) duration=${arg#*=} ;;
    --warmup-time=*) warmup=${arg#*=} ;;
    --concurrency=*) concurrency=${arg#*=} ;;
    -h | --help)
      sed -n '/^# Usage:/,/^#   --quick/p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    --*)
      echo "adoption: unknown flag '$arg' (see --help)" >&2
      exit 2
      ;;
    *) impls+=("$arg") ;;
  esac
done
if [ "$quick" = 1 ]; then
  [ "$warmup" = 3 ] && warmup=1
  [ "$duration" = 10 ] && duration=3
fi
if [ ${#impls[@]} -eq 0 ]; then
  impls=(python-aiopg python-asyncpg python-psycopg3-async fastpostgres-dbapi)
fi

echo "### ADOPTION"
echo "cpu: $(awk -F': ' '/^model name/ {print $2; exit}' /proc/cpuinfo)"
awk '/MemTotal:/ {printf "mem: %.0f GiB\n", $2 / 1048576}' /proc/meminfo
echo "cores: $(nproc)"
echo "postgres: $(psql --version | awk '{print $3}')"
echo "suite: MagicStack/pgbench at $(cat "$suite/PIN"), vendored verbatim"
echo "cells: ${impls[*]}"
echo "warmup: ${warmup}s; duration: ${duration}s per cell; concurrency: $concurrency"
echo "git: ${BENCH_GIT_REV:-unknown}"
python3 - <<'PY'
import importlib.metadata as m
import sys

print("python:", sys.version.split()[0])
for name in ("asyncpg", "aiopg", "psycopg2-binary", "psycopg", "py-postgresql", "uvloop", "distro"):
    try:
        print(f"{name}: {m.version(name)}")
    except m.PackageNotFoundError:
        print(f"{name}: absent")
PY
echo "# The suite's own protocol is the measurement: each cell runs in a process of"
echo "# its own, at this concurrency, for this duration, and reports queries, rows"
echo "# and a latency histogram in hundredths of a millisecond. Its 'rows' column is"
echo "# the output check this class can make: the same query must return the same row"
echo "# count in every cell the suite supports."
echo "# A cell the driver cannot run (COPY) exits 3, the suite's own contract; the"
echo "# record shows the cell missing instead of a number that hides it."
echo "### END ADOPTION"
echo

cd "$suite"
export PYTHONPATH="/src/bench/adoption/players:${PYTHONPATH:-}"
python3 "$runner" \
  --pghost=127.0.0.1 --pgport=5432 --pguser=postgres \
  --warmup-time="$warmup" --duration="$duration" --concurrency="$concurrency" \
  "${impls[@]}"
