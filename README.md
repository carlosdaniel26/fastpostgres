<div align="center">
  <img src="docs/assets/logo.png" height="180" alt="FastPostgres">

  <h1>FastPostgres</h1>

  <p><strong>PostgreSQL query results delivered through POSIX shared memory as typed arrays, instead of the socket's text protocol.</strong></p>

  <p>
    <a href="https://www.postgresql.org/"><img src="https://img.shields.io/badge/PostgreSQL%2018-4169E1?style=for-the-badge&logo=postgresql&logoColor=white" alt="PostgreSQL 18"></a>
    <a href="extension/include/fastpostgres/ring.h"><img src="https://img.shields.io/badge/C%20extension-00599C?style=for-the-badge&logo=c&logoColor=white" alt="C extension"></a>
    <a href="python"><img src="https://img.shields.io/badge/Python%203-3776AB?style=for-the-badge&logo=python&logoColor=white" alt="Python 3"></a>
    <a href="https://www.docker.com/"><img src="https://img.shields.io/badge/Docker-2496ED?style=for-the-badge&logo=docker&logoColor=white" alt="Docker"></a>
    <a href="./LICENSE"><img src="https://img.shields.io/badge/License-MIT-3DA639?style=for-the-badge" alt="MIT License"></a>
    <a href="docs/LIMITATIONS.md"><img src="https://img.shields.io/badge/Status-research%20prototype-B00020?style=for-the-badge" alt="Research prototype"></a>
  </p>

  <p>
    <a href="#measured-results"><strong>Results</strong></a> ·
    <a href="#where-it-loses"><strong>Where it loses</strong></a> ·
    <a href="#quick-start"><strong>Quick Start</strong></a> ·
    <a href="bench/README.md"><strong>Benchmarks</strong></a> ·
    <a href="DOCUMENTATION.md"><strong>Documentation</strong></a> ·
    <a href="bench/METHODOLOGY.md"><strong>Methodology</strong></a> ·
    <a href="docs/LIMITATIONS.md"><strong>Limitations</strong></a>
  </p>
</div>

---

Every PostgreSQL read crosses a socket, is serialized to text, and is parsed back into typed values in the client. FastPostgres removes that path for read-only `SELECT`s: a background worker runs the query with a portal and publishes every batch into `/dev/shm`, and the client maps the segment and reads it in place. The result tuples go out as the executor laid them out (one `memcpy` per row) or, for the zero-copy column views, as a columnar frame. Both formats, their readers and their writer live in one header, [`extension/include/fastpostgres/fastpostgres_shm.h`](extension/include/fastpostgres/fastpostgres_shm.h).

Writes, DDL and transactions keep using libpq, so a FastPostgres application is an ordinary PostgreSQL application with one URL changed.

> [!IMPORTANT]
> FastPostgres is a research prototype, not production software. It is Linux-only and same-host by construction (the transport is POSIX shared memory), and `/dev/shm/<seg>` is an SQL-execution endpoint. Read [`docs/LIMITATIONS.md`](docs/LIMITATIONS.md) before using it for anything real.

## Why FastPostgres

- **Nothing is parsed on the client, and the ratio says so.** The bytes the executor wrote are the bytes the client reads: no text encoding, no value parsing, no per-row Python object unless the caller wants one. Where both stacks land the same columnar read, that is **6.2-6.6x ADBC + Arrow (625-655%)**, at 100K, 1M and 10M rows (6.5-7.0x, or 655-703%, on the harder `mixed` shape).
- **The executor's own tuples.** The row frame carries PostgreSQL's tuples as they lie, one `memcpy` per row, so every type the server can store can be sent; types whose storage is not the wire bytes take the columnar frame instead.
- **Zero-copy column views.** `stream()` hands back views over the segment's columns, so a sum, a filter or a conversion to numpy touches the shared memory in place: 14.7-14.9M rows/s on the same read where ADBC + Arrow lands 2.3M.
- **One wire format, defined once.** Reader and writer share the same header, and the benchmark generator includes it too, so the format cannot drift between them.
- **Composes with a cache, does not compete with one.** A result cache removes execution on repeated queries; this removes delivery on every query. A cached aggregate beats reading the same rows 61x, and the ring beats a cached large result 2.6x; the two are measured against each other in [`bench/PG-CACHE.md`](bench/PG-CACHE.md).
- **One URL.** No new query language, no annotation, no client rewrite: the same SQLAlchemy application, on a driver whose reads take a different road.

## How it works

