#!/usr/bin/env bash
# The competitive comparisons, behind one command: what the ring does against
# ADBC + Arrow, at its defaults and with every connection it can use.
#
# Each target is the recipe that was always there, unchanged; this only routes.
# The records are the committed logs the README quotes.
set -euo pipefail

target=${1:-}
[ $# -gt 0 ] && shift || true

usage() {
  cat <<'EOF'
rivals: the competitive benchmarks

  adbc           fastpostgres vs ADBC + Arrow over the same rows
                 (bench/results/fastpostgres-vs-arrow.log, -mixed, -small)
  arrow-memory   the same reads with the ring's buffer fixed (--mib=N)
                 (bench/results/fastpostgres-vs-arrow-memory.log)

Arguments after the target go to that target's recipe.
EOF
}

case "$target" in
  clickbench)   just clickbench "$@" ;;
  adbc)         just bench-adbc "$@" ;;
  arrow-memory) just arrow-memory-bench "$@" ;;
  "" | list | help) usage ;;
  *)
    echo "rivals: unknown target '$target'" >&2
    usage >&2
    exit 2
    ;;
esac
