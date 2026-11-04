#!/usr/bin/env bash
# Runs inside the dev image, never on the host: the justfile's `bench` recipe
# hands this to the container as its command.
#
# Run the fast-postgres vs ADBC + Arrow read benchmark; bench/charts/adbc.py parses
# the log it prints.
#
#   benchmarks.sh [nrows ...]    row counts default to the benchmark's own
#
# One process per size, so each size is measured from a cold process.
set -euo pipefail

if [ "$#" -eq 0 ]; then
    set -- 100000 1000000 10000000
fi

for n in "$@"; do
    python3 /src/bench/rivals/adbc-arrow/sqlalchemy_bench.py "$n"
done
