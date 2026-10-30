#!/usr/bin/env bash
set -euo pipefail

# Builds the image (extension + Python client) and runs the client's tests
# against a throwaway PostgreSQL inside it: postgres and the client share
# /dev/shm, so the ring needs no socket plumbing.
#
# Usage: scripts/py-docker.sh

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="fastpostgres-dev"

echo "==> building image (postgres:18 + extension + Python client)"
docker build -t "$IMG" -f "$ROOT/scripts/Dockerfile" "$ROOT"

echo "==> running the client tests"
docker run --rm --shm-size=3g "$IMG" python3 -m unittest discover -s tests -v
