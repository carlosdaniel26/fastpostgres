#!/usr/bin/env bash
set -euo pipefail

# Runs the Node benchmark (benchmark/bench.ts) inside a postgres:18 + Node
# container: the same data across tcp / unix socket / shared memory, read and
# write.
#
# Emits an "### ENV" block before the numbers so the environment (CPU, memory,
# kernel, versions, /dev/shm size) travels with the measurements; plot_bench.py
# renders it on the chart.
#
# Usage: scripts/bench-node-docker.sh [nrows ...]   (default: 100000 1000000 10000000)

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="fastpostgres-node"
SIZES="${*:-100000 1000000 10000000}"

echo "==> building image (postgres:18 + dev headers + extension + Node addon)"
docker build -t "$IMG" -f "$ROOT/scripts/Dockerfile.node" "$ROOT"

echo "==> running the benchmark (rows: $SIZES)"
docker run --rm --shm-size=3g -e SIZES="$SIZES" -i "$IMG" bash -s <<'SCRIPT'
set -euo pipefail
export PGDATA=/tmp/pgdata
rm -rf "$PGDATA"
mkdir -p "$PGDATA"
chown -R postgres:postgres /tmp/pgdata

# listen on 127.0.0.1 (tcp) and /tmp (unix socket), default postgres settings
gosu postgres initdb -D "$PGDATA" >/dev/null
gosu postgres pg_ctl -D "$PGDATA" -l /tmp/pg.log \
  -o "-c listen_addresses='127.0.0.1' -c unix_socket_directories='/tmp'" -w start >/dev/null

gosu postgres psql -h /tmp -d postgres -v ON_ERROR_STOP=1 \
  -c "CREATE EXTENSION fastpostgres;" >/dev/null

# environment the numbers are measured in (read back by scripts/plot_bench.py)
echo "### ENV"
ghz=""
if command -v lscpu >/dev/null 2>&1; then
  ghz=$(lscpu | awk -F: '/CPU max MHz/ {printf "%.1f", $2 / 1000; exit}')
fi
if [ -z "$ghz" ]; then
  ghz=$(awk -F': ' '/cpu MHz/ {v=$2+0; if (v>m) m=v} END {if (m>0) printf "%.1f", m/1000}' /proc/cpuinfo)
fi
echo "ghz: $ghz"
cores=$(awk -F': ' '/^core id/ {print $2}' /proc/cpuinfo | sort -u | wc -l)
if [ "$cores" -eq 0 ]; then cores=$(nproc); fi
echo "cores: $cores"
echo "postgres: $(psql --version | awk '{print $3}')"
echo "node: $(node --version)"
echo "shm: $(df -h /dev/shm | awk 'NR==2 {print $2}')"
echo "### END ENV"

cd /src/node-extension
gosu postgres node --import tsx --max-old-space-size=8192 ../benchmark/bench.ts $SIZES

gosu postgres pg_ctl -D "$PGDATA" -w stop >/dev/null
SCRIPT
