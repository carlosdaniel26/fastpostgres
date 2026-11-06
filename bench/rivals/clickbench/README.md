# ClickBench, vendored

The universal workload for the rivals comparison. Everything here is upstream's,
unmodified; the harness that runs it lives beside this directory and is ours.

| file | what it is |
| --- | --- |
| `create.sql` | ClickBench's PostgreSQL schema for `hits` (105 columns), verbatim |
| `queries.sql` | ClickBench's 43 queries, verbatim, `Q`-numbered in order |
| `LICENSE` | upstream's licence, copied so the vendoring carries its own terms |

- **Source:** https://github.com/ClickHouse/ClickBench
- **Pinned at:** `e8c10d15e5da327e3f9d5aae152408514ca92fd0`
- **Files taken:** `postgresql/create.sql`, `postgresql/queries.sql`, `LICENSE`
- **Rewrite policy:** never edited. A schema or query change means re-vendoring at
  a new pin and recording it here, so a number can always be traced to the text
  that produced it.

## Why this workload

The rivals comparison used to read a six-column table this repository invented.
That made every ratio a claim about our own shape. ClickBench is the benchmark
the analytical engines publish against: one `hits` table, one query set, a
documented metric, and results for ClickHouse, DuckDB, Postgres, DataFusion and
dozens of others. A number from it can be checked against something a reader
already trusts.

## How it will be run

`just rivals clickbench` — the same three players, all at the same width:

| player | width |
| --- | --- |
| fast-postgres | one ring per core less one (`FASTPOSTGRES_PARALLEL`) |
| ADBC + Arrow | one connection per core less one, same ctid page ranges |
| psycopg 3 | one connection per core less one, same page ranges |

Both sides read the same rows over the same page ranges, so the difference
measured is delivery, not sharding.

**Two metrics, not one.** ClickBench scores queries per second over the whole
set. Most of its queries aggregate heavily, and the executor — not the transport
— dominates those, so q/s compresses the difference this project is about. The
harness reports both:

- `q/s` — ClickBench's own metric, so the result is comparable to the published
  table;
- `rows/s` per query — what the ring actually delivers, which is the claim this
  repository makes.

## The data

Upstream publishes one file, `hits.tsv.gz` (16.3 GB), and no official small
sample at that path. `fetch.py` streams it through gzip and keeps a bounded
prefix — the header and the first `--rows` data rows — then records the URL,
the row count and the sha256 of what it wrote in `data/source.json`.

```
python3 bench/rivals/clickbench/fetch.py               # 1,000,000 rows
python3 bench/rivals/clickbench/fetch.py --rows 200000 # cheaper smoke run
```

`data/` is git-ignored: the file is fetched once per host and re-used, and a
run calls `fetch.py` unconditionally because it is a no-op when the recorded
checksum still matches.

**What this slice is not:** it is upstream's first N rows of the same file, not
ClickBench's 1M-row "small" dataset, and a q/s figure from it is comparable to
another run of this harness — not to the published 100M-row table. Every record
carries the row count and the checksum so that distinction cannot be lost.

## Adaptations

Filled in as the harness lands; each one is a deviation from upstream's runner
and has to be stated. Nothing is listed yet because nothing has been adapted.
