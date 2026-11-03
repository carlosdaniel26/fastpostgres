# PgCache

The rival is a result cache: a wire-compatible proxy in front of the same
PostgreSQL, caching query results and keeping them fresh from the origin's
logical replication stream. It removes *execution* on a repeated query; the ring
removes *delivery* on every query, so the two win different shapes and the
comparison is written to show which.

The proxy is **not in this tree** — it is the vendor's own Docker image
(`pgcache/pgcache`, pulled, its image id recorded in the log's `### ENV`), because
a cache is a product and not a driver. What is in the tree is everything needed
to compare against it fairly.

## The two sides

| side | the code | how it reads |
|---|---|---|
| fastpostgres | `player.py`'s `fastpostgres` player | the shm ring, via the driver's own DBAPI: one process, one ring, the same `SELECT` |
| PgCache | `player.py`'s `pgcache` player | psycopg 3 to the proxy on `127.0.0.1:6432`, in the origin container's network namespace |

Both players return Python tuples from the same table, and the run compares the
digest (count and the column sums) across them, so a cache that served something
else fails the run.

## The workloads

| workload | what it is | what it prices |
|---|---|---|
| `large` | the benchmark's six-column read, every row | delivery — where the ring wins |
| `small` | `count(*), sum(price), sum(stock)`, one row out | execution — where a materialized cache wins, by ~51x |
| `through` | the same six columns through a view | the proxy hop with no cache in front of it: PgCache forwards views |

## What lives here

- `player.py` — the comparison at the rival's defaults. `bench/harness/pgcache_bench.sh`
  drives it: the origin's table is built first, the proxy starts afterwards (its
  cache resets on startup, so no entry can predate this run's rows), the ring is
  timed with the proxy stopped, and one read after a proxy restart is timed as
  the cold fill.
- `memory_player.py` — the same players and workloads with an evidence line, for
  the equal-budget sweep: the ring under `FASTPOSTGRES_SEGMENT_BYTES` (two
  alternating segments, so a stream's shared memory is 2x it), the cache under
  the proxy's `--memory` cap with `MEMORY_LIMIT` and `MEMO_CACHE_SIZE` set to
  the same M. `bench/harness/pgcache_memory_bench.sh` drives it and reports a
  budget the cache cannot run at as that fact, not as a broken run.

## The rules this one holds to

- **A hit is proven, not assumed.** The warm phase reads until the proxy's own
  `pgcache_queries_cache_hit` counter has held still for two seconds — the state
  a cache is in between changes — and the log records how many reads and
  milliseconds that took, because the population latency is part of what the
  cache costs.
- **The fill is measured** (`--cold`: one read after a restart, a miss by
  construction), and the origin is started with `wal_level=logical` because that
  is what the proxy needs to follow it — the benchmark pays the same tax on both
  sides' behalf that a deployment would.
- **The budget is the evidence.** The ring reports the segment the worker
  actually mapped (the harness clears `/dev/shm` first) and the timed reads that
  took the ring; the cache reports the budget it resolved, its used bytes, its
  memo and whether pressure is throttling registration.

## What would flatter the rival

A larger `MEMORY_LIMIT`/`MEMO_CACHE_SIZE`, or a cache that could hold the large
result without being sized for the whole of it. That is why the budget sweep
exists: the default run says what each side does with its own defaults, the sweep
says what each buys with the same bytes. The cache is also given the benefit of
the warm-up rule above; a cache measured cold on every read would be a
different, weaker number.

## Where the argument is

[`../../PG-CACHE.md`](../../PG-CACHE.md) for both runs and the decision table
(read the rows you repeat → the cache; read the large result → the ring).
