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

The record is committed as `results/adoption-magicstack-pgbench.log` and indexed
in [`RESULTS.md`](RESULTS.md).
