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
  codec          the codec x transport matrix
  ab             before/after, from an improve-*.log pair (--log=...)

Arguments after the target go to that chart's script.
EOF
}

case "$target" in
  arrow)        exec bench/charts/adbc.py "$@" ;;
  arrow-memory) exec bench/charts/arrow_memory.py "$@" ;;
  "" | list | help) usage ;;
  *)
    echo "charts: unknown target '$target'" >&2
    usage >&2
    exit 2
    ;;
esac
