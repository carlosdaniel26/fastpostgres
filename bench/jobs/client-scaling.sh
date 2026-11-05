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
  threads    the dbapi row path; the decode holds the GIL, so this buys a few
             percent, not a multiple (bench/NOTES.md: +6..12%)
  asyncio    the same work with the event loop free and a tier thread per
             connection — it tracks threads and never blocks the loop
  columnar   stream() batches reduced with numpy, whose loops release the GIL:
             N threads on N partitions genuinely run in parallel here
  processes  one process per worker, separate GILs: the tuple path parallelized
             the other way, at the price of `workers` background-worker slots
Every mode accounts for every read — the row paths' `shm` column (reads/reads),
the columnar mode's cross-worker sum — and `libpq_reads=0` says nothing fell back.
EOF
