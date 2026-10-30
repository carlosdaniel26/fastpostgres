#!/usr/bin/env bash
set -euo pipefail

# Runs the SQLAlchemy benchmark (benchmark/sqlalchemy_bench.py): the same
# ORM/Core reads on psycopg2 and on fastpostgres. The "### ENV" block it prints
# travels with the numbers (scripts/run_bench.py reads it back).
#
# Usage: scripts/bench-sqlalchemy-docker.sh [nrows ...]  (default: the benchmark's own)

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="fastpostgres-dev"

echo "==> building image (postgres:18 + extension + Python client + SQLAlchemy)"
docker build -t "$IMG" -f "$ROOT/scripts/Dockerfile" "$ROOT"

echo "==> running the SQLAlchemy benchmark${*:+ (rows: $*)}"
docker run --rm --shm-size=3g -e SIZES="$*" "$IMG" \
  bash -c 'bench-env.sh && python3 /src/benchmark/sqlalchemy_bench.py $SIZES'
