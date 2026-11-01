<p align="center"><img src="assets/logo-speed.png" width="600" alt="FastPostgres"></p>

# FastPostgres

PostgreSQL query results delivered through POSIX shared memory as typed arrays,
instead of the socket's text protocol. A background worker runs each read with a
portal and publishes every batch in `/dev/shm`: the result tuples as the executor
laid them out (`FPROWS`, one memcpy per row) or, for the zero-copy column views,
a columnar frame (`FPCOLUMN`). The client maps the segment and reads it in place.
Writes, DDL and transactions keep using libpq. Both formats, their readers and
their writer live in one header, `pg-extension/fastpostgres_shm.h`.

## Measured

![SQLAlchemy Core reads](assets/core-read.png)

8-core container, PostgreSQL 18.6, 100K / 1M / 10M rows, one process per load.
The Core path lands 73–96% ahead of psycopg2 and the gap widens with the load;
the ORM lands 1–10%, because there SQLAlchemy's own hydration is ~4 µs of a
~5.5 µs row and no driver can move it.

## Usage

Any SQLAlchemy app, unchanged apart from the URL:

```python
import fastpostgres.dialect  # registers postgresql+fastpostgres
engine = create_engine("postgresql+fastpostgres://user@/app?host=/tmp")
```

Or the driver directly, as a PEP 249 module:

```python
import fastpostgres.dbapi as dbapi

conn = dbapi.connect("host=/tmp dbname=app user=app")
rows = conn.cursor().execute("select id, name from products").fetchall()
```

Read-only `SELECT`s take the ring; everything else goes over libpq. Build and
run with Docker and `just` (Linux host with `/dev/shm`):

```bash
just test          # client test suite
just demo 5000     # the same app on psycopg2 and fastpostgres
just charts        # benchmarks + charts
```

Known limits: ring reads run in the worker's session, so they miss the calling
connection's uncommitted writes (the driver is autocommit); `numeric` widens to
`float8` on the wire; a ring serves one stream at a time, with at most
`FASTPOSTGRES_SHM_POOL` (default 4) per process, and concurrent ring reads are
capped server-side by `max_worker_processes` — one background worker per ring, so
7 with the default of 8, after which reads fall back to libpq; the client's object
arena is sized to half the cgroup limit (8 GiB at most, `FASTPOSTGRES_OBJ_ARENA`
to override) and is never returned to the OS; `/dev/shm/<seg>` is mode 0600 and the
ring is an SQL-execution endpoint, so it is not for production.
