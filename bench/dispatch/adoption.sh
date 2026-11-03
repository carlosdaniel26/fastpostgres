#!/usr/bin/env bash
# The adopted third-party suite, behind one command.
#
# One target today: MagicStack's pgbench, whose Python roster fastpostgres joins
# as one more cell. The record is bench/results/adoption-magicstack-pgbench.log,
# and the write-up - what the suite tests, what its cells found, and where the
# ring loses on its terms - is bench/ADOPTION.md.
set -euo pipefail

target=${1:-}
[ $# -gt 0 ] && shift || true

usage() {
  cat <<'EOF'
adoption: a published suite's own terms

  magicstack-pgbench   MagicStack's PostgreSQL driver toolbench, vendored
                       verbatim at a pinned commit, with fastpostgres added to
                       its Python roster (bench/ADOPTION.md,
                       bench/results/adoption-magicstack-pgbench.log)
                       --quick        warmup 1s, duration 3s
                       --duration=N   seconds per cell (default 10)
                       impl ...       python-aiopg, python-asyncpg,
                                      python-psycopg3-async, fastpostgres-dbapi

Arguments after the target go to the class's job.
EOF
}

case "$target" in
  magicstack-pgbench) just adoption-bench "$@" ;;
  "" | list | help) usage ;;
  *)
    echo "adoption: unknown target '$target'" >&2
    usage >&2
    exit 2
    ;;
esac
