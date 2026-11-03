#!/usr/bin/env bash
# Render a committed log into its chart, one target per log.
set -euo pipefail

target=${1:-}
[ $# -gt 0 ] && shift || true

usage() {
  cat <<'EOF'
charts: one figure per log

  workloads      the realistic workload suite (bench/results/fastpostgres-vs-psycopg.log)
  arrow          fastpostgres vs ADBC + Arrow (bench/results/fastpostgres-vs-arrow.log)
  arrow-transport  the same read per ring engine, from its record
                 (bench/results/arrow-transport.log)
  arrow-memory   the same reads under a fixed ring buffer
  cache          fastpostgres vs PgCache, defaults
  cache-budget   fastpostgres vs PgCache under one memory budget
  codec          the codec x transport matrix
  transport      the two doorbells: hops/s, us per hop, us of CPU per hop
  transport-e2e  the same two, end to end: delivered rows/s, p50, CPU per row
  transport-summary  both records in one figure: the doorbell, the read, the bill
  ab             before/after, from an improve-*.log pair (--log=...)

Arguments after the target go to that chart's script.
EOF
}

case "$target" in
  workloads)    exec bench/charts/workloads.py "$@" ;;
  arrow)        exec bench/charts/adbc.py "$@" ;;
  arrow-transport) exec bench/charts/arrow_transport.py "$@" ;;
  arrow-memory) exec bench/charts/arrow_memory.py "$@" ;;
  cache)        exec bench/charts/cache.py "$@" ;;
  cache-budget) exec bench/charts/cache_budget.py "$@" ;;
  codec)        exec bench/charts/codec.py "$@" ;;
  transport)    exec bench/charts/transport.py "$@" ;;
  transport-e2e) exec bench/charts/transport_e2e.py "$@" ;;
  transport-summary) exec bench/charts/transport_summary.py "$@" ;;
  ab)           exec bench/charts/ab.py "$@" ;;
  "" | list | help) usage ;;
  *)
    echo "charts: unknown target '$target'" >&2
    usage >&2
    exit 2
    ;;
esac
