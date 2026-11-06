# Combined design notes

Team design records merged on `merge/read-path`. Each section is the team's own document, kept intact.

# H8-A — the allocation is the frame

`fastpostgres_columnar_build` used to `palloc` a row-major `Datum`/`bool` scratch
(`8 B + 1 B` per column per row — 7.2 MiB at 100k x 6), deform every tuple into
it, and then transpose it into the FPCOLUMN payload one column at a time. Every
value was touched twice, and every chunk paid a fresh multi-MiB allocation (and
its first-touch faults). This is the `bench/NOTES.md` "allocation finding".

This change implements **H8 shape A**: the column regions are reserved up front
and values are written straight into the mmap, once. The pull protocol, the
column order and the wire bytes are unchanged.

## What is built

`extension/src/columnar.c` now:

1. resolves every column once into a `ColumnPlan` (kind, width, varlena flag,
   output function);
2. estimates each text column's byte area from the first 8 rows
   (`max sampled value + 16 B` per row, floor 24 B/row);
3. lays out every column header and its reserved data region before the first
   value is written, backpatching `data_len` through the existing
   `fastpostgres_columnar_write_*` helpers;
4. deforms each row **once** into a reused row-sized `Datum`/`bool` array and
   writes each value into its column's region — fixed widths at
   `data + row * width`, text bytes at the column's running byte pointer and
   offset table;
5. compacts each column left by the unused part of its reservation, rewriting
   `data_len` to the exact used size and zeroing only the alignment pad.

The final bytes are the old frame's bytes: same magic/version, same column
order, same header fields, same exact `data_len`, same alignment padding, same
null bitmaps. Readers need no change and the client's generators are untouched.
The shared `fastpostgres_columnar_write_column` / `write_end` helpers remain the
only layout authority.

## Varlena strategy

Both options from the H8 note are present, in this order:

- **Reserved capacity per text column (fast path).** The sample-based estimate
  is a reservation, not the frame length. A text column that grows past it (or
  a reservation that does not fit the segment) returns from `columnar_fill`;
- **one exact sizing pass (retry).** The retry walks the same tuples, sums only
  the varlena byte lengths, and fills again with exact capacities. Because the
  capacities are then exact, the compaction is a no-op and the frame is the
  same. Fixed-width-only batches never sample and never size: their regions are
  exact from the start.

Sampling plus retry was chosen over "reserved capacity with slack in
`data_len`" because the frame must stay byte-identical, and over a mandatory
sizing pass because the measured cost of that extra full deform (about
16 ns/row on the benchmark shape) was larger than the scratch it removed.

The detoast/output-function temporaries (`pg_detoast_datum_packed`,
`OidOutputFunctionCall`) now live in a per-build memory context that is reset
between passes and deleted at the end, rather than accumulating in the SPI
context.

## Measured

Committed A/B: `bench/results/team-databases-ab.log`. Same commands, same host,
baseline `fpq-baseline` against `fpq-databases`, one at a time under
`/tmp/fpq-bench.lock`.

| | baseline | H8-A | |
|---|---|---|---|
| `benchmarks.sh 100000`, zero-copy `stream()` | 8.27M rows/s | **9.37M** | +13.3% |
| `benchmarks.sh 100000`, PATHS `stream()` | 8.19M | **9.41M** | +14.9% |
| encode isolation, `ring.next` (columnar) | 112.6 ns/row | **103.5** | -8.1% |
| encode isolation, `ring.next_rows` control | 34.2 | 34.2 | flat |

The isolation probe streams 100k-row batches from a plain table scan and times
`ring.next` (fetch + `columnar_build` + publish); `ring.next_rows` is the
control because it writes the same rows without the columnar transpose. The
columnar overhead over the row frame fell from ~78 to ~69 ns/row. The client
`read_rows` line is unchanged within noise.

`tests.sh` is green: `make installcheck` plus 55 client tests, including a new
regression for text that grows after the capacity sample (the exact-size retry)
and NULL text values.

The frame is also byte-identical, not merely equivalent: hashing every exposed
byte (name, kind, pgtype, data area, offset+byte areas and null bitmap) for the
benchmark shape, a NULL-heavy shape, the growing-text retry and the binary kinds
gives the same sha256 on `fpq-baseline` and `fpq-databases` for all four.

## What remains

- **H8-B (streaming DestReceiver).** Shape A still keeps SPI's `SPI_tuptable`
  (a tuplestore of `HeapTuple`s) for the batch. A custom `DestReceiver` feeding
  chunks per column region would remove the tuplestore as well, but it is a
  protocol change: `ring.next()` becomes a pull-driven queue over
  `push_ready`/`push_free`. That is the next H8 step, not this slice.
- **A column offset table** (the other half of H8-A's description) would remove
  the compaction move by making text regions independently addressable, but it
  changes the wire format; this slice deliberately did not.
- **The compaction copy** is ~3-4 B/row of moved payload in the benchmark shape.
  A capacity estimate tight enough to avoid it, or per-text-column sizing from
  the previous chunk, is a micro-optimisation on top.
- **Per-value text temporaries** still exist for toasted/compressed values and
  output-function fallbacks (numeric, jsonb, name, …); they are now owned by the
  per-build context, but the allocations themselves are still per value.
- **H5 (64-byte column alignment)** now falls out naturally if the reservation
  is made to 64 B; it is not part of this change and the wire still promises
  8 B.
- The two text columns in the benchmark still take one memcpy from the varlena
  into the payload. Removing it needs the value to arrive already in its final
  place (H9's buffer lease) or a dictionary (H10); neither is local to the
  encoder.

---

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
