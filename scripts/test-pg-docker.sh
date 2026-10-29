#!/usr/bin/env bash
set -euo pipefail

# Builds the nativepostgress extension inside a postgres:18 container, runs the
# correctness tests (query + shared-memory round-trip + write round-trip), then
# reads /dev/shm/np_test from the host into JS typed arrays (shm-demo).
#
# Usage: scripts/test-pg-docker.sh

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXT="$ROOT/pg-extension"
IMG="nativepostgress-pg"

echo "==> building extension image (postgres:18 + dev headers + extension)"
docker build -t "$IMG" -f - "$EXT" <<'DOCKERFILE'
FROM postgres:18
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential postgresql-server-dev-18 \
 && rm -rf /var/lib/apt/lists/*
COPY . /src
WORKDIR /src
RUN make clean && make && make install
DOCKERFILE

rm -f /dev/shm/np_test 2>/dev/null || true

echo "==> running a real query + writing the result to shared memory (--ipc=host)"
docker run --rm --ipc=host -i "$IMG" bash -s <<'SCRIPT'
set -euo pipefail
export PGDATA=/tmp/pgdata
rm -rf "$PGDATA"
mkdir -p "$PGDATA"
chown -R postgres:postgres /tmp/pgdata

gosu postgres initdb -D "$PGDATA" >/dev/null
gosu postgres pg_ctl -D "$PGDATA" \
  -o "-c listen_addresses='' -c unix_socket_directories='/tmp'" -w start >/dev/null

gosu postgres psql -h /tmp -d postgres -v ON_ERROR_STOP=1 -f /src/test.sql

gosu postgres pg_ctl -D "$PGDATA" -w stop >/dev/null
echo "OK: postgres wrote /dev/shm/np_test"
SCRIPT

echo
echo "==> building the host-side reader (separate process, V8 side)"
cmake -S "$ROOT/v8-extension" -B "$ROOT/v8-extension/build" >/dev/null
cmake --build "$ROOT/v8-extension/build" --target shm-demo 2>&1 | tail -3

echo "==> reading /dev/shm/np_test from the host into JS typed arrays"
"$ROOT/v8-extension/build/shm-demo" np_test
