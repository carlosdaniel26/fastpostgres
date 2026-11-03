# Workloads

The app-facing suite: realistic statements over a small commerce schema, the same
application on both drivers, and per-statement latency and throughput instead of
a rows/s headline. It is the psycopg comparison — the plain driver the
application ran on before — and it is also where the schema the other benches
reuse lives.

## The shape

`schema.py` builds `customers`, `products`, `orders` and `order_items` with
primary and foreign keys, the indexes a real application would have (an
`orders (customer_id, created_at)` index for the page, a status index, an
items-by-product index), and money as `numeric` — which the row frame cannot
carry natively, so the realistic reads exercise the columnar frame the client
falls back to. Scale 1 is 10k customers, 10k products, 1M orders, 3M order
items; `schema.setup(scale)` drops and rebuilds it.

`scenarios.py` holds the seven scenarios and their checks:

| scenario | shape | measured as |
|---|---|---|
| `lookup` | 200 primary-key lookups | statements/s, p50/p95/p99 |
| `paged` | 200 pages: `where customer_id = $1 order by created_at desc limit 100` | statements/s, p95 |
| `scan` | the two-table join, every row out | rows/s |
| `join` | `order_items x products`, 90% of the rows | rows/s |
| `aggregate` | `group by country` over the join | ms/read |
| `concurrent` | 4 clients on the paged query for 10 s | statements/s, ring-fallback rate |
| `mixed` | 90% paged reads and 10% order inserts, 10 s | reads/s, writes/s |

## The two sides

| side | how it reads |
|---|---|
| fastpostgres | `fastpostgres.dbapi`, the ring for read-only `SELECT`s and libpq for anything else |
| psycopg 3 | the image's own `python3-psycopg`; the application's call is identical on both |

`player.py` is one player × one scenario × one process, printing one JSON object
with its samples, digest and ring evidence; `runner.py` sets the schema up,
spawns the players one at a time, compares digests across them and prints the
record. `bench/harness/workloads_bench.sh` is the driver (`just bench`), and it
runs with the transfer segment at 64 MiB (`SEGMENT_MIB`), not the client's 1 GiB
default: an application's read is one batch, and 1 GiB per segment would reserve
2 GiB of address space per client for a few MB of batch.

## The rules this one holds to

- **The application is unchanged between players**: same schema, same statements,
  same parameters, same `execute(...).fetchall()`.
- **Every statement's rows are checked** against its parameters and folded into a
  digest the runner compares across players; a fallback to libpq that the timing
  would otherwise hide is printed.
- **Duration scenarios compare per statement**, because a window that stops on a
  clock does not issue the same number of statements twice.

The record is [`../results/fastpostgres-vs-psycopg.log`](../results/fastpostgres-vs-psycopg.log);
the catalog, including what the three answers in it mean (bulk is the ring's, a
small statement is psycopg's, a parallel plan is neither stack's to win), is
[`../README.md`](../README.md).
