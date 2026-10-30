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
4. **Client.** `read()` maps the segment once and returns memoryviews over it;
   `read_rows()` decodes the same chunk into Python tuples.

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

## Client API

```python
import fastpostgres as fp

seg  = fp.read("seg_name")        # zero-copy: seg.columns[i].data is a memoryview
rows = fp.read_rows("seg_name")   # typed tuples, decoded natively
```

`fastpostgres.dbapi` is a PEP 249 driver: read-only `SELECT`s take the ring,
everything else libpq, falling back to libpq when the ring is unavailable.
`Cursor.stream()` yields zero-copy columnar batches.

`postgresql+fastpostgres://` is a SQLAlchemy dialect, so an existing app changes
only its URL.

## Build and run

Needs Docker on a Linux host with `/dev/shm`.

```bash
scripts/dev.sh test          # client test suite
scripts/dev.sh demo 5000     # the same app on psycopg2 and fastpostgres
scripts/run_bench.py         # benchmarks + charts
```

## Measured

8-core container, PostgreSQL 18.6, `/dev/shm` 3 GB:

| workload | socket | fastpostgres |
| --- | --- | --- |
| read, transport (rows/s) | ~1.2 M | ~7.5 M |
| write, transport (rows/s) | ~1.0 M | ~3.4 M |
| rows through the DB-API, 10 M (rows/s) | 2.5 M | 3.7 M |
| zero-copy stream, 10 M (rows/s) | — | 9.6 M |
| SQLAlchemy Core read | baseline | +30–60% |
| SQLAlchemy ORM read | baseline | ~0–30% |

The transport is roughly 6× the socket. Materializing rows into Python objects
is allocation-bound and consumes most of that; the ORM's own hydration consumes
most of the rest.

## Limitations

- Ring reads run in the **worker's** session, so they do not observe the calling
  connection's uncommitted writes; the DB-API connection is autocommit.
- `numeric` is widened to `float8` on the wire (lossy). Integers, floats, text,
  timestamps, uuid and bytea are exact.
- A ring serves one stream at a time; a process holds at most
  `FASTPOSTGRES_SHM_POOL` (default 4) worker-backed rings.
- The ring is an SQL-execution endpoint: `/dev/shm/<seg>` is mode 0600 and must
  stay owned by the database user. Not intended for production.
