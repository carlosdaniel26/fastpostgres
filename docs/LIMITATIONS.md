# Limits, edge cases and trade-offs

Written for a reader deciding whether to trust the numbers or to use this. The
method is in [`METHODOLOGY.md`](METHODOLOGY.md); the measured record is
[`fastpostgres-vs-arrow.log`](fastpostgres-vs-arrow.log) (bench shape),
[`fastpostgres-vs-arrow-mixed.log`](fastpostgres-vs-arrow-mixed.log) (the harder
shape) and [`fastpostgres-vs-arrow-small.log`](fastpostgres-vs-arrow-small.log)
(1K rows).

## What the benchmark covers

| | covered | where |
|---|---|---|
| six narrow columns, short ASCII text, no NULLs | yes | `fastpostgres-vs-arrow.log` |
| NULLs, both varlena header sizes, non-ASCII | yes | `SHAPE=mixed` log |
| 100K / 1M / 10M rows | yes | all three logs |
| a small result | 1K rows | `fastpostgres-vs-arrow-small.log` |

## What it does not cover, and what happens there

- **Wide rows (24+ attributes), very wide values (1 MB of text), numeric,
  timestamps, jsonb, arrays, enums.** The row frame carries PostgreSQL's own
  tuples, so any type the server can store can be sent; numeric is the documented
  exception (it widens to float8), and types whose storage is not the wire bytes
  take the columnar frame instead. None of those shapes is measured here.
- **Cold page cache, first touch, page faults.** The segment faults once per
  process and the arena's pages are first-touch. The logs carry the arena state
  (`objpool:` lines) but the benchmark does not measure fault counts.
- **Concurrency.** One stream at a time per ring; several processes are shown as
  one aggregate row, not as a latency distribution. No mixed read/write workload
  is measured.
- **Multi-host.** Everything is POSIX shared memory — client and server share
  `/dev/shm`. There is no network path to benchmark.
- **ADBC parallelism.** The ADBC player is a single connection doing one fetch.
  A threaded ADBC consumer is not measured, so "ADBC is slower" means this ADBC
  usage, not every possible one.

## Small results: where the rankings invert

At 1K rows the stack-versus-stack ranking holds — tuples 6.11M vs 1.16M rows/s,
columnar + sum 4.92M vs 1.50M — but the paths *inside* fast-postgres invert:

| path at 1K rows | rows/s | vs the plain driver read |
|---|---|---|
| DBAPI `fetchall` | 5.36M | — |
| `stream()` + numpy sum | 4.71M | −12% |
| `core` interface | 2.62M | −51% |
| four processes | 1.37M | −74% |

The sum costs more than it saves on a small batch, the lean interface's fixed
costs dominate, and fan-out is pure overhead. For small results, use the plain
driver read. (Within-run spread at this size is ±15%, so treat the ordering, not
the digits.)

## Pros and cons

### fastpostgres vs ADBC + Arrow

| | fastpostgres | ADBC + Arrow |
|---|---|---|
| Python rows, 100K–10M | 5.0–5.7× faster | baseline (Arrow → `to_pylist`) |
| columnar + one sum | 3.6–3.8× faster | Arrow record batches |
| what crosses the wire | nothing: the executor's tuple bytes are written into the reader's address space | libpq binary/text, then converted into Arrow buffers |
| ecosystem | custom columnar frame; numpy/pandas adapters | Arrow, parquet, pandas, polars, duckdb |
| portability | Linux, same host, server extension | any OS, any network, many databases |
| operations | one background worker per ring, 0600 shm, arena never returned | ordinary connections |
| failure modes | worker death loses the batch; views alias the segment (invalid after the next pull); arena exhaustion silently falls back | ordinary PostgreSQL errors |
| maturity | research prototype, not for production | production drivers |

### The paths inside fastpostgres (1M, bench shape)

| path | rows/s | use it when |
|---|---|---|
| four processes, DBAPI | 13.4M aggregate | one process cannot keep up |
| `stream()` + numpy | 8.3M | you want numbers, not rows |
| DBAPI / `core` | 7.2M | you want Python tuples |
| SQLAlchemy CORE | 1.4M | you want SQLAlchemy's API and can pay ~570 ns/row |

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

**"5× on six narrow columns proves nothing."** The mixed shape is the answer:
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

**"Which numbers should I quote?"** The committed logs, not the README:
`docs/fastpostgres-vs-arrow*.log`.
