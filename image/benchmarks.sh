#!/usr/bin/env bash
# Runs inside the dev image, never on the host: the justfile's `bench` recipe
# hands this to the container as its command.
#
# Run both benchmarks in one container, so their logs share one environment
# block (tools/charts.py parses it back out).
#
#   benchmarks.sh [nrows ...]    row counts default to each benchmark's own
set -euo pipefail

python3 /src/benchmark/bench.py "$@"
python3 /src/benchmark/sqlalchemy_bench.py "$@"
