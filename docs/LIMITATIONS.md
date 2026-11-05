# Limits, edge cases and trade-offs

Written for a reader deciding whether to trust the numbers or to use this. The
method is in [`METHODOLOGY.md`](../bench/METHODOLOGY.md); the measured record is
[`fastpostgres-vs-arrow.log`](../bench/results/fastpostgres-vs-arrow.log) (bench shape),
[`fastpostgres-vs-arrow-mixed.log`](../bench/results/fastpostgres-vs-arrow-mixed.log) (the harder
shape) and [`fastpostgres-vs-arrow-small.log`](../bench/results/fastpostgres-vs-arrow-small.log)
(1K rows).

## What the benchmark covers

| | covered | where |
|---|---|---|
| six narrow columns, short ASCII text, no NULLs | yes | `fastpostgres-vs-arrow.log` |
| NULLs, both varlena header sizes, non-ASCII | yes | `SHAPE=mixed` log |
| 100K / 1M / 10M rows | yes | all three logs |
| a small result | 1K rows | `fastpostgres-vs-arrow-small.log` |
| what each side holds while it reads | 1M / 5M / 10M rows | `fastpostgres-vs-arrow-memory.log` |

## What it does not cover, and what happens there

- **Wide rows (24+ attributes), very wide values (1 MB of text), numeric,
  timestamps, jsonb, arrays, enums.** The row frame carries PostgreSQL's own
  tuples, so any type the server can store can be sent; numeric is the documented
  exception (it widens to float8), and types whose storage is not the wire bytes
  take the columnar frame instead. None of those shapes is measured here.
- **Cold page cache, first touch, page faults.** The segment faults once per
  process. The benchmark does not measure fault counts.
- **Concurrency.** One stream at a time per ring; several processes are shown as
  one aggregate row, not as a latency distribution. No mixed read/write workload
  is measured. The client's thread and asyncio scaling is measured on its own
  (`just internals client-scaling`), not through the rival benchmark.
- **The asyncio layer.** A thread-per-connection facade: each connection's tier
  thread runs the blocking ring waits with the GIL released, so the event loop
  stays live, but concurrency is capped by the ring pool. No `PQcancel` yet —
  cancelling a task lets the call in flight finish (its result is dropped),
  rather than interrupting libpq or the ring. A batch's views are valid until
  that stream's next pull, so copy what must outlive it.
- **The row decode and the GIL.** Reading tuples is Python-object construction, so
  it holds the GIL and N threads on N partitions gain a few percent on a normal
  build (`just internals client-scaling`, `bench/results/client-scaling.log`). The
  zero-copy path scales there anyway (numpy releases the GIL, +375% at six
  workers), and the row path scales on a free-threaded interpreter: `just nogil`
  builds the client against CPython 3.13t and the same cell runs +119% at six
  threads (`bench/results/client-scaling-nogil.log`).
- **Multi-host.** Everything is POSIX shared memory — client and server share
  `/dev/shm`. There is no network path to benchmark.
- **ADBC parallelism.** The ADBC player is a single connection doing one fetch.
  A threaded ADBC consumer is not measured, so "ADBC is slower" means this ADBC
  usage, not every possible one.
- **Memory as a budget.** `just arrow-memory-bench` fixes the ring's transfer
  buffer (512 MiB by default) and reports what each side held — the ring's segment
  and batch, ADBC's largest Arrow batch, the Arrow table's whole result — in
  [`fastpostgres-vs-arrow-memory.log`](../bench/results/fastpostgres-vs-arrow-memory.log). It is
  one shape, three sizes and three reps; it does not measure allocation *rates*,
  page faults, or a consumer that holds several results at once.

## Pros and cons

### fastpostgres vs ADBC + Arrow

| | fastpostgres | ADBC + Arrow |
|---|---|---|
| Python rows, 100K–10M | 2.6–3.0× faster | baseline (Arrow → `to_pylist`) |
| columnar + one sum | 3.5–3.6× faster | Arrow record batches |
| what crosses the wire | nothing: the executor's tuple bytes are written into the reader's address space | libpq binary/text, then converted into Arrow buffers |
| ecosystem | custom columnar frame; numpy/pandas adapters | Arrow, parquet, pandas, polars, duckdb |
| portability | Linux, same host, server extension | any OS, any network, many databases |
| operations | one background worker per ring, 0600 shm | ordinary connections |
| failure modes | worker death loses the batch; views alias the segment (invalid after the next pull) | ordinary PostgreSQL errors |
| maturity | research prototype, not for production | production drivers |

