#!/usr/bin/env bash
set -euo pipefail

# Profiles the nativepostgress path with `perf` (Linux process tracer).
# Captures BOTH the client (bench) and the postgres backend (the extension),
# then prints the hottest functions.
#
# Usage: scripts/profile.sh [nrows]   (default 1000000)

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXT="$ROOT/pg-extension"
IMG="nativepostgress-bench"
NROWS="${1:-1000000}"

echo "==> building profile image (postgres:18 + dev headers + libpq + perf)"
docker build -t "$IMG" -f - "$EXT" <<'DOCKERFILE'
FROM postgres:18
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential postgresql-server-dev-18 libpq-dev linux-perf \
 && rm -rf /var/lib/apt/lists/*
COPY . /src
WORKDIR /src
RUN make clean && make && make install \
 && gcc -O2 -g -o bench bench.c -I. -I"$(pg_config --includedir)" -lpq
DOCKERFILE

echo "==> perf record across the benchmark ($NROWS rows)"
docker run --rm --privileged --ipc=host -i "$IMG" bash -s <<SCRIPT
set -euo pipefail
export PGDATA=/tmp/pgdata
rm -rf "\$PGDATA"; mkdir -p "\$PGDATA"; chown -R postgres:postgres /tmp/pgdata
gosu postgres initdb -D "\$PGDATA" >/dev/null 2>&1
gosu postgres pg_ctl -D "\$PGDATA" -l /tmp/pg.log \
  -o "-c listen_addresses='127.0.0.1' -c unix_socket_directories='/tmp'" -w start >/dev/null
gosu postgres psql -h /tmp -d postgres -v ON_ERROR_STOP=1 -c "CREATE EXTENSION nativepostgress;" >/dev/null

# system-wide profile while the benchmark runs (client + postgres backend)
perf record -g -a -o /tmp/perf.data -- /src/bench $NROWS >/dev/null 2>&1 || true
gosu postgres pg_ctl -D "\$PGDATA" -w stop >/dev/null

echo "=== raw top symbols (self time) ==="
perf report -i /tmp/perf.data --stdio --no-children 2>/dev/null | head -60

echo
echo "=== nativepostgress + serialization symbols ==="
perf report -i /tmp/perf.data --stdio --no-children 2>/dev/null \
  | grep -iE "nativepostgress|fill_columnar|compute_sizes|col_datum|multi_insert|heap_|Sprintf|AllocSet|text_|memcpy" \
  | head -40
SCRIPT
