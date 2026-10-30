#!/usr/bin/env bash
set -euo pipefail

# Build the image once, then run something in it (every command gets a throwaway
# PostgreSQL with the extension, from scripts/with-db.sh).
#
#   scripts/dev.sh test            the Python client's test suite
#   scripts/dev.sh bench [nrows]   transport + SQLAlchemy benchmarks
#   scripts/dev.sh demo [nrows]    the psycopg2 vs fastpostgres driver swap
#
# Row counts default to each benchmark's own.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="fastpostgres-dev"

run() {
  docker build -q -t "$IMG" -f "$ROOT/scripts/Dockerfile" "$ROOT" >/dev/null
  docker run --rm --shm-size="${SHM:-3g}" "$@"
}

case "${1:-}" in
  test)
    run "$IMG" python3 -m unittest discover -s tests -v
    ;;
  bench)
    shift
    run -e SIZES="$*" "$IMG" bash -c \
      'python3 /src/benchmark/bench.py $SIZES && python3 /src/benchmark/sqlalchemy_bench.py $SIZES'
    ;;
  demo)
    run -e N="${2:-5000}" "$IMG" bash -c '
      run() { echo; echo "### $1"; python3 /src/demo/sqlalchemy_app.py "$2" "$N"; }
      run "psycopg2  (the existing driver)" "postgresql+psycopg2://postgres@/postgres?host=/tmp"
      run "fastpostgres  (one URL change)"  "postgresql+fastpostgres://postgres@/postgres?host=/tmp"
    '
    ;;
  *)
    echo "usage: $0 {test|bench|demo} [nrows ...]" >&2
    exit 2
    ;;
esac