### The paths inside fastpostgres (1M, bench shape)

| path | rows/s | use it when |
|---|---|---|
| four processes, DBAPI | 9.05M aggregate | one process cannot keep up |
| `stream()` + numpy | 7.97M | you want numbers, not rows |
| DBAPI / `core` | 3.79M | you want Python tuples |

## When not to use this

- in production, multi-user, or across a network;
- when your consumers are Arrow-native — use ADBC and change nothing else;
- for small results, where the plain driver is simpler and as fast;
- when you cannot install a server extension or spare background workers
  (`max_worker_processes` bounds the ring pool).

## FAQ — the objections this will get

**"Not zero-copy: the server wrote the bytes."** Correct. The *client* copies
nothing and parses nothing; the server memcpys the executor's tuple into the
segment. The claim is "no serialization on the wire, no parse on the client",
not "no memory movement at all".

**"ADBC → `to_pylist` is a straw man."** It was the fair worry, so it is now
measured rather than argued. ADBC's DBAPI can also hand back Python rows itself
with `fetchall()` — the plainest DBAPI call there is — and on the same 1M rows it
runs at **0.14M rows/s**, against **1.34M** for the `to_pylist` route the harness
uses. The column-wise `to_pylist` is the *faster* of ADBC's two row routes by
roughly 10×, so the comparison uses the generous one, not the convenient one.
Both are in the PATHS table of every run. The route ADBC is designed for is the
columnar one, and that comparison is reported beside it.

**"3× on six narrow columns proves nothing."** The mixed shape is the answer:
with NULLs, both varlena header sizes and non-ASCII text, the rank holds and
ADBC loses relatively more (about −18% throughput at 1M, against −13% for
fast-postgres on tuples and −7% on the columnar path), so the ratio does not
shrink.

**"You benchmarked your extension against a general-purpose driver."** Yes, and
ADBC is the strongest general-purpose answer to the same question. Same
container, same server, same rows, same process isolation, same statement.

**"Where are the error bars?"** Within-run: the `spread:` line under each row
(± half the min–max range; under 2% at 100K/1M, up to ~15% at 1K). Run-to-run:
documented in METHODOLOGY (a few percent for the driver rows, up to ~10% for
`stream()`). The 10M sizes are single-rep and marked `n=1`.

**"Was the benchmark run on the code that is published?"** Each log's `### ENV`
block carries `git:` (`git describe --always --dirty`). A `-dirty` suffix means
the measurement included uncommitted changes: commit, then `just bench`, for a
clean stamp.

**"Why not just cache the result?"** A result cache removes *execution* on
repeated queries; this removes *delivery* on every query, and the two rank by
workload shape. Measured against PgCache, a CDC-maintained result cache, in
[`fastpostgres-vs-pgcache.log`](../bench/results/fastpostgres-vs-pgcache.log): a cached aggregate
is 56× faster than the ring at 1M rows — the executor is the cost, and no
transport fixes that. A cached 1M-row result is the other way round: 1.42M rows/s
against 3.79M on the ring, and a hit costs what its cold fill costs, because a hit
still ships every row through the socket and builds every Python object, while
this path never builds them twice. Method and caveats in
[`PG-CACHE.md`](../bench/PG-CACHE.md).

**"Would equal memory change that?"** It was measured, not argued
(`just pgcache-memory-bench`, the record in
[`fastpostgres-vs-pgcache-memory.log`](../bench/results/fastpostgres-vs-pgcache-memory.log)): one
budget M on both sides, the ring's transfer buffer against the cache's own
memory. Where the cache fits, the ranking is the same — at 1 GiB, the aggregate
is 1.229 ms against the ring's 57.628 ms and the scan 1.45M against 3.87M rows/s.
What the budget bought the ring above ~one batch is nothing (its numbers are flat
from an 8 MiB budget up, and its segments grow 128× across the sweep), while
the cache's container is OOM-killed at 8 MiB and 64 MiB before it can serve.

**"Which numbers should I quote?"** The committed logs, not the README:
`bench/results/fastpostgres-vs-arrow*.log`.
