# Measurement methodology

What this repository measures, how, and what the numbers can and cannot support.
`NOTES.md` is the decode's lab notebook; this file is the protocol for
the benchmark and the record of its threats to validity.

## The claims

| claim | evidence | command |
|---|---|---|
| fast-postgres returns Python rows faster than ADBC + Arrow does | TUPLE READ in `bench/results/fastpostgres-vs-arrow.log` (2.6–3.0×) | `just bench` |
| the columnar path is faster still, and faster than Arrow's columnar path | ZERO-COPY READ in the same log (3.5–3.6×) | `just bench` |
| the ranking survives a harder data shape (NULLs, long text, non-ASCII) | the same two sections in `bench/results/fastpostgres-vs-arrow-mixed.log` (2.7–3.4× and 3.5–3.8×) | `SHAPE=mixed just bench` |
| SQLAlchemy's `Row` construction, not the transport, is what makes the object path expensive | PATHS table (−61…75% against the driver read) | `just bench` |
| the row decode is stall-bound rather than issue-bound, and the stalls are branch mispredictions and first-touch writes | Ir/row, mispredicts/row, D1/LL misses, IPC | `just decode-cycles`, `just decode-ir`, `just decode-mca` |

## Systems under test

- **fastpostgres** (this repository): a PostgreSQL 18 extension plus a Python
  client. Row path: the worker publishes the executor's own tuple bytes into
  `/dev/shm` (`FPROWS`), and the client walks them in place, building Python
  values (typed tuples).
- **ADBC + pyarrow**: `adbc_driver_postgresql` (Arrow Database Connectivity over
  libpq) with `pyarrow`. Results arrive as Arrow record batches; no Python
  object is built per value.
- PostgreSQL 18.6, Python 3.13. psycopg 3 is used only for the harness's own DDL
  and inserts; it is not a measured player.

Every run's log carries the exact versions in its `### ENV` block: `cpu`, `ghz`,
`cores`, `postgres`, `python`, `sqlalchemy`, `pyarrow`, `adbc`, `shm`, `mem`.

## Workload

`fp_bench_rows` — one table, one shape, the same for every player:

| column | type | value |
|---|---|---|
| `id` | int4 | `g` |
| `sku` | text | `'SKU-' \|\| g` (5–7 bytes) |
| `name` | text | `'Product ' \|\| g` (9–12 bytes) |
| `price` | float8 | `g * 0.5` |
| `stock` | int4 | `g % 500` |
| `active` | bool | `g % 2 = 0` |

One `INSERT … SELECT generate_series(1, n)` per size, so the data is
deterministic and identical for every player. Sizes: 100K / 1M / 10M.

Two data shapes, selected with `SHAPE=` and recorded as `shape:` in the log:

- `bench` — the friendly shape above: short ASCII text, no NULLs.
- `mixed` — the same six columns with what a real table has: ~10% NULLs, text on
  both sides of the 126-byte varlena split (1–54 bytes and 130–200 bytes), and a
  non-ASCII value every eighth row. It exercises the null bitmap, both varlena
  headers and the text fallback path.

The read is the same statement for every player and every shape: `SELECT id,
sku, name, price, stock, active FROM fp_bench_rows` — no `ORDER BY`, no `WHERE`,
no parameters.

## Protocol

- **Isolation**: one player per process, one player at a time, never side by
  side. Interleaving them measured the neighbours — with the Arrow rival
  present, the fast-postgres row read came out ~20% below the same read
  measured alone. Each player's own report (`read_rows` cycles/row)
  stays in the log.
- **Warm read** before the timed ones (connections, rings, Arrow pools, plans);
  it is never timed.
- **Repetitions**: three, median reported. Above 1M rows, a single rep.
- **Timed region**: the read call itself, including everything the consumer must
  do to hold the result — the DBAPI's `fetchall`, ADBC's `fetch_arrow_table`
  plus the conversion to tuples, `stream()` iteration plus the numpy sum.
