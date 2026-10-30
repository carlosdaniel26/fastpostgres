#!/usr/bin/env bash
set -euo pipefail

# Runs demo/sqlalchemy_app.py twice against the same PostgreSQL — once on the
# standard psycopg2 driver, once on fastpostgres — to show the swap is a URL
# change. Both runs must print the same rows/checksum.
#
# Usage: scripts/sqlalchemy-demo-docker.sh [nrows]   (default: 5000)

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="fastpostgres-sqlalchemy"
N="${1:-5000}"

echo "==> building image (postgres:18 + extension + Python client + SQLAlchemy)"
docker build -t "$IMG" -f - "$ROOT" <<'DOCKERFILE'
FROM postgres:18
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential postgresql-server-dev-18 python3 python3-dev python3-setuptools \
      python3-sqlalchemy python3-psycopg2 \
 && rm -rf /var/lib/apt/lists/*
COPY pg-extension /src/pg-extension
COPY python-extension /src/python-extension
COPY demo /src/demo
WORKDIR /src/pg-extension
RUN make clean && make && make install
WORKDIR /src/python-extension
RUN python3 setup.py build_ext --inplace
DOCKERFILE

echo "==> comparing drivers ($N rows)"
docker run --rm --shm-size=1g -e N="$N" -i "$IMG" bash -s <<'SCRIPT'
set -euo pipefail
export PGDATA=/tmp/pgdata
rm -rf "$PGDATA"; mkdir -p "$PGDATA"; chown -R postgres:postgres "$PGDATA"
gosu postgres initdb -D "$PGDATA" >/dev/null
gosu postgres pg_ctl -D "$PGDATA" -l /tmp/pg.log \
  -o "-c listen_addresses='127.0.0.1' -c unix_socket_directories='/tmp'" -w start >/dev/null
gosu postgres psql -h /tmp -d postgres -v ON_ERROR_STOP=1 \
  -c "CREATE EXTENSION fastpostgres;" >/dev/null

run() {
  echo
  echo "### $1"
  gosu postgres env PYTHONPATH=/src/python-extension/src \
    python3 /src/demo/sqlalchemy_app.py "$2" "$N"
}

run "psycopg2  (the existing driver)"       "postgresql+psycopg2://postgres@/postgres?host=/tmp"
run "fastpostgres  (one URL change)"        "postgresql+fastpostgres://postgres@/postgres?host=/tmp"

gosu postgres pg_ctl -D "$PGDATA" -w stop >/dev/null
SCRIPT
