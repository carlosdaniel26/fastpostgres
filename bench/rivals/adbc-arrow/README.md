# ADBC + Arrow

The rival is the Arrow ecosystem's own answer to the same read: ADBC's
PostgreSQL driver, whose results arrive as Arrow record batches, with `pyarrow`
doing the arithmetic. Both sides are asked for the same output, so the
comparison prices the transport and not two different results.

## The two sides

| side | the code | how it reads |
|---|---|---|
| fastpostgres | the client itself: `python/src/fastpostgres/` | the DBAPI (typed tuples), `stream()` (zero-copy columnar batches, reduced in place with numpy), the lean `fastpostgres.core`, SQLAlchemy CORE, and a four-process read |
| ADBC + Arrow | `sqlalchemy_bench.py`, `arrow_memory_bench.py` | `adbc_driver_postgresql.dbapi`, then either `to_pylist()` / `fetchall()` for the tuple comparison, `pyarrow.compute` over the Arrow table, or `fetch_record_batch()` one batch at a time |

Both drivers are installed by the dev image, from PyPI, in one benchmark-only
layer (`docker/Dockerfile`: `pyarrow adbc_driver_postgresql` next to the client's
own dependencies). The versions a number was produced with are in the record's
own `### ENV` block, because that is the version that measured it; the image
prints them on every run.

## What lives here

- `sqlalchemy_bench.py` — the read comparison: **TUPLE READ** (the same Python
  tuples from both), **ZERO-COPY READ** (one column summed in place, each stack
  in its own columnar form) and the **PATHS** table (what fastpostgres's own
  wrappers cost against its driver read). Driven by `just rivals adbc`, which is
  `benchmarks.sh` in this directory, run inside the image.
- `arrow_memory_bench.py` — the same reads with the ring's transfer buffer fixed
  (`--mib`, 512 by default, one data segment): `ring` against `arrow-batch` and
  `arrow-table`, each reporting the bytes it carried and its peak RSS.
  Driven by `bench/harness/arrow_memory_bench.sh`.

## The rules this one holds to

- **Like against like.** The tuple comparison reads tuples on both sides and the
  columnar one compares columnar forms; fast-postgres's tuples against ADBC's
  Arrow table would price two different outputs.
- **One player per process, one at a time.** Interleaving them measures the
  neighbours' cache footprint — with the Arrow rival present, the row read came
  out ~20% below the same read measured alone (see `sqlalchemy_bench.py`'s
  docstring).
- **The output is checked** (the same column sum, and the same row count), and
  past 1M rows a single rep, because the client's object arena cannot hold
  several 10M-row reads in one process.

## What would flatter the rival

Its own terms are the batch size it chooses and the option to materialise the
whole result (`arrow-table`) instead of streaming it (`arrow-batch`); the memory
comparison exists because those two answers are not the same budget. Handing
ADBC a server-side cursor or a smaller batch would change what it holds, which is
what `arrow_memory_bench.py` reports rather than assumes.

## Where the argument is

[`../../ARROW-MEMORY.md`](../../ARROW-MEMORY.md) for the fixed-buffer numbers,
[`../../WHY-IT-WINS.md`](../../WHY-IT-WINS.md) for the mechanism and the
falsification predictions, [`../../../docs/LIMITATIONS.md`](../../../docs/LIMITATIONS.md) for the
small-result inversion — the regime where the plain driver read wins.
