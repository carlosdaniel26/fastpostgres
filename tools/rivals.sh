#!/usr/bin/env bash
# The competitive comparisons, behind one command: what the ring does against
# ADBC + Arrow and against a result cache, at each side's defaults and under a
# fixed memory budget.
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
                 (docs/fastpostgres-vs-arrow.log, -mixed, -small)
  arrow-memory   the same reads with the ring's buffer fixed (--mib=N)
                 (docs/fastpostgres-vs-arrow-memory.log)
  cache          vs a CDC-maintained result cache, each side at its defaults
                 (docs/fastpostgres-vs-pgcache.log)
  cache-budget   vs the same cache under one memory budget (--budgets="...")
                 (docs/fastpostgres-vs-pgcache-memory.log)

Arguments after the target go to that target's recipe.
EOF
}

case "$target" in
  adbc)         just bench-adbc "$@" ;;
  arrow-memory) just arrow-memory-bench "$@" ;;
  cache)        just pgcache-bench "$@" ;;
  cache-budget) just pgcache-memory-bench "$@" ;;
  "" | list | help) usage ;;
  *)
    echo "rivals: unknown target '$target'" >&2
    usage >&2
    exit 2
    ;;
esac
