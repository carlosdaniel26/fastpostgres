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
