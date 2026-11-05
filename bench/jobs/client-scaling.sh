#!/usr/bin/env bash
# Runs inside the dev image: the justfile's `client-scaling` recipe hands this to
# the container as its command.
#
# The client's own scaling, one partition per worker: the serial baseline, then
# threads (dbapi, one connection and ring per thread) and asyncio (one
# AsyncConnection and tier thread per task) at 1..N workers. The cell is
# bench/internals/threads/client_scaling.py; this wrapper says which numbers are
# the point and refuses to print a table that cannot mean anything:
#
#   - every worker must have read the whole series (the cell exits otherwise)
#   - the `shm` column must be reads/reads: a fallback to libpq shows up there
#
#   client-scaling.sh [--rows N] [--reads N] [--max-workers N] [--mode M]
set -euo pipefail

echo "### CLIENT-SCALING RUN"
echo "args: ${*:-<cell defaults>}"
echo "### END CLIENT-SCALING RUN"

out=$(mktemp)
trap 'rm -f "$out"' EXIT

python3 /src/bench/internals/threads/client_scaling.py "$@" | tee "$out"

if ! grep -q '^partitions: ' "$out"; then
    echo "client-scaling: the cell produced no summary; see the output above" >&2
    exit 1
fi

echo
echo "--- reading the table ---"
cat <<'EOF'
serial is one connection, back to back: the same road the modes take, so that row
sits next to their n=1 rows. Each mode is compared against its own n=1 row:
  columnar   stream() batches reduced with numpy: the array path
  threads    Cursor.aggregate: sum/count walked in C++ over the segment, no
             Python object per row — the path that parallelizes on any interpreter
  asyncio    the same consumption with the event loop free and a tier thread per
             connection
  processes  one process per worker, separate GILs, at the price of `workers`
             background-worker slots
Tuple reads (execute + fetchall) are deliberately not measured: that is the
DB-API compatibility path, not how a result is consumed in a hot path.
Every aggregate read is accounted for by the `shm` column (reads/reads), the
columnar mode by the cross-worker sum, and `libpq_reads=0` says nothing fell back.
EOF