1. The client creates a ring segment in `/dev/shm` and initializes its semaphores, then bootstraps a background worker with one SQL call (`fastpostgres_ring_start`) over libpq.
2. The worker opens the query as a portal and, per request, fetches a batch and writes it into a data segment the request names - the row frame (`FPROWS`) or the columnar frame (`FPCOLUMN`).
3. The client maps the data segment read-only and decodes the batch in place. Requests and replies travel through the ring, so the read itself never touches a socket.
4. Two data segments alternate: the client posts the request for batch *n+1* before decoding batch *n*, which hides the worker's turn behind the decode.
5. Writes, DDL, `SET` and anything inside a transaction stay on libpq.

The extension's SQL surface is deliberately tiny: `fastpostgres_ring_start(text)` and `fastpostgres_ring_stop(text)`. Everything else runs inside the worker, off the ring.

## The read paths

A read takes one of two roads, and the choice is what the consumer does with the
result:

- **Lazy rows** — `fetchall()`, the DBAPI default. The worker writes the
  columnar frame (`FPCOLUMN`), the client copies each column out of the segment
  (one `memcpy` per column, not per value), and a tuple is built only for a row
  that is indexed or iterated. `len()`, `keys()`, `fetchone()` and `fetchmany()`
  work as they do on a list; `force_tuples=True` or
  `FASTPOSTGRES_MATERIALIZE=1` restores the eager list of tuples.
- **Zero-copy** — `stream()`. The client hands back `memoryview`s over the
  segment itself: a fixed cost to open the stream and map it, then nothing per
  row, and the `memoryview`s are valid until that stream's next pull.

The fixed cost is what makes the small end flip: at 1K rows `stream()` already
wins, while the `core` interface and process fan-out — the same work with extra
machinery — lose to a plain read, because their fixed costs dominate a result
that small. Pick `stream()` to reduce columns in place, `fetchall()` for rows
on demand, and `fetchall(force_tuples=True)` when a plain list is required.

## Measured results

All numbers below are from committed logs, on an AMD Ryzen 7 7700 (8 cores / 16 threads, 30 GiB RAM), in a Debian container on `postgres:18`, with `/dev/shm` at 3 GiB. Each player runs in a process of its own, one at a time, with a warm read before the timed ones; the protocol and its threats to validity are in [`bench/METHODOLOGY.md`](bench/METHODOLOGY.md), and every record is indexed in [`bench/RESULTS.md`](bench/RESULTS.md). The read-path headline below is the integrated run in [`bench/results/main-final-ab.log`](bench/results/main-final-ab.log) (`main@a990ca5`, 100K/1M/10M).

> **Headline: 625-655% of ADBC + Arrow** (6.25-6.55x) on the same columnar read, and **655-703%** on the `mixed` shape; `stream()` delivers 14.7-14.9M rows/s where ADBC + Arrow lands 2.3M.

### Where it loses

Read this before the ratios above: the losses are as measured as the wins, and
they are what decides whether this belongs in an application.

| where | the number | why |
|---|---|---|
| `SELECT 1+1`, 10 connections, a published suite | 41.0K queries/s against asyncpg's 85.1K | one row per statement: a stream opened and closed, against a prepared pipeline ([`bench/ADOPTION.md`](bench/ADOPTION.md)) |
| a 1000-row batched `INSERT` | 5.4/s against asyncpg's 885 | the client's libpq DML path, one row at a time; the ring is not involved |
| `COPY` | not supported | writes are libpq, and the client has no COPY call there |

### Read path: FastPostgres against ADBC + Arrow

The rival is the Arrow ecosystem's own answer to the same read - ADBC's PostgreSQL driver, whose results arrive as Arrow record batches - and each comparison asks both stacks for the same rows, consumed by neither.

![FastPostgres vs ADBC + Arrow: the same read, nothing consumed](docs/assets/zerocopy-read.png)

