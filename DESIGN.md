# The lazy default read path

## The problem

The measured fast paths (`stream()` + numpy, `fastpostgres.core`, and
`Cursor.aggregate`) are 3–6× the DB-API `fetchall()` read, but every one of them
was opt-in. The default path built a Python object per value while the ring was
still checked out, so an application that only counted, filtered, or forwarded a
result paid for tuples it never read.

This change makes the object-free representation the default for the reads that
can take it: a read-only SELECT still travels over /dev/shm, but the client now
copies the **columnar** frame out of the ring and decodes a tuple only when one
is asked for.

## The default

`Cursor.execute()` for a read-only SELECT (`_is_read_only`, the existing
predicate) calls `Connection._shm_read_lazy()` instead of `_shm_read()`:

1. `Ring.post(seg, batch)` asks the worker for the columnar frame into one of
   two alternating data segments. It is the columnar twin of `post_rows()`, so
   the next batch is already in flight while the current one is copied — the
   same pipelining the eager path used.
2. For each column, `bytes(column.data)` copies the whole column region out of
   /dev/shm (one memcpy per column, not per value). For the varlena kinds the
   `bytes` view is a slice of that same copy, so text is copied once.
3. The ring is closed and released immediately. The batch becomes a list of
   `_client.Column` objects (`_client.column(...)`) over buffers the result owns.
4. `execute()` hands the cursor the `ResultSet`; `fetchall()`, `fetchone()`,
   `fetchmany()`, `len()`, indexing, slicing, iteration and `keys()` behave like
   the old list. `_client.decode_row(columns, row)` builds one tuple in C for the
   row that was read.

`Connection.fetchall(...)` and `fastpostgres.core` use the same path; `aio` gets
it for free (the copy runs on the connection's tier thread and the returned
`ResultSet` is safe to decode on the loop thread, because it no longer aliases
the segment).

The metadata (`Cursor.description`, `ResultSet.keys()`, `field_types()`) comes
from the first columnar batch, which the worker writes even when the result is
empty.

Fallbacks are unchanged: if the extension is absent, the ring pool is exhausted,
or the statement is not a read, the read takes libpq and returns the old list of
tuples. A failed columnar read is re-run on libpq so the caller sees the real
error, exactly as `_shm_read` did.

## The escape hatch

Two equivalent switches restore the previous behaviour — a plain list of tuples,
decoded while the batch is in the segment:

- `FASTPOSTGRES_MATERIALIZE=1` in the environment, read once at import;
- `Connection(..., force_tuples=True)` / `aio.connect(..., force_tuples=True)`;
- `Connection.fetchall(statement, force_tuples=True)` and
  `Cursor.fetchall(force_tuples=True)` for one call.

The forced path is the old `_shm_read()` row-frame path, so it keeps the old
speed (~3.8M rows/s at 1M on this machine) and the old `read_rows` cycle
report. A lazy result can also be materialised after the fact with
`ResultSet.to_tuples()` (or `core.Result.to_tuples()`), which decodes the copied
columnar frame in C through the same specialized loops and measured *faster*
than the old row-frame decode (~4.7M rows/s).

## Lifetime rules

- `stream()` keeps its contract unchanged: its `memoryview`s alias the data
  segment and are valid only until that stream's next pull.
- A `ResultSet` owns everything it reads. The column bytes are copied before the
  ring is released, so the result is independent of the pool, of later queries,
  and of the connection's lifetime. A later read on the same ring cannot rewrite
  it (covered by `test_lazy_result_outlives_the_ring`).
- The cost is one copy of the result, not one object per value. Peak RSS for the
  1M-row read fell from 454 MB to 199 MB (setup included on both sides).
- `to_tuples()` caches the materialised list inside the result; after that,
  indexing and iteration use the list. Nothing else mutates a result after
  construction.

## What the A/B measured

`bench/results/team-architecture-ab.log` has the full record. At 100K/1M rows,
DBAPI went 3.76M→7.23M / 4.07M→8.24M rows/s and `core` 3.55M→7.10M /
3.96M→8.23M; four processes went 8.79M→17.45M / 9.63M→21.61M. `stream()` is
unchanged, as it should be. The suites (`tests.sh`, extension + 57 client tests)
are green.

## What remains

- **Row attribute access.** Indexing a `ResultSet` returns a tuple, not a
  `RowView` with `row.sku`; that wrapper is deliberately still absent. A lazy
  `RowView` could be added over `decode_row` without changing the storage.
- **Per-row iteration is lazier than it is fast.** `list(result)` decodes one
  row per call (~3.4M rows/s at 1M), while `to_tuples()` decodes a whole frame
  in C (~4.7M). A block-wise iteration (decode, say, 4K rows at a time) would
  close that gap but would build tuples the caller may never read; the current
  choice favours early-stopping consumers.
- **One copy per column.** The result could hold the ring lease until the
  result is released, avoiding the copy entirely, but that would let a lazy
  result occupy a pool partition indefinitely. Copying is the lifetime rule
  that makes an unbounded result safe.
- **libpq reads stay eager.** Only the shared-memory columnar frame has a
  cheap lazy form; a libpq text result still arrives as tuples, and a pooled
  ring that is unavailable falls back to it.
- **SQLAlchemy sees a sequence.** The dialect works because SQLAlchemy consumes
  `cursor.fetchall()` as a sequence (verified against 2.0.40), but a third-party
  consumer that mutates the returned list, or asserts `isinstance(rows, list)`,
  needs `force_tuples=True`.
- **No automatic numpy/Arrow view.** The default result is columnar but its
  public surface is row-shaped. `ResultSet` could expose a zero-copy column
  accessor, sharing `stream()`'s typed views.
