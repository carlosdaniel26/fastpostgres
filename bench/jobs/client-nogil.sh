#!/usr/bin/env bash
# Runs inside the free-threaded image (`--target nogil`, CPython 3.13t): the
# client suite under an interpreter without the GIL, then the scaling cell's row
# paths — where the decode is no longer serialized and N threads on N partitions
# should finally add up.
set -euo pipefail

PY=/opt/nogil/bin/python
export PYTHONPATH=/src/python/src
export PATH=/opt/nogil/bin:$PATH

echo "### NOGIL"
"$PY" - <<'EOF'
import sys

import fastpostgres._client as client

print(f"python: {sys.version.split()[0]}")
print(f"gil: {'disabled' if not sys._is_gil_enabled() else 'enabled'}")
print(f"client: {client.__file__}")
EOF
echo "### END NOGIL"
echo

cd /src/python
"$PY" -m unittest discover -s tests -v

reps=${REPS:-3}
echo
echo "### the client's own scaling, GIL off"
"$PY" /src/bench/internals/threads/client_scaling.py --mode threads --reps "$reps"
echo
"$PY" /src/bench/internals/threads/client_scaling.py --mode columnar --reps "$reps"
