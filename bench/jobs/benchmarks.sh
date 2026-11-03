#!/usr/bin/env bash
# Runs inside the dev image, never on the host: the justfile's `bench` recipe
# hands this to the container as its command.
#
# Run the fast-postgres vs ADBC + Arrow read benchmark; bench/charts/adbc.py parses
# the log it prints.
#
#   benchmarks.sh [nrows ...]    row counts default to the benchmark's own
#
# One process per size: the client's object arena is append-only and process-
# scoped, so a single session that reads several sizes measures how the arena
# filled, not the driver. A fresh process per size gives each its own.
set -euo pipefail

if [ "$#" -eq 0 ]; then
    set -- 100000 1000000 10000000
fi

for n in "$@"; do
    python3 /src/bench/rivals/sqlalchemy_bench.py "$n"
done
