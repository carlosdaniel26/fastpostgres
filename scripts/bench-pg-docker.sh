#!/usr/bin/env bash
set -euo pipefail

# Benchmarks the SAME query + data across three transports, for read and write:
#   tcp / unix socket / shared memory (native columnar).
#
# Usage: scripts/bench-pg-docker.sh [nrows ...]   (default: 100000 1000000 10000000)

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXT="$ROOT/pg-extension"
IMG="nativepostgress-bench"
SIZES="${*:-100000 1000000 10000000 100000000}"

echo "==> building benchmark image (postgres:18 + dev headers + libpq + bench)"
docker build -t "$IMG" -f - "$EXT" <<'DOCKERFILE'
FROM postgres:18
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential postgresql-server-dev-18 libpq-dev \
 && rm -rf /var/lib/apt/lists/*
COPY . /src
WORKDIR /src
RUN make clean && make && make install \
 && gcc -O2 -o bench bench.c -I. -I"$(pg_config --includedir)" -lpq
DOCKERFILE

echo "==> running the benchmark (rows: $SIZES)"
docker run --rm --ipc=host -i "$IMG" bash -s <<SCRIPT
set -euo pipefail
export PGDATA=/tmp/pgdata
rm -rf "\$PGDATA"
mkdir -p "\$PGDATA"
chown -R postgres:postgres /tmp/pgdata

# listen on both 127.0.0.1 (tcp) and /tmp (unix socket)
gosu postgres initdb -D "\$PGDATA" >/dev/null 2>&1
gosu postgres pg_ctl -D "\$PGDATA" -l /tmp/pg.log \
  -o "-c listen_addresses='127.0.0.1' -c unix_socket_directories='/tmp'" -w start >/dev/null

gosu postgres psql -h /tmp -d postgres -v ON_ERROR_STOP=1 \
  -c "CREATE EXTENSION nativepostgress;" >/dev/null

# bench runs as a client; the extension writes /dev/shm (shared via --ipc=host)
gosu postgres /src/bench $SIZES

gosu postgres pg_ctl -D "\$PGDATA" -w stop >/dev/null
SCRIPT
