#!/usr/bin/env bash
# Render a committed log into its chart, one target per log.
set -euo pipefail

target=${1:-}
[ $# -gt 0 ] && shift || true

usage() {
  cat <<'EOF'
charts: one figure per log

  arrow          fastpostgres vs ADBC + Arrow (bench/results/fastpostgres-vs-arrow.log)
  arrow-memory   the same reads under a fixed ring buffer
  cache          fastpostgres vs PgCache, defaults
  cache-budget   fastpostgres vs PgCache under one memory budget
  codec          the codec x transport matrix
  ab             before/after, from an improve-*.log pair (--log=...)
  styles         one figure through every candidate style (style lab)

Arguments after the target go to that chart's script.
EOF
}

case "$target" in
  arrow)        exec bench/charts/adbc.py "$@" ;;
  arrow-memory) exec bench/charts/arrow_memory.py "$@" ;;
  cache)        exec bench/charts/cache.py "$@" ;;
  cache-budget) exec bench/charts/cache_budget.py "$@" ;;
  codec)        exec bench/charts/codec.py "$@" ;;
  ab)           exec bench/charts/ab.py "$@" ;;
  styles)       exec bench/charts/style_lab.py "$@" ;;
  "" | list | help) usage ;;
  *)
    echo "charts: unknown target '$target'" >&2
    usage >&2
    exit 2
    ;;
esac