That is **6.2-6.6x faster on the same read — 625% at 100K, 655% at 1M, 638% at 10M** (the benchmark's own `vs arrow` ratio). The shm bytes are read where they landed, while ADBC converts the wire data into Arrow buffers first. The default DB-API read is now the same lazy columnar path (one copy per column, a tuple only for a row that is read), so it lands at the same rate as `stream()` instead of paying a Python object per value.

The ranking survives a harder shape. With `SHAPE=mixed` - NULLs on ~10% of the values, text on both sides of the 126-byte varlena split, a non-ASCII value every eighth row - the columnar comparison reads **6.5-7.0x (655-703%)** (100K 7.03x, 1M 6.55x), so the ratio holds.

| rows (`bench` shape) | FastPostgres `stream()` | ADBC + Arrow | ratio |
|---|---:|---:|---:|
| 100K | 14.66M | 2.35M | **6.25x (625%)** |
| 1M | 14.75M | 2.25M | **6.55x (655%)** |
| 10M | 14.90M | 2.34M | **6.38x (638%)** |

The figure is the original record; the integrated numbers are in the table.

The same read also reports the rest of what FastPostgres does with its rows, against its own default DBAPI read:

| path (1M rows, `bench` shape) | vs the DBAPI read |
|---|---|
| `stream()` (zero-copy views) | level (+4%): the default is the same columnar path |
| `fastpostgres.core` (the lean interface) | level (-1%) |
| four processes, DBAPI | +216% aggregate (the ring pool and memory bandwidth saturate) |
| ADBC Arrow, for reference | -83% |

### What each side holds: against Arrow under a fixed buffer

Speed is half of the comparison; memory is the other half. With the ring's transfer buffer fixed at 512 MiB - a quarter of its default - the same reads report what each side carried ([`bench/ARROW-MEMORY.md`](bench/ARROW-MEMORY.md), [`log`](bench/results/fastpostgres-vs-arrow-memory.log)):

![FastPostgres against Arrow under a fixed 512 MiB buffer](docs/assets/arrow-memory.png)

A transfer buffer is a window: the ring's is the same 512 MiB whatever the result's size. A table is a working set: at 10M rows ADBC's holds ~476 MiB at once, one size up from the buffer the ring ever spends.

### Result caches: PgCache

A result cache removes *execution* on repeated queries; this removes *delivery* on every query, so they win different shapes. The rival is PgCache, a CDC-maintained result cache in front of the same server, with its defaults ([`bench/PG-CACHE.md`](bench/PG-CACHE.md), [`log`](bench/results/fastpostgres-vs-pgcache.log)):

![FastPostgres vs PgCache: the full scan and the aggregate](docs/assets/pgcache-read.png)

A cached aggregate is 61x faster than reading the same rows directly: the executor is the cost, and no transport closes that. A cached large result is the other way round - 2.6x slower than the ring - and a hit costs what its cold fill costs, because a hit still ships every row through the socket and rebuilds every Python object.

Given the **same memory budget on both sides** - the ring's transfer buffer and the cache's own memory, one `M` - the ranking holds where the cache fits at all: at 1 GiB, the 1M-row scan is 3.89M rows/s against the cache's 1.47M, and the aggregate 1.177 ms cached against 57.403 ms read. The ring's numbers are flat from an 8 MiB budget up, while the cache's container is OOM-killed at 8 MiB and 64 MiB ([`section`](bench/PG-CACHE.md#the-same-memory-budget-on-both-sides), [`log`](bench/results/fastpostgres-vs-pgcache-memory.log)).

![FastPostgres vs PgCache under one memory budget](docs/assets/pgcache-memory.png)

### A published suite's own terms

The fourth class runs somebody else's benchmark, on its own protocol, with
FastPostgres added to its roster as one more cell: MagicStack's pgbench, the
PostgreSQL driver toolbench behind asyncpg's documentation, vendored at a pinned
commit. Seven queries over which the ring wins the reads with structure in them
(`int[]` 2.8x asyncpg, `generate_series` 1.72x), loses the one-row statement
(41.0K against 85.1K) and the batched `INSERT` (5.4 against 885, and that cell is
the client's libpq path), and cannot run `COPY` at all. The full table, the
method and every adaptation the suite needed are in
[`bench/ADOPTION.md`](bench/ADOPTION.md).

## Quick start

Requirements: a Linux host with `/dev/shm`, Docker and [`just`](https://github.com/casey/just). Everything runs in the dev image, which builds the extension, the client and the benchmark harnesses.

```bash
git clone https://github.com/carlosdaniel26/fastpostgres.git
cd fastpostgres

just test          # the extension's SQL surface and the client's test suite
just rivals adbc   # FastPostgres vs ADBC + Arrow, 100K / 1M / 10M rows
just adoption magicstack-pgbench   # somebody else's suite, our cell in its roster
just demo 5000     # the same app on psycopg 3 and on FastPostgres
```

Each command starts a throwaway PostgreSQL with the extension, runs, and exits. No service is left behind; no benchmark writes to your database.

### Benchmarks and meters

| class | command | what it answers |
|---|---|---|
| rivals | `just rivals [adbc\|arrow-memory\|cache\|cache-budget]` | how the ring compares with ADBC + Arrow and with a result cache, at their defaults and under a fixed memory budget |
| internals | `just internals [decode-cycles\|decode-ir\|decode-mca\|codec-transport\|cache-stat\|client-scaling]` | what the row decode and the transport cost at the instruction, cache and port level, and how the client itself scales across threads, asyncio and processes |
| adoption | `just adoption [magicstack-pgbench]` | what a published third-party suite's own queries and protocol find, with our cell in its roster |

`just nogil` builds the client against free-threaded CPython 3.13t and runs the
client suite plus the scaling cell there, where the row decode stops being
serialized by the GIL.

The full catalog - the scenarios, the method, the log behind every number and the
caveats - is [`bench/README.md`](bench/README.md); `just charts <target>`
renders any of the logs into an image under `docs/assets/`. The harnesses read two
host-side settings: `SHM` for the container's `/dev/shm` size (default `3g`) and
`SHAPE` for the data shape (`bench`, the friendly one, or `mixed`).

## Usage

Any SQLAlchemy application, unchanged apart from the URL:

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

For the zero-copy path, iterate the columnar batches and reduce them in place:

```python
cur = conn.cursor()
for batch in cur.stream("select id, price from products"):
    prices = np.frombuffer(batch.columns[1].data, dtype="<f8")
    total = prices.sum()          # views alias the segment: use, or copy, before the next batch
```

Read-only `SELECT`s take the ring; everything else goes over libpq. The `examples/` application is the same SQLAlchemy app run on psycopg 3 and on FastPostgres, to show what the URL change does.

When the answer is numbers, let the extension do the reduction: it streams the
result and walks each batch in C++ — no Python object per row, no numpy — with
the GIL released, so one thread per partition actually runs in parallel:

```python
sums = conn.cursor().aggregate(
    "select price, stock from products",
    {"rows": ("count_all", None), "price_sum": ("sum", "price"),
     "cheapest": ("min", "price"), "stock_seen": ("count", "stock")},
)
```

The asyncio face is `fastpostgres.aio`: every connection gets a thread of its own
for the blocking ring waits (the event loop is not blocked), and each live stream
holds its own partition of the shared memory, so several connections' streams run
at once — each on its own ring:

```python
from fastpostgres import aio

async with await aio.connect("host=/tmp dbname=app user=app") as conn:
    rows = await conn.fetchall("select id, name from products")
    async with conn.stream("select id, price from products") as batches:
        async for batch in batches:
            total += np.asarray(batch.columns[1]).sum()   # valid until the next pull
```

An async read queues for a free partition (`wait=True`, the default) rather than
dropping to libpq; the GIL still serializes the decode, so the win is many
streams in flight, not a faster single read.

Where the parallelism is, measured (`just internals client-scaling`): with one
ring per thread, the consumption paths scale — `aggregate` (the C++ reduce) +340%,
the numpy reduction +367%, one process per worker +386% at six workers. The
benchmark measures those, not Python-tuple materialization: reading rows into
objects is the DB-API compatibility path, and it is GIL-bound — on a
**free-threaded** interpreter (`just nogil`: CPython 3.13t, the module declaring
`Py_MOD_GIL_NOT_USED`) even that path scales, which is why that build exists.

## Configuration

| variable | default | effect |
|---|---|---|
| `FASTPOSTGRES_SHM_POOL` | sized from the machine and the server, at most 8 | how many rings (each one background worker) a process may create. `0` turns the shared-memory path off and every read stays on libpq |
| `FASTPOSTGRES_SEGMENT_BYTES` | 1 GiB | capacity of each data segment, in bytes; clamped to 64 KiB - 1 GiB. A stream alternates two of them |

On the server side, `max_worker_processes` bounds how many rings (background workers) can exist at once; when the budget is exhausted a read falls back to libpq and the client warns once.

## Status

- **A research prototype.** Linux, same host, single writer's discretion: see [`docs/LIMITATIONS.md`](docs/LIMITATIONS.md) for the list, including the ones that shape how it can be used at all.
- **Tested**: the extension's SQL surface under `pg_regress`, and 54 client tests covering the wire types, the ring pool, the threads/partitions, the asyncio layer, the C++ aggregate path, fork safety and the libpq fallbacks; CI runs both, plus ruff, shellcheck and the markdown link check. The same suite passes on the free-threaded 3.13t build (`just nogil`).
- **Not here yet**: COPY, a network transport, and any platform but Linux. Writes, DDL and transactions are libpq's, deliberately.

## Documentation

Project layout, the documentation index, the known limitations, contributing,
and the license are in [`DOCUMENTATION.md`](DOCUMENTATION.md); the claim's own
tree - the classes, the records, the arguments they back - starts at
[`bench/README.md`](bench/README.md).
