#!/usr/bin/env bash
set -euo pipefail

# Builds the PostgreSQL extension and the Python client, then runs the client's
# tests inside one container (postgres:18 + Python): postgres and the client
# share /dev/shm, so the shared-memory ring needs no socket plumbing.
#
# Usage: scripts/py-docker.sh

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="fastpostgres-python"

echo "==> building image (postgres:18 + extension + Python client)"
docker build -t "$IMG" -f - "$ROOT" <<'DOCKERFILE'
FROM postgres:18
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential postgresql-server-dev-18 python3 python3-dev python3-setuptools \
 && rm -rf /var/lib/apt/lists/*
COPY pg-extension /src/pg-extension
COPY python-extension /src/python-extension
WORKDIR /src/pg-extension
RUN make clean && make && make install
WORKDIR /src/python-extension
RUN python3 setup.py build_ext --inplace
DOCKERFILE

echo "==> running the client tests"
docker run --rm --shm-size=3g -i "$IMG" bash -s <<'SCRIPT'
set -euo pipefail
export PGDATA=/tmp/pgdata
rm -rf "$PGDATA"; mkdir -p "$PGDATA"; chown -R postgres:postgres "$PGDATA"
gosu postgres initdb -D "$PGDATA" >/dev/null
gosu postgres pg_ctl -D "$PGDATA" -l /tmp/pg.log \
  -o "-c listen_addresses='127.0.0.1' -c unix_socket_directories='/tmp'" -w start >/dev/null
gosu postgres psql -h /tmp -d postgres -v ON_ERROR_STOP=1 \
  -c "CREATE EXTENSION fastpostgres;" >/dev/null

cd /src/python-extension
gosu postgres env PYTHONPATH=. DATABASE_URL="host=/tmp dbname=postgres user=postgres" \
  python3 -m unittest discover -s tests -v

gosu postgres pg_ctl -D "$PGDATA" -w stop >/dev/null
SCRIPT
