#!/usr/bin/env bash
# The environment a benchmark ran in, so the numbers travel with their context.
# scripts/run_bench.py parses this block back out of the log.
set -euo pipefail

ghz=""
if command -v lscpu >/dev/null 2>&1; then
  ghz=$(lscpu | awk -F: '/CPU max MHz/ {printf "%.1f", $2 / 1000; exit}')
fi
if [ -z "$ghz" ]; then
  ghz=$(awk -F': ' '/cpu MHz/ {v=$2+0; if (v>m) m=v} END {if (m>0) printf "%.1f", m/1000}' /proc/cpuinfo)
fi
cores=$(awk -F': ' '/^core id/ {print $2}' /proc/cpuinfo | sort -u | wc -l)
if [ "$cores" -eq 0 ]; then cores=$(nproc); fi

echo "### ENV"
echo "ghz: $ghz"
echo "cores: $cores"
echo "postgres: $(psql --version | awk '{print $3}')"
echo "python: $(python3 --version | awk '{print $2}')"
python3 -c 'import sqlalchemy; print("sqlalchemy:", sqlalchemy.__version__)' 2>/dev/null || true
echo "shm: $(df -h /dev/shm | awk 'NR==2 {print $2}')"
echo "### END ENV"
