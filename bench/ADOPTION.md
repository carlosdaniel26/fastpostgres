# Adoption: what a published suite found

Every other class in this tree measures the questions the project chose. This
one measures someone else's: a benchmark suite published and maintained by
others, vendored verbatim at a pinned commit, run on its own protocol, with
fastpostgres added to its roster as one more cell. It is the difference between
"our benchmark says we win" and "their benchmark, their queries, here is the
line".

## The suite

**[MagicStack/pgbench](https://github.com/MagicStack/pgbench)** (MIT) - the
PostgreSQL client-driver toolbench whose numbers stand behind asyncpg's
documentation. Its seven query files are deliberately small and varied, and they
are the opposite of this repository's home turf: a type-table read, a
`generate_series`, a 1000-row bytea table, an `int[]` table, a `COPY`, a 1000-row
batched `INSERT`, and `SELECT 1+1` - one statement, one row, ten times a second
per connection. A transport that saves client-side parsing shows up in the
first four and mostly does not show up in the last two.

Its protocol is its own and is kept as its own: each cell runs in a process of
its own, warms up, then runs a timed window at a fixed concurrency, reporting
queries, rows and a latency histogram in hundredths of a millisecond.

## Our cell, in its roster

`fastpostgres-dbapi` is the client's own DBAPI, executed through the suite's
Python runner contract: the adapter
([`adoption/players/pgbench_fastpostgres`](adoption/players/pgbench_fastpostgres))
connects, executes and returns the row count, and hands the suite's own
`runner()` the result, so the warmup, the timed loop, the concurrency and the
JSON are the suite's code. The roster is extended by
[`adoption/run.py`](adoption/run.py), not edited into the vendored tree;
[`adoption/README.md`](adoption/README.md) is the class's own README and states
every adaptation.

The rivals in the cell set are the suite's own Python roster: `asyncpg`,
`aiopg`, and `psycopg3`'s async path. Its Go and Node cells need toolchains the
dev image does not carry, so no class here names them.

## What the first record shows

One run, this machine, the dev image, 10 connections, a 10-second window per cell
after a 3-second warmup; queries/s, their protocol. Best cell in bold.

| query, concurrency 10 | aiopg | aiopg-tuples | psycopg3-async | asyncpg | fastpostgres |
|---|---|---|---|---|---|
| `1-pg_type` (a type-table read) | 708 | 3,525 | 1,686 | 3,901 | **4,100** |
| `2-generate_series` (1,000 rows) | 1,812 | 10,381 | 3,298 | 6,304 | **11,806** |
| `3-large_object` (100 x 1 KB bytea) | 3,806 | 5,124 | 2,827 | **22,069** | 7,997 |
| `4-arrays` (100 x `int[]`) | 2,612 | 3,137 | 2,577 | 5,954 | **17,359** |
| `5-copyfrom` | 109 | 110 | 112 | **258** | not supported |
| `6-batch` (1,000-row `INSERT`) | 188 | 196 | 28 | **895** | 5.4 |
| `7-oneplusone` (`SELECT 1+1`) | 49,526 | 59,937 | 29,002 | **82,416** | 42,718 |
| geometric mean | 1,441 | 2,579 | 1,206 | **4,831** | 3,405 |

**The ring wins the reads with structure in them.** `4-arrays` is 2.9x asyncpg
(int[] takes the columnar frame, and 100 rows of 100 arrays land as typed
buffers), `2-generate_series` 1.87x, and `1-pg_type` is a wash at +5%. These are
the shapes where delivery is the work.

**It loses two cells badly, and the losses are informative.** `7-oneplusone` is
42.7k against 82.4k: one row per statement, ten statements in flight per
connection, every one of them opening and closing a stream - the same trade the
workload suite's `lookup` cell measures, here against asyncpg's prepared
pipeline. `6-batch` is 5.4 queries/s against asyncpg's 895: that cell is not the
ring at all, it is the client's DML path (libpq, one row at a time), and a
1000-row `executemany` taking 1.9 seconds says the client has no batch insert
path worth the name. The same cell is also psycopg3-async's worst (28 queries/s),
so it is a driver feature this suite prices rather than an artefact.

**`3-large_object` is the interesting loss**: 100 rows of 1 KB `bytea` per
statement, and the ring is 2.8x behind asyncpg (8.0k against 22.1k). The ring's
per-statement cost is paid per *statement*, not per row, and this query is 100 KB
of payload under the same fixed overhead - the ratio between the small-statement
regime and the bulk one, on the suite's own query.

**So the geometric mean puts the ring second of five** (3,405 against asyncpg's
4,831, and ahead of aiopg-tuples' 2,579), and the ranking is not the point: six
of the seven queries are ones this project would not have chosen, one of them
cannot run at all, and the two it loses worst are where the work is, not where
the transport is.

## What this class can and cannot say

- **It can say what their queries find**, including where the ring loses - a
  one-row `SELECT 1+1` per statement is the regime where opening a stream costs
  more than a socket round trip, and the record's own numbers show it.
- **It cannot check values, only row counts.** The suite's protocol does not
  digest what came back; the classes here that do (the workload suite, the ADBC
  comparison) are the ones to read for that.
- **It is one query set, not a verdict.** Seven queries on a fixed schema,
  one concurrency, one machine; the same caveats as everywhere else in this tree
  apply, and [`METHODOLOGY.md`](METHODOLOGY.md) is where they are written down.
- **Adaptations are the class's, and are listed.** The suite is 2023 code on a
  2025 image: its own pinned driver versions do not build on Python 3.13, and one
  call site expects an event loop to be created implicitly. Both are dealt with
  in the open - see [`adoption/README.md`](adoption/README.md) - rather than by
  editing the suite.

## The record

```bash
just adoption magicstack-pgbench            # the Python roster + our cell
just adoption magicstack-pgbench --quick    # warmup 1s, duration 3s
```

The record is committed as
[`results/adoption-magicstack-pgbench.log`](results/adoption-magicstack-pgbench.log)
and indexed in [`RESULTS.md`](RESULTS.md).
