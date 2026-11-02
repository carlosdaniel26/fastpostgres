#!/usr/bin/env bash
# Render a committed log into its chart, one target per log.
set -euo pipefail

target=${1:-}
[ $# -gt 0 ] && shift || true

usage() {
  cat <<'EOF'
charts: one figure per log

  workloads      the realistic workload suite (benchmarks/results/fastpostgres-vs-psycopg.log)
  arrow          fastpostgres vs ADBC + Arrow (benchmarks/results/fastpostgres-vs-arrow.log)
  arrow-memory   the same reads under a fixed ring buffer
  cache          fastpostgres vs PgCache, defaults
  cache-budget   fastpostgres vs PgCache under one memory budget
  codec          the codec x transport matrix
  ab             before/after, from an improve-*.log pair (--log=...)

Arguments after the target go to that chart's script.
EOF
}

case "$target" in
  workloads)    exec benchmarks/charts/workloads.py "$@" ;;
  arrow)        exec benchmarks/charts/adbc.py "$@" ;;
  arrow-memory) exec benchmarks/charts/arrow_memory.py "$@" ;;
  cache)        exec benchmarks/charts/cache.py "$@" ;;
  cache-budget) exec benchmarks/charts/cache_budget.py "$@" ;;
  codec)        exec benchmarks/charts/codec.py "$@" ;;
  ab)           exec benchmarks/charts/ab.py "$@" ;;
  "" | list | help) usage ;;
  *)
    echo "charts: unknown target '$target'" >&2
    usage >&2
    exit 2
    ;;
esac
