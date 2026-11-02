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

![fast-postgres vs ADBC + Arrow: the same output, Python tuples](assets/tuples-read.png)

AMD Ryzen 7 7700 (8 cores / 16 threads, 30 GiB RAM), Debian container on
`postgres:18`, `/dev/shm` 3 GiB, 100K / 1M / 10M rows. Each player is measured
in a process of its own, one at a time, with a warm read before the timed ones.
The rival is the Arrow ecosystem's own answer to the same read — ADBC's
PostgreSQL driver, whose results arrive as Arrow record batches — and each
comparison asks both stacks for the same output:

| same output: Python tuples | 100K | 1M | 10M |
|---|---|---|---|
| fast-postgres (its DBAPI) | 6.42M | 7.19M | 7.25M |
| ADBC + Arrow → `to_pylist` | 1.28M | 1.27M | 1.27M |

![fast-postgres vs ADBC + Arrow: the same output, columnar, one column summed](assets/zerocopy-read.png)

| same output: the columnar form, one column summed in place | 100K | 1M | 10M |
|---|---|---|---|
| fast-postgres `stream()` + numpy | 8.20M | 8.25M | 8.32M |
| ADBC Arrow + `pyarrow.compute` | 2.23M | 2.28M | 2.18M |

That is **5.0–5.7× faster when both build Python tuples** and **3.6–3.8× faster
when both consume columnar**: the shm bytes are read where they landed, while
ADBC converts the wire data into Arrow buffers first — and, for tuples, converts
a second time into Python objects. (Comparing fast-postgres's tuples against
ADBC's Arrow table would price those two outputs, not the two transports, which
is why neither table above does it.)

Both of those are about speed. What each side *holds* while it delivers the same
rows is the other half of the comparison
([`docs/ARROW-MEMORY.md`](docs/ARROW-MEMORY.md), `just arrow-memory-bench`):
with the ring's transfer buffer fixed at 512 MiB, the ring's segment is 512 MiB
at 1M rows and at 10M, ~4.9 MiB of it in flight per 100k-row batch and a flat
~143 MiB RSS — while ADBC's record-batch reader holds one 11.6 MiB batch at a
time, and its Arrow table path holds the whole result, 475.9 MiB at 10M rows
(649 MiB of RSS), in buffers. The ring is 3.6–3.8× faster there too, the same
ranking as above.

![fast-postgres against Arrow under a fixed 512 MiB buffer](assets/arrow-memory.png)

The ranking survives a harder shape. With `SHAPE=mixed` — NULLs on ~10% of the
values, text on both sides of the 126-byte varlena split, a non-ASCII value every
eighth row — the same two comparisons read **4.7–6.0×** and **4.1–4.3×**: the
data slows fast-postgres ~13% and ADBC ~18%, so the ratio holds.

`just bench` also prints the rest of what fast-postgres does with those rows,
against its own driver read: `stream()` (+13–21%), the lean core interface
(`fastpostgres.core`, level with the DBAPI), four processes (+84–94% aggregate
at 100K/1M, +21% at 10M where the ring pool and memory bandwidth saturate), the
same read through SQLAlchemy CORE (−72–83%, which is SQLAlchemy's `Row`
construction at ~570 ns/row, not the transport), and ADBC's Arrow + sum for
reference (−65–69%).

The runs this section quotes are committed, every line of them — each player's
own report, the per-rep spread, and the `git:`/`shape:` stamp:
[bench shape](docs/fastpostgres-vs-arrow.log),
[mixed shape](docs/fastpostgres-vs-arrow-mixed.log), and a
[1K-row run](docs/fastpostgres-vs-arrow-small.log) where the paths inside
fast-postgres invert and the plain driver read wins. The protocol, statistics
and threats to validity are in [`docs/METHODOLOGY.md`](docs/METHODOLOGY.md); the
edge cases, pros/cons and anticipated objections are in
[`docs/LIMITATIONS.md`](docs/LIMITATIONS.md); and the mechanism-by-mechanism
answer to "why is this faster than Arrow, and why doesn't Arrow do the same
thing?" is in [`docs/WHY-IT-WINS.md`](docs/WHY-IT-WINS.md).

## Measured against a result cache