- **Verification**: every read's row count is checked against `n`; a mismatch
  prints `MISMATCH` and fails the run (the `ok` column).
- **Dispersion**: the log prints the spread over the reps (`±`, half the
  min–max range) under each row; `n=1` marks the single-rep sizes.
- **Provenance**: each log's `### ENV` block carries `shape:` and `git:` —
  `git describe --always --dirty`. A `-dirty` suffix means the run included
  uncommitted changes; commit and re-run for a clean stamp.

## Environment

Measured on an AMD Ryzen 7 7700 (8 cores / 16 threads, 30 GiB RAM), inside the
container this repository builds: Debian on `postgres:18`, `/dev/shm` 3 GiB.

Observed run-to-run spread on this host across repeated full runs: a few percent
for the driver-level numbers (fast-postgres's tuples ±2%, ADBC's ±3%) and up to
about ±10% for `stream()`, whose rate depends on the state of the shm segment's
pages. The `±` lines in each log show the *within-run* spread: typically under
2%, up to ~6% at the smallest size.

## Threats to validity

- **Single host, single run family.** Every number is one machine, one kernel,
  one container configuration. Clock frequencies drift with thermals; the TSC is
  invariant, so cycle figures are comparable across runs, but throughput is not
  portable.
- **ADBC → tuples is a conversion path.** ADBC's design output is Arrow; asking
  it for Python tuples means its Arrow conversion *plus* a column-wise
  `to_pylist`. That is the honest cost of the same output, but not the path ADBC
  is optimised for.
- **Comparable outputs, not comparable internals.** TUPLE READ compares object
  rows with object rows; ZERO-COPY READ compares columnar with columnar. No
  table compares tuples against an Arrow table — that would price two different
  outputs rather than the two transports.
- **SQLAlchemy is measured, not required.** The PATHS table shows it costs
  ~570 ns/row of `Row` construction on top of the driver; the two headline
  tables do not use it.
- **Two shapes, not every shape.** NULLs, both varlena header sizes and
  non-ASCII text are measured (`SHAPE=mixed`); wide rows, numeric/timestamps and
  TOAST-heavy values are not — see
  [`LIMITATIONS.md`](../docs/LIMITATIONS.md) for the full list of what the benchmark
  does not cover and what happens in those cases.
- **The decode meters are simulation.** `just decode-ir` runs callgrind, which
  simulates instructions and caches and never touches the PMU (which is also why
  it runs inside the container); `just decode-mca` models an address span with
  llvm-mca, which does not predict branches and models calls at a flat 100
  cycles. The IPC `decode-ir` prints is against TSC ticks, not core cycles.
- **No PMU counters.** `perf stat` is unavailable in the container. The
  equivalents above are simulation-based or TSC-based, and the difference
  matters — see `NOTES.md`.

## Reproducing

```bash
just test                   # the client test suite, in the image
just bench                  # 100K / 1M / 10M, log to stdout
just bench 100000 1000000   # any sizes
SHAPE=mixed just bench      # the harder data shape (NULLs, long and non-ASCII text)
just charts                 # runs the benchmark and renders both charts
just charts --log bench/results/fastpostgres-vs-arrow.log   # the committed run, re-rendered
just decode-cycles          # the decode's TSC cycles/row (seconds)
just decode-ir              # its dynamic instructions and miss profile (minutes)
just decode-mca             # its ports and latency, modelled offline (seconds)
```

The runs this repository quotes are committed at
`bench/results/fastpostgres-vs-arrow.log` (bench shape) and
`bench/results/fastpostgres-vs-arrow-mixed.log` (mixed shape), and the charts in
`assets/` are rendered from the first. `just rivals adbc > bench/results/my.log` keeps any other run
with its `### ENV` block attached.

The `bench-*.log` files at the repository root predate this harness — they carry
the psycopg2 comparison and the older section names — and are kept as history,
not as a baseline: `just charts --log <one of them>` will warn that the harness
changed rather than compare across it.
