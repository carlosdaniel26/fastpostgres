#!/usr/bin/env bash
set -euo pipefail

# Runs demo/sqlalchemy_app.py twice against the same database -- once on
# psycopg2, once on fastpostgres -- to show the swap is a URL change. Both runs
# must print the same rows/checksum.
#
# Usage: scripts/sqlalchemy-demo-docker.sh [nrows]   (default: 5000)

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="fastpostgres-dev"
N="${1:-5000}"

echo "==> building image (postgres:18 + extension + Python client + SQLAlchemy)"
docker build -t "$IMG" -f "$ROOT/scripts/Dockerfile" "$ROOT"

echo "==> comparing drivers ($N rows)"
docker run --rm --shm-size=1g -e N="$N" "$IMG" bash -c '
  run() { echo; echo "### $1"; python3 /src/demo/sqlalchemy_app.py "$2" "$N"; }
  run "psycopg2  (the existing driver)" "postgresql+psycopg2://postgres@/postgres?host=/tmp"
  run "fastpostgres  (one URL change)"  "postgresql+fastpostgres://postgres@/postgres?host=/tmp"
'