A result cache removes *execution* on repeated queries; this removes *delivery*
on every query, so they win different shapes. The rival is PgCache, a
CDC-maintained result cache in front of the same server; the method, the fill and
pass-through taxes and the caveats are in [`docs/PG-CACHE.md`](docs/PG-CACHE.md)
and the committed record in
[`docs/fastpostgres-vs-pgcache.log`](docs/fastpostgres-vs-pgcache.log)
(`just pgcache-bench`).

![fast-postgres vs PgCache: the full scan and the aggregate](assets/pgcache-read.png)

| 1M rows, the same rows out | fastpostgres | PgCache (hit) |
|---|---|---|
| the full scan, rows/s | **6.35M** | 1.47M |
| count/sums, ms/read | 62.533 | **1.228** |

A cached aggregate is 51× faster than reading the same rows directly — the
executor is the cost, and no transport closes that. A cached large result is the
other way round: 4.3× slower than the ring, and a hit costs what its cold fill
costs, because a hit still ships every row through the socket and rebuilds every
Python object.

Given the **same memory budget** on both sides — the ring's transfer buffer and
the cache's own memory, one M — the ranking holds where the cache fits at all:
at 1 GiB, the 1M-row scan is 6.53M rows/s against the cache's 1.47M and the
aggregate 1.117 ms against 55.776 ms. The ring's numbers are flat from an 8 MiB
budget up (a transfer buffer is a window, one batch wide), while the cache's
container is OOM-killed below ~256 MiB and its registration is throttled to
nothing at 256 MiB. `just pgcache-memory-bench` sweeps it and proves each cell
from both sides' own evidence — segment bytes and `shm_reads` for the ring, the
proxy's memory gauges for the cache
([section](docs/PG-CACHE.md#the-same-memory-budget-on-both-sides),
[log](docs/fastpostgres-vs-pgcache-memory.log)).

![fast-postgres vs PgCache under one memory budget](assets/pgcache-memory.png)

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
just bench         # fast-postgres vs ADBC + Arrow, 100K / 1M / 10M
just demo 5000     # the same app on psycopg 3 and fastpostgres
just charts        # benchmarks + charts
just codec-transport  # format cost vs transport cost, isolated (2x2 matrix)
just codec-charts     # charts for that matrix
just decode-cycles # the row decode's cycles/row, on a fixed shape
just decode-ir     # the decode's dynamic instructions + miss profile (callgrind)
just decode-mca    # ports and latency of the decode loop (llvm-mca)
just cache-stat    # cache counters on the host's PMU: decodes, transports, sweep
```

The decode meters, the matrix, the cache run and their caveats are documented in
`simd/hypotheses.md` and [`docs/CODEC-TRANSPORT.md`](docs/CODEC-TRANSPORT.md).

Known limits: ring reads run in the worker's session, so they miss the calling
connection's uncommitted writes (the driver is autocommit); `numeric` widens to
`float8` on the wire; a ring serves one stream at a time and the
pool per process is sized on first use from the machine (one stream per ~1.2
hardware threads, divided by the SMT width — 6 on this box) and from the free
`max_worker_processes` slots left on the server, `FASTPOSTGRES_SHM_POOL` to
override; when that worker budget is exhausted the read falls back to libpq and
says so once; the client's object
arena is sized to half the cgroup limit (8 GiB at most, `FASTPOSTGRES_OBJ_ARENA`
to override) and is never returned to the OS; the ring is an SQL-execution
endpoint and `/dev/shm/<seg>` must be treated as one — the client creates the
segment 0600 and the worker widens it to 0666 (`pg-extension/src/shm.c`) so a
client running as a different user can map it — so it is not for production. The
full list —
what the benchmark does not cover and what happens in those cases, the pros and
cons against ADBC, and the objections to expect — is in
[`docs/LIMITATIONS.md`](docs/LIMITATIONS.md).

## Citing

If you use this in academic work, cite it via [`CITATION.cff`](CITATION.cff) —
GitHub renders a "Cite this repository" button from it. The measurement protocol
and its threats to validity live in [`docs/METHODOLOGY.md`](docs/METHODOLOGY.md);
the decode's lab notebook is [`simd/hypotheses.md`](simd/hypotheses.md).

## License

MIT — see [`LICENSE`](LICENSE).
