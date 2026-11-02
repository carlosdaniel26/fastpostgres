# Benchmarks: three classes, what each answers, where its record lives

There are three classes of measurement here, and each answers a different
question. The commands are the entry points; every number the project quotes is
a line in a committed log under [`docs/`](.) with a `git:` stamp of the commit it
was produced from.

| class | command | the question it answers |
|---|---|---|
| workloads | [`just bench`](../README.md#benchmarks-and-meters) | does swapping the driver help an application - lookups, pages, scans, joins, aggregates, concurrency, writes - over a schema with foreign keys and indexes |
| rivals | `just rivals [target]` | how the ring compares with the other stacks (ADBC + Arrow, a result cache), at their defaults and under a fixed memory budget |
| internals | `just internals [target]` | what the row decode and the transport cost at the instruction, cache and port level - the evidence the design notes argue from |

Rules every class follows, because a measurement that breaks them is not one:

- **One player per process, one at a time.** Interleaving players measures the
  neighbours' cache footprint (see [`METHODOLOGY.md`](METHODOLOGY.md)).
- **The same output, checked.** Every comparison defines what "the same" means -
  Python tuples for tuples, one column summed in place for columnar, a digest
  over the rows for the workload suite - and fails the run when it disagrees.
- **Evidence per cell.** A player says how its work actually travelled: reads
  served by the ring against reads issued, the ring pool, the bytes it mapped,
  its peak RSS, the transfer segment it was given. A fallback to libpq that the
  timing would otherwise hide is printed.
- **A committed log, with its stamp.** `git describe --always --dirty` and the
  shape/scale, so a published number points back at the code and the data.

Nothing is deleted from the history: the old recipe names still exist as targets
of the dispatchers below, and their logs remain the record.

## 1. Workloads: `just bench [scenario...]`

```
just bench                                  # every scenario, scale 1
just bench lookup paged                     # only those
just bench --quick                          # a tenth of the scale, short windows
just bench --scale=2 --duration=30 --clients=8
```

The rival is **psycopg 3**, the plain driver the same application ran on before,
and both sides use the same app-facing call - `execute(...).fetchall()` - so the
comparison is the driver swap and nothing else; the other stacks are the rivals
class. The record is [`fastpostgres-vs-psycopg.log`](fastpostgres-vs-psycopg.log),
the chart is `just charts workloads`.

The schema is a small commerce database (`benchmark/workloads/schema.py`) at a
scale where 1 means 10k customers, 10k products, 1M orders and 3M order items:
four tables with primary and foreign keys, an `orders (customer_id, created_at)`
index for the page, a status index, an items-by-product index, and money as
`numeric` - which the row frame cannot carry natively, so the realistic reads
exercise the columnar frame the client falls back to. The data is generated, and
`orders.total` is not reconciled against the items to the cent; the shape is the
realistic part.

| scenario | class | shape | measured as |
|---|---|---|---|
| `lookup` | app | 200 primary-key lookups | statements/s, p50/p95/p99 |
| `paged` | app | 200 pages: `where customer_id = $1 order by created_at desc limit 100` | statements/s, p95 |
| `scan` | analytics | the two-table join, every row out | rows/s, 3 reps |
| `join` | analytics | `order_items x products`, 90% of the rows | rows/s, 3 reps |
| `aggregate` | analytics | `group by country` over the join | ms/read, 3 reps |
| `concurrent` | concurrency | 4 clients on the paged query for 10 s | statements/s, p50/p95/p99, ring-fallback rate |
| `mixed` | mixed | 90% paged reads and 10% order inserts, 10 s | reads/s, writes/s, p95 of each |

Each statement's rows are checked against its parameters (a lookup must return
the row it asked for; a page must be at most 100 rows with non-negative totals)
and folded into a digest the runner compares across players. The duration
scenarios compare per statement instead of by total, because a window that stops
on a clock does not issue the same number of statements twice.

The suite runs with the transfer segment at 64 MiB (`SEGMENT_MIB`), not the
client's 1 GiB default: an application's read is one batch, and 1 GiB per segment
would reserve 2 GiB of address space per client for a few MB of batch.

**What the committed run shows.** Three shapes, three answers:

- **Bulk delivery is the ring's.** The scan moves 2.21M rows/s against 0.84M and
  the join 2.18M against 1.26M, and every read was served by the ring - the log's
  `served_by_shm` equals `reads` in every cell.
- **A small statement is psycopg's.** A primary-key lookup is 4.7x slower on the
  ring (8.2K against 38.6K statements/s, p50 0.12 ms against 0.02), because each
  statement opens and closes a stream - two semaphore round trips - where psycopg
  pays one socket round trip. The page (2.2x) and the four concurrent clients
  (1.3x) show the same trade, and the page's p95 is where it hurts most: 1.03 ms
  against 0.23.
