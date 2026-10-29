#!/usr/bin/env bash
set -euo pipefail

# Profiles the nativepostgress path with `perf` (Linux process tracer).
# Captures BOTH the Node client and the postgres backend (the extension), then
# prints the hottest functions.
#
# Usage: scripts/profile.sh [nrows]   (default 1000000)

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="nativepostgress-node"
NROWS="${1:-1000000}"

echo "==> building profile image (postgres:18 + dev headers + Node addon + perf)"
docker build -t "$IMG" -f - "$ROOT" <<'DOCKERFILE'
FROM postgres:18
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential postgresql-server-dev-18 python3 nodejs npm linux-perf \
 && rm -rf /var/lib/apt/lists/*
COPY pg-extension /src/pg-extension
COPY node-extension /src/node-extension
COPY benchmark /src/benchmark
WORKDIR /src/pg-extension
RUN make clean && make && make install
WORKDIR /src/node-extension
RUN npm install --no-audit --no-fund && npm run build
DOCKERFILE

echo "==> perf record across the benchmark ($NROWS rows)"
docker run --rm --privileged --shm-size=2g -i "$IMG" bash -s <<SCRIPT
set -euo pipefail
export PGDATA=/tmp/pgdata
rm -rf "\$PGDATA"; mkdir -p "\$PGDATA"; chown -R postgres:postgres /tmp/pgdata
gosu postgres initdb -D "\$PGDATA" >/dev/null 2>&1
gosu postgres pg_ctl -D "\$PGDATA" -l /tmp/pg.log \
  -o "-c listen_addresses='127.0.0.1' -c unix_socket_directories='/tmp'" -w start >/dev/null
gosu postgres psql -h /tmp -d postgres -v ON_ERROR_STOP=1 -c "CREATE EXTENSION nativepostgress;" >/dev/null

# system-wide profile while the benchmark runs (client + postgres backend)
perf record -g -a -o /tmp/perf.data -- gosu postgres \
  sh -c "cd /src/node-extension && exec node --import tsx ../benchmark/bench.ts $NROWS" \
  >/dev/null 2>&1 || true
gosu postgres pg_ctl -D "\$PGDATA" -w stop >/dev/null

echo "=== raw top symbols (self time) ==="
perf report -i /tmp/perf.data --stdio --no-children 2>/dev/null | head -60

echo
echo "=== nativepostgress + serialization symbols ==="
perf report -i /tmp/perf.data --stdio --no-children 2>/dev/null \
  | grep -iE "np_|columnar|text_cell|col_datum|multi_insert|heap_|OidOutput|AllocSet|memcpy" \
  | head -40
SCRIPT
