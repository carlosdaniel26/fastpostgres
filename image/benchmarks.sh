#!/usr/bin/env bash
# Runs inside the dev image, never on the host: the justfile's `bench` recipe
# hands this to the container as its command.
#
# Run the SQLAlchemy driver benchmark; tools/charts.py parses the log it prints.
#
#   benchmarks.sh [nrows ...]    row counts default to the benchmark's own
set -euo pipefail

python3 /src/benchmark/sqlalchemy_bench.py "$@"
