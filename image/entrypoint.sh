#!/usr/bin/env bash
# The image's ENTRYPOINT (runs inside the image only): boot a throwaway
# PostgreSQL with the fastpostgres extension, print the environment the run
# happened in (so benchmark numbers travel with their context), run the command
# it is given as the postgres user, then stop the server.
#
# The command runs as postgres because the /dev/shm ring is created 0600 and
# must be shared with the backend worker.
set -euo pipefail

export PGDATA=/tmp/pgdata
rm -rf "$PGDATA"
mkdir -p "$PGDATA"
chown -R postgres:postgres "$PGDATA"

# listen on 127.0.0.1 (tcp) and /tmp (unix socket), default postgres settings.
# PG_OPTS adds server settings a single run needs (the PgCache comparison asks
# for wal_level=logical and the replication slots it implies); empty otherwise,
# so every other recipe starts the server it always did.
gosu postgres initdb -D "$PGDATA" >/dev/null
gosu postgres pg_ctl -D "$PGDATA" -l /tmp/pg.log \
  -o "-c listen_addresses='127.0.0.1' -c unix_socket_directories='/tmp' ${PG_OPTS:-}" -w start >/dev/null
gosu postgres psql -h /tmp -d postgres -v ON_ERROR_STOP=1 \
  -c "CREATE EXTENSION fastpostgres;" >/dev/null

cleanup() { gosu postgres pg_ctl -D "$PGDATA" -m immediate -w stop >/dev/null 2>&1 || true; }
trap cleanup EXIT

ghz=""
if command -v lscpu >/dev/null 2>&1; then
  ghz=$(lscpu | awk -F: '/CPU max MHz/ {printf "%.1f", $2 / 1000; exit}')
fi
if [ -z "$ghz" ]; then
  ghz=$(awk -F': ' '/cpu MHz/ {v=$2+0; if (v>m) m=v} END {if (m>0) printf "%.1f", m/1000}' /proc/cpuinfo)
fi
cores=$(awk -F': ' '/^core id/ {print $2}' /proc/cpuinfo | sort -u | wc -l)
if [ "$cores" -eq 0 ]; then cores=$(nproc); fi

cpu=$(awk -F': ' '/^model name/ {print $2; exit}' /proc/cpuinfo)
mem=$(awk '/MemTotal:/ {printf "%.0f GiB", $2 / 1048576}' /proc/meminfo)

echo "### ENV"
echo "cpu: ${cpu:-unknown}"
echo "mem: ${mem:-unknown}"
echo "ghz: $ghz"
echo "cores: $cores"
echo "postgres: $(psql --version | awk '{print $3}')"
echo "python: $(python3 --version | awk '{print $2}')"
python3 -c 'import sqlalchemy; print("sqlalchemy:", sqlalchemy.__version__)' 2>/dev/null || true
python3 -c 'import importlib.metadata as m; print("pyarrow:", m.version("pyarrow")); print("adbc:", m.version("adbc_driver_postgresql"))' 2>/dev/null || true
echo "shape: ${SHAPE:-bench}"
echo "git: ${BENCH_GIT_REV:-unknown}"
echo "shm: $(df -h /dev/shm | awk 'NR==2 {print $2}')"
echo "### END ENV"

gosu postgres "$@"