- **A parallel plan is neither stack's to win.** The aggregate is 2.7x slower on
  the ring (231.3 ms against 85.2), because the read runs as a serial plan in the
  background worker. A side measurement on the same data puts that query at 224 ms
  with `max_parallel_workers_per_gather = 0` and at 93 ms with it on: the ring's
  number is the serial one, and psycopg's backend is the one that gets the Gather.
  That is the cost of running the read inside a worker, and the suite measures it
  rather than hiding it.

Those three sentences are why this class exists: the transport benchmark says
"5-6x faster" and it is right about bulk reads, wrong about point reads, and
silent about plans. The per-cell evidence in the log says which path served each
read, and the README's workload table is this log's headline numbers.

## 2. Rivals: `just rivals [target]`

```
just rivals                  # list the targets
just rivals adbc             # the ADBC + Arrow comparison, unchanged
just rivals cache --quick
```

| target | the question | record | method and caveats |
|---|---|---|---|
| `adbc` | the read against the Arrow ecosystem's own answer, same output | [`fastpostgres-vs-arrow.log`](fastpostgres-vs-arrow.log), [`-mixed`](fastpostgres-vs-arrow-mixed.log), [`-small`](fastpostgres-vs-arrow-small.log) | [`METHODOLOGY.md`](METHODOLOGY.md), [`WHY-IT-WINS.md`](WHY-IT-WINS.md), [`LIMITATIONS.md`](LIMITATIONS.md) |
| `arrow-memory` | the same reads with the ring's buffer fixed: what each side holds | [`fastpostgres-vs-arrow-memory.log`](fastpostgres-vs-arrow-memory.log) | [`ARROW-MEMORY.md`](ARROW-MEMORY.md) |
| `cache` | a CDC-maintained result cache in front of the same server, its defaults | [`fastpostgres-vs-pgcache.log`](fastpostgres-vs-pgcache.log) | [`PG-CACHE.md`](PG-CACHE.md) |
| `cache-budget` | the same, with one memory budget on both sides | [`fastpostgres-vs-pgcache-memory.log`](fastpostgres-vs-pgcache-memory.log) | [`PG-CACHE.md#the-same-memory-budget-on-both-sides`](PG-CACHE.md#the-same-memory-budget-on-both-sides) |

## 3. Internals: `just internals [target]`

These are not benchmarks of the product. They are the meters behind
[`WHY-IT-WINS.md`](WHY-IT-WINS.md) and
[`CODEC-TRANSPORT.md`](CODEC-TRANSPORT.md) - how many instructions a row costs,
which ports the decode loop uses, what a format costs versus what a transport
costs - and their results live in the lab notebook rather than in this catalog.

| target | the meter | results in |
|---|---|---|
| `decode-cycles` | TSC cycles per row on a fixed shape | [`../simd/hypotheses.md`](../simd/hypotheses.md) |
| `decode-ir` | dynamic instructions, branch and cache misses, IPC (callgrind; profile image) | [`../simd/hypotheses.md`](../simd/hypotheses.md) |
| `decode-mca` | the decode loop's ports and latency, modeled offline (llvm-mca; profile image) | [`../simd/hypotheses.md`](../simd/hypotheses.md) |
| `codec-transport` | format cost and transport cost, separated (2x2 matrix) | [`CODEC-TRANSPORT.md`](CODEC-TRANSPORT.md), [`codec-transport.log`](codec-transport.log) |
| `cache-stat` | decodes per row and transports per KiB, from the host's PMU | [`cache-stat.log`](cache-stat.log) |

## Charts: `just charts [target]`

`workloads` · `arrow` · `arrow-memory` · `cache` · `cache-budget` · `codec` ·
`ab` - one figure per log, into `assets/` by default.

## Old names, and where they went

| the recipe you may remember | now |
|---|---|
| `just bench` (ADBC + Arrow) | `just rivals adbc` |
| `just pgcache-bench` | `just rivals cache` |
| `just pgcache-memory-bench` | `just rivals cache-budget` |
| `just arrow-memory-bench` | `just rivals arrow-memory` |
| `just decode-cycles`, `-ir`, `-mca` | `just internals decode-cycles` … |
| `just codec-transport` | `just internals codec-transport` |
| `just cache-stat` | `just internals cache-stat` |
| `just charts`, `just codec-charts`, … | `just charts arrow`, `just charts codec`, … |

## Which numbers should I quote?

The committed logs, never a sentence alone. For "does this help my app", the
workload log; for "how does it compare with the other stacks", the rival logs;
for "why", the internals notebook. The README carries the headline of each and
links the log it came from.
