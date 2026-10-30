#!/usr/bin/env bash
set -euo pipefail

# Runs the transport benchmark (benchmark/bench.py): the same data across tcp /
# unix socket / shared memory, read and write. The "### ENV" block it prints
# travels with the numbers (scripts/run_bench.py reads it back).
#
# Usage: scripts/bench-docker.sh [nrows ...]   (default: the benchmark's own)

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="fastpostgres-dev"

echo "==> building image (postgres:18 + extension + Python client)"
docker build -t "$IMG" -f "$ROOT/scripts/Dockerfile" "$ROOT"

echo "==> running the transport benchmark${*:+ (rows: $*)}"
docker run --rm --shm-size=3g -e SIZES="$*" "$IMG" \
  bash -c 'bench-env.sh && python3 /src/benchmark/bench.py $SIZES'
