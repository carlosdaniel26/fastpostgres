#!/usr/bin/env bash
# Runs inside the dev image, never on the host: the justfile's `demo` recipe
# hands this to the container as its command.
#
# Run the same SQLAlchemy app on psycopg 3 and on fastpostgres, to compare the
# two drivers end to end.
#
#   demo.sh [nrows]    default 5000
set -euo pipefail

N="${1:-5000}"

demo() {
  echo
  echo "### $1"
  python3 /src/demo/sqlalchemy_app.py "$2" "$N"
}

demo "psycopg 3  (the existing driver)" "postgresql+psycopg://postgres@/postgres?host=/tmp"
demo "fastpostgres  (one URL change)"  "postgresql+fastpostgres://postgres@/postgres?host=/tmp"
