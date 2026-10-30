#!/usr/bin/env bash
set -euo pipefail

# Runs the SQLAlchemy benchmark (benchmark/sqlalchemy_bench.py) inside a
# postgres:18 + Python container: the same ORM/Core reads on psycopg2 and on
# fastpostgres.
#
# Emits an "### ENV" block before the numbers so the environment travels with
# the measurements.
#
# Usage: scripts/bench-sqlalchemy-docker.sh [nrows ...]  (default: 100000 1000000 10000000)

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="fastpostgres-sqlalchemy-bench"
SIZES="${*:-100000 1000000 10000000}"

echo "==> building image (postgres:18 + extension + Python client + SQLAlchemy)"
docker build -t "$IMG" -f - "$ROOT" <<'DOCKERFILE'
FROM postgres:18
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential postgresql-server-dev-18 python3 python3-dev python3-setuptools \
      python3-sqlalchemy python3-psycopg2 \
 && rm -rf /var/lib/apt/lists/*
COPY pg-extension /src/pg-extension
COPY python-extension /src/python-extension
COPY benchmark /src/benchmark
WORKDIR /src/pg-extension
RUN make clean && make && make install
WORKDIR /src/python-extension
RUN python3 setup.py build_ext --inplace
DOCKERFILE

echo "==> running the benchmark (rows: $SIZES)"
docker run --rm --shm-size=2g -e SIZES="$SIZES" -i "$IMG" bash -s <<'SCRIPT'
set -euo pipefail
export PGDATA=/tmp/pgdata
rm -rf "$PGDATA"; mkdir -p "$PGDATA"; chown -R postgres:postgres "$PGDATA"
gosu postgres initdb -D "$PGDATA" >/dev/null
gosu postgres pg_ctl -D "$PGDATA" -l /tmp/pg.log \
  -o "-c listen_addresses='127.0.0.1' -c unix_socket_directories='/tmp'" -w start >/dev/null
gosu postgres psql -h /tmp -d postgres -v ON_ERROR_STOP=1 \
  -c "CREATE EXTENSION fastpostgres;" >/dev/null

echo "### ENV"
ghz=$(awk -F': ' '/cpu MHz/ {v=$2+0; if (v>m) m=v} END {if (m>0) printf "%.1f", m/1000}' /proc/cpuinfo)
echo "ghz: $ghz"
cores=$(awk -F': ' '/^core id/ {print $2}' /proc/cpuinfo | sort -u | wc -l)
if [ "$cores" -eq 0 ]; then cores=$(nproc); fi
echo "cores: $cores"
echo "postgres: $(psql --version | awk '{print $3}')"
echo "python: $(python3 --version | awk '{print $2}')"
echo "sqlalchemy: $(python3 -c 'import sqlalchemy; print(sqlalchemy.__version__)')"
echo "### END ENV"

cd /src/python-extension
gosu postgres env PYTHONPATH=src python3 /src/benchmark/sqlalchemy_bench.py $SIZES

gosu postgres pg_ctl -D "$PGDATA" -w stop >/dev/null
SCRIPT
