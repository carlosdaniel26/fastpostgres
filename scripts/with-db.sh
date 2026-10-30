#!/usr/bin/env bash
# Boot a throwaway PostgreSQL with the fastpostgres extension, run the given
# command as the postgres user, then stop it.
#
# The command runs as postgres because the /dev/shm ring is created 0600 and
# must be shared with the backend worker. This is the image ENTRYPOINT, so every
# container command gets a live database.
set -euo pipefail

export PGDATA=/tmp/pgdata
rm -rf "$PGDATA"
mkdir -p "$PGDATA"
chown -R postgres:postgres "$PGDATA"

# listen on 127.0.0.1 (tcp) and /tmp (unix socket), default postgres settings
gosu postgres initdb -D "$PGDATA" >/dev/null
gosu postgres pg_ctl -D "$PGDATA" -l /tmp/pg.log \
  -o "-c listen_addresses='127.0.0.1' -c unix_socket_directories='/tmp'" -w start >/dev/null
gosu postgres psql -h /tmp -d postgres -v ON_ERROR_STOP=1 \
  -c "CREATE EXTENSION fastpostgres;" >/dev/null

cleanup() { gosu postgres pg_ctl -D "$PGDATA" -m immediate -w stop >/dev/null 2>&1 || true; }
trap cleanup EXIT

gosu postgres "$@"
