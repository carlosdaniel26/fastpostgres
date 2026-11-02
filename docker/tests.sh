#!/usr/bin/env bash
# Runs inside the dev image, never on the host: the justfile's `test` recipe
# hands this to the container as its command.
#
# Both suites, against the entrypoint's fresh PostgreSQL:
#
#   extension   pg_regress (`make installcheck`): the extension installs, its
#               version is the control file's, and its SQL surface is the two
#               ring functions
#   client      the Python suite: the DBAPI and the dialect against the live
#               server, including the ring's round trip over /dev/shm
set -euo pipefail

# the entrypoint puts the server's socket in /tmp and its user is postgres
export PGHOST=/tmp PGUSER=postgres

echo "### extension: make installcheck"
cd /src/extension
make installcheck

echo
echo "### client: unittest"
cd /src/python
python3 -m unittest discover -s tests -v
