#!/usr/bin/env bash
# Render a committed log into its chart, one target per log.
set -euo pipefail

target=${1:-}
[ $# -gt 0 ] && shift || true

usage() {
  cat <<'EOF'
charts: one figure per log

  workloads      the realistic workload suite (docs/fastpostgres-vs-psycopg.log)
  arrow          fastpostgres vs ADBC + Arrow (docs/fastpostgres-vs-arrow.log)
  arrow-memory   the same reads under a fixed ring buffer
  cache          fastpostgres vs PgCache, defaults
  cache-budget   fastpostgres vs PgCache under one memory budget
  codec          the codec x transport matrix
  ab             before/after, from an improve-*.log pair (--log=...)

Arguments after the target go to that chart's script.
EOF
}

case "$target" in
  workloads)    exec tools/workloads_charts.py "$@" ;;
  arrow)        exec tools/charts.py "$@" ;;
  arrow-memory) exec tools/arrow_memory_charts.py "$@" ;;
  cache)        exec tools/pgcache_charts.py "$@" ;;
  cache-budget) exec tools/pgcache_memory_charts.py "$@" ;;
  codec)        exec tools/codec_charts.py "$@" ;;
  ab)           exec tools/ab_charts.py "$@" ;;
  "" | list | help) usage ;;
  *)
    echo "charts: unknown target '$target'" >&2
    usage >&2
    exit 2
    ;;
esac
