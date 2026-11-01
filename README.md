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

Against that Core read, the paths a consumer can opt into (rows/s, 100K / 1M /
10M): `stream()`'s zero-copy column views **7.5M / 7.8M / 8.2M**, the lean core
interface (`fastpostgres.core`, tuples instead of `Row`) **6.8M / 6.5M / —**, and
Core across four processes **3.7M / 4.2M / —**, for Core's 1.7M / 1.4M / 1.2M.
`stream()` never builds an object, so it is the path that scales with the load.

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
`float8` on the wire; a ring serves one stream at a time and the
pool per process is sized on first use from the machine (one stream per ~1.2
hardware threads, divided by the SMT width — 6 on this box) and from the free
`max_worker_processes` slots left on the server, `FASTPOSTGRES_SHM_POOL` to
override; when that worker budget is exhausted the read falls back to libpq and
says so once; the client's object
arena is sized to half the cgroup limit (8 GiB at most, `FASTPOSTGRES_OBJ_ARENA`
to override) and is never returned to the OS; `/dev/shm/<seg>` is mode 0600 and the
ring is an SQL-execution endpoint, so it is not for production.
