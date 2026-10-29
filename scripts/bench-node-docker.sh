#!/usr/bin/env bash
set -euo pipefail

# Runs the Node benchmark (node-extension/bench/bench.js) inside a postgres:18 +
# Node container: the same data across tcp / unix socket / shared memory, read
# and write.
#
# Usage: scripts/bench-node-docker.sh [nrows ...]   (default: 100000 1000000 10000000)

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="nativepostgress-node"
SIZES="${*:-100000 1000000 10000000}"

echo "==> building image (postgres:18 + dev headers + extension + Node addon)"
docker build -t "$IMG" -f "$ROOT/scripts/Dockerfile.node" "$ROOT"

echo "==> running the benchmark (rows: $SIZES)"
docker run --rm --shm-size=3g -i "$IMG" bash -s <<SCRIPT
set -euo pipefail
export PGDATA=/tmp/pgdata
rm -rf "\$PGDATA"
mkdir -p "\$PGDATA"
chown -R postgres:postgres /tmp/pgdata

# listen on 127.0.0.1 (tcp) and /tmp (unix socket), default postgres settings
gosu postgres initdb -D "\$PGDATA" >/dev/null
gosu postgres pg_ctl -D "\$PGDATA" -l /tmp/pg.log \
  -o "-c listen_addresses='127.0.0.1' -c unix_socket_directories='/tmp'" -w start >/dev/null

gosu postgres psql -h /tmp -d postgres -v ON_ERROR_STOP=1 \
  -c "CREATE EXTENSION nativepostgress;" >/dev/null

gosu postgres node --max-old-space-size=8192 /src/benchmark/bench.js $SIZES

gosu postgres pg_ctl -D "\$PGDATA" -w stop >/dev/null
SCRIPT
