#!/usr/bin/env bash
set -euo pipefail

# Builds the extension and the Node addon in a postgres:18 + Node image, runs the
# SQL correctness suite, then `node --test` for the client. Postgres and Node run
# in the same container, so they share /dev/shm directly.
#
# Usage: scripts/test-pg-docker.sh

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="nativepostgress-node"

echo "==> building image (postgres:18 + dev headers + extension + Node addon)"
docker build -t "$IMG" -f "$ROOT/scripts/Dockerfile.node" "$ROOT"

echo "==> running the SQL suite and node --test as postgres"
docker run --rm --shm-size=2g -i "$IMG" bash -s <<'SCRIPT'
set -euo pipefail
export PGDATA=/tmp/pgdata
rm -rf "$PGDATA"
mkdir -p "$PGDATA"
chown -R postgres:postgres /tmp/pgdata

gosu postgres initdb -D "$PGDATA" >/dev/null
gosu postgres pg_ctl -D "$PGDATA" \
  -o "-c listen_addresses='' -c unix_socket_directories='/tmp'" -w start >/dev/null

echo "--- psql: pg-extension/test.sql ---"
gosu postgres psql -h /tmp -d postgres -v ON_ERROR_STOP=1 -f /src/pg-extension/test.sql

echo
echo "--- node --test (Node client) ---"
cd /src/node-extension
gosu postgres node --test

gosu postgres pg_ctl -D "$PGDATA" -w stop >/dev/null
SCRIPT
