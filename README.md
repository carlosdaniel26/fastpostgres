<p align="center"><img src="assets/logo.png" width="600" alt="FastPostgres"></p>

# FastPostgres

PostgreSQL query results delivered through POSIX shared memory as columnar
typed arrays, instead of the socket's text protocol.

A background worker in the backend runs each read with an SPI cursor and
publishes result batches as `FPCOLUMN` payloads in `/dev/shm`; the client maps
the segment and reads the columns in place. Writes, DDL and transactions keep
using libpq.

## Layout

| path | contents |
| --- | --- |
| `pg-extension/` | server extension (C): columnar encoder, ring worker, segment ingest |
| `python-extension/` | client (C++): extension module, PEP 249 driver, SQLAlchemy dialect |
| `benchmark/` | transport and SQLAlchemy-driver benchmarks |
| `scripts/` | one container image, `dev.sh`, `run_bench.py` |

## How a read runs

1. **Bootstrap, once per process.** `Ring.start()` sends
   `SELECT fastpostgres_ring_start('<seg>')` over libpq; that registers a
   dynamic background worker, which maps the ring and blocks on its semaphore.
2. **Requests.** The client writes `OPEN` / `NEXT` / `CLOSE` into the ring
   (`/dev/shm` + POSIX semaphores, one request in flight).
3. **Server.** The worker opens an SPI cursor, fetches a batch, encodes it into
   a second segment as `FPCOLUMN`, publishes it (payload size, then a seqlock
   bump), and replies with the row count.
4. **Client.** The segment is mapped once and read in place: either the columnar
   views are consumed as-is (zero-copy), or the chunk is decoded into tuples.

Everything that is not a read-only `SELECT` — DML, DDL, `SET`, multi-statement
SQL — goes over libpq.

## Wire format

`pg-extension/fastpostgres_shm.h` is the single definition, included by both
sides:

```
[ FastpostgresShmHeader (64 B) ][ FPCOLUMN: magic, version, flags, ncols, nrows
                                  per column: name, kind, pgtype, nulls, data_len, data ]
```

Columns carry a wire kind (`bool`, `int16/32/64`, `float4/8`, `text`) so the
client needs no type table; fixed-width data is 8-byte aligned, which is what
makes zero-copy typed views possible. The header holds the reader *and* the
writer, so the layout has one implementation.

## Usage

Point an existing SQLAlchemy app at it — no model or query changes:

```python
import fastpostgres.dialect  # registers the postgresql+fastpostgres dialect
from sqlalchemy import create_engine

engine = create_engine("postgresql+fastpostgres://user@/app?host=/tmp")
```

Or use the driver directly, like any PEP 249 module:

```python
import fastpostgres.dbapi as dbapi

conn = dbapi.connect("host=/tmp dbname=app user=app")
cur = conn.cursor()
cur.execute("select id, name from products")
rows = cur.fetchall()
```

## Build and run

Needs Docker on a Linux host with `/dev/shm`.

```bash
scripts/dev.sh test          # client test suite
scripts/dev.sh demo 5000     # the same app on psycopg2 and fastpostgres
scripts/run_bench.py         # benchmarks + charts
```

## Measured

![SQLAlchemy Core reads: fast-postgres vs psycopg2](assets/core-read.png)

8-core container, PostgreSQL 18.6, `/dev/shm` 3 GB. At the transport layer reads
run ~6× the socket transports (≈7.5 M vs ≈1.2 M rows/s) and writes ~2.5×. Most
of that is given back once rows become Python objects: the Core path above lands
17–34% ahead of psycopg2, the ORM path 5–13%, because materialising and
hydrating objects is allocation-bound. Regenerate with `scripts/run_bench.py`.

## Limitations

- Ring reads run in the **worker's** session, so they do not observe the calling
  connection's uncommitted writes; the DB-API connection is autocommit.
- `numeric` is widened to `float8` on the wire (lossy). Integers, floats, text,
  timestamps, uuid and bytea are exact.
- A ring serves one stream at a time; a process holds at most
  `FASTPOSTGRES_SHM_POOL` (default 4) worker-backed rings.
- The ring is an SQL-execution endpoint: `/dev/shm/<seg>` is mode 0600 and must
  stay owned by the database user. Not intended for production.
