# ClickBench: the universal workload

The rivals comparison, measured on a workload nobody here invented.

ClickBench is the analytical benchmark the engines publish against: one `hits`
table (105 columns), 43 fixed queries, a documented metric, and results for
ClickHouse, DuckDB, Postgres, DataFusion, Snowflake and dozens of others. The
schema and the query text are vendored verbatim in
[`rivals/clickbench/`](rivals/clickbench/) at a pinned commit; this document is
about how we run them and what the numbers mean.

## The players and the width

Both sides get the same machine, which is the whole point:

| player | one client is |
| --- | --- |
| fast-postgres | a connection plus its ring worker (one backend) |
| ADBC + Arrow | a connection, results landed as an Arrow table |
| psycopg 3 | a connection, results as Python tuples |

The harness runs **C clients of each player at once** (`--clients`, default: one
physical core less one), each walking the whole query set. It also sets
`FASTPOSTGRES_PARALLEL=1` for the run: with C clients already saturating the
cores, letting the ring fan a query out over more would hand fast-postgres cores
the rivals cannot match inside a single query.

## The two numbers

```
q/s      C x 43 / wall
rows/s   rows delivered / wall
```

`q/s` is ClickBench's metric and is what makes the result comparable to the
published tables. `rows/s` is the claim this repository actually makes, and it
is reported separately because most ClickBench queries aggregate: the executor
dominates them, and a q/s figure alone would hide the delivery difference under
an execution one.

Each query's `ms` is its fastest of `--reps` walks, summed — ClickBench's own
rule — and the per-query table prints the median client, so a single slow client
cannot flatter the total.

## The data

Fetched once per host by `rivals/clickbench/fetch.py`, into a git-ignored
directory. Upstream publishes one 16.3 GB `hits.tsv.gz` and no official small
sample at that path, so the fetcher streams it and keeps a **bounded prefix**:
the header and the first `--rows` rows (1,000,000 by default).

That is upstream's first N rows of the same file, not ClickBench's published
small dataset. A q/s from it is comparable to another run of this harness; it is
not comparable to the published 100M-row table, and every record carries the row
count and the sha256 of the file it measured.

## Running

```
just rivals clickbench                  # C clients, whole set
just rivals clickbench --clients 1      # the latency regime
just rivals clickbench --reps 3         # fastest of three per query
just rivals clickbench --only Q1 Q7     # a subset
```

## Adaptations

Deviations from upstream's own runner, stated rather than hidden:

- **Concurrency.** Upstream's page reports single-threaded and higher-concurrency
  cells; this harness reports the concurrency it ran, in its header, and the
  width is explicit in every record.
- **Prefix data.** See above: a slice, not the published dataset.
- **`FASTPOSTGRES_PARALLEL=1`.** The ring's per-query fan-out is off in the same
  width mode, so one client is one backend on every side.
- **The ring carries reads only.** Any query the extension cannot take falls
  back to libpq, and `fastpostgres` counts those reads (`libpq_reads`); a run
  that fell back is not a run of the ring.
