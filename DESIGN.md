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
# Schema-specialized row-frame deformer

The client decodes the row frame (`FPROWS`) by walking PostgreSQL's own tuple
bytes (`python/src/fastpostgres/native/codec.cpp`, `fp_decode_tuples`). Before
this change the walk already hoisted the kind switch out of the row loop (one
specialised body per column per block of rows). This change specialises the
layout and the branches on top of that.

## What is specialised now

- **Per-batch schema, resolved once.** `AttLayout` is built from the frame's
  column descriptors: wire width (or varlena), tuple alignment, and the column's
  null-bit position (`t_bits` is at a fixed offset; the bitmap width follows
  `ncols`). `RowBlock` keeps each tuple's attribute base (`row + t_hoff`) and all
  offsets are relative to it, so every row can have a different `t_hoff` and the
  same layout still applies.
- **Static attribute offsets.** A fixed-width attribute whose predecessors in
  the block are all fixed-width and null-free has one constant offset, so its
  body reads `base + offset` directly instead of running the
  align/addlength chain per value. The offsets are re-resolved per block
  (cheap: one pass over `ncols`), because a row's nullness only becomes known as
  the frame streams in; the schema part is per batch.
- **Hoisted null test.** The tuple bitmap marks attributes that are *not* null.
  The block ANDs the bitmaps of its rows; a column whose bit survives has no
  null anywhere in the block and gets a body with no per-value null load or
  branch. Tuples without a bitmap contribute nothing (they have no nulls).
- **One varlena-header decode.** The old body asked for the data pointer, the
  data length and then the total size to advance the offset, which decoded the
  same header up to three times. `fp_varatt_size` decodes it once and hands the
  same total size to the offset chain.
- **Branchless header and alignment.** The 1-byte/4-byte varlena choice and
  `att_align_pointer`'s "align only when the first byte is zero" rule are now
  masks/cmovs. On a mixed batch both are data-dependent; on the benchmark shape
  they are not.
- **Block size 16** (was 8): the kind dispatch and the per-block layout pass are
  amortised over twice as many rows while the block still fits in L1.

No wire bytes changed. The row frame, the descriptors and the segment layout are
exactly as before.

## What cannot be specialised

- **Offsets after a varlena.** A text/bytea value's length is per row, so every
  attribute behind the first varlena has a data-dependent offset. Only the
  leading fixed-width / null-free prefix can be constant. The bench shape
  (`id, sku, name, price, stock, active`) makes `id` and `sku` static and leaves
  the other four on the walk.
- **A varlena at an unaligned offset.** `att_align_pointer` aligns a value only
  when its first byte is zero (the 4-byte header form); a 1-byte header is taken
  where it lies. When the schema offset is not already aligned, the actual
  offset depends on the value's own header, so the static path must fall back to
  the per-row rule.
- **A nullable column.** The null test cannot be removed or made truly
  branchless without constructing a value that is then thrown away: Python
  object birth has side effects (allocation, refcounts). It stays a branch, but
  only for columns that actually have a null in the block.
- **The object births.** `PyLong`, `PyFloat`, `PyUnicode` and `PyTuple` are the
  dominant cost of `read_rows`, and the walk is a small share of it. They cannot
  be specialised further without changing object identity, lifetime or the
  CPython ABI, which this driver does not do.

## What the measurement says

`bench/results/team-compilers-ab.log` is the A/B, three interleaved rounds per
shape. On this host the row's run-to-run spread is +/-2-4%, so the honest
reading is:

- bench shape: `read_rows` median **656.0 -> 633.2 cycles/row, -3.5%**
  (5.79M -> 6.00M rows/s equivalent). A direct 8-vs-16 block comparison was
  neutral on this shape, so the gain is the removed per-value null test and the
  static prefix for `id` and `sku`; with a spread this wide it is fair to call
  it 1-3% rather than take the top of the range.
- mixed shape: **739.3 -> 748.5 cycles/row, +1.2%**, i.e. a wash inside the
  spread. Here the branches that actually mispredict are inside
  `PyUnicode_FromStringAndSize`'s UTF-8 decode and the allocator, not in the
  walk; the null test that this change can hoist is a smaller share than the
  births it sits next to.

The deformer work removed is a few instructions per value against a decode
dominated by CPython object births, which is the same conclusion the project's
own meters reached for the earlier loop rewrite: instruction removal from this
loop does not move the row time, because the loop is not where the row's time
goes. The specialisation is kept because the bench shape does improve and
because it removes branches and dependent loads without changing behaviour,
while keeping the per-batch layout in one place for the parts that *can* be
precomputed.

---

# Schema-specialized row-frame deformer

The client decodes the row frame (`FPROWS`) by walking PostgreSQL's own tuple
bytes (`python/src/fastpostgres/native/codec.cpp`, `fp_decode_tuples`). Before
this change the walk already hoisted the kind switch out of the row loop (one
specialised body per column per block of rows). This change specialises the
layout and the branches on top of that.

## What is specialised now

- **Per-batch schema, resolved once.** `AttLayout` is built from the frame's
  column descriptors: wire width (or varlena), tuple alignment, and the column's
  null-bit position (`t_bits` is at a fixed offset; the bitmap width follows
  `ncols`). `RowBlock` keeps each tuple's attribute base (`row + t_hoff`) and all
  offsets are relative to it, so every row can have a different `t_hoff` and the
  same layout still applies.
- **Static attribute offsets.** A fixed-width attribute whose predecessors in
  the block are all fixed-width and null-free has one constant offset, so its
  body reads `base + offset` directly instead of running the
  align/addlength chain per value. The offsets are re-resolved per block
  (cheap: one pass over `ncols`), because a row's nullness only becomes known as
  the frame streams in; the schema part is per batch.
- **Hoisted null test.** The tuple bitmap marks attributes that are *not* null.
  The block ANDs the bitmaps of its rows; a column whose bit survives has no
  null anywhere in the block and gets a body with no per-value null load or
  branch. Tuples without a bitmap contribute nothing (they have no nulls).
- **One varlena-header decode.** The old body asked for the data pointer, the
  data length and then the total size to advance the offset, which decoded the
  same header up to three times. `fp_varatt_size` decodes it once and hands the
  same total size to the offset chain.
- **Branchless header and alignment.** The 1-byte/4-byte varlena choice and
  `att_align_pointer`'s "align only when the first byte is zero" rule are now
  masks/cmovs. On a mixed batch both are data-dependent; on the benchmark shape
  they are not.
- **Block size 16** (was 8): the kind dispatch and the per-block layout pass are
  amortised over twice as many rows while the block still fits in L1.

No wire bytes changed. The row frame, the descriptors and the segment layout are
exactly as before.

## What cannot be specialised

- **Offsets after a varlena.** A text/bytea value's length is per row, so every
  attribute behind the first varlena has a data-dependent offset. Only the
  leading fixed-width / null-free prefix can be constant. The bench shape
  (`id, sku, name, price, stock, active`) makes `id` and `sku` static and leaves
  the other four on the walk.
- **A varlena at an unaligned offset.** `att_align_pointer` aligns a value only
  when its first byte is zero (the 4-byte header form); a 1-byte header is taken
  where it lies. When the schema offset is not already aligned, the actual
  offset depends on the value's own header, so the static path must fall back to
  the per-row rule.
- **A nullable column.** The null test cannot be removed or made truly
  branchless without constructing a value that is then thrown away: Python
  object birth has side effects (allocation, refcounts). It stays a branch, but
  only for columns that actually have a null in the block.
- **The object births.** `PyLong`, `PyFloat`, `PyUnicode` and `PyTuple` are the
  dominant cost of `read_rows`, and the walk is a small share of it. They cannot
  be specialised further without changing object identity, lifetime or the
  CPython ABI, which this driver does not do.

## What the measurement says

`bench/results/team-compilers-ab.log` is the A/B, three interleaved rounds per
shape. On this host the row's run-to-run spread is +/-2-4%, so the honest
reading is:

- bench shape: `read_rows` median **656.0 -> 633.2 cycles/row, -3.5%**
  (5.79M -> 6.00M rows/s equivalent). A direct 8-vs-16 block comparison was
  neutral on this shape, so the gain is the removed per-value null test and the
  static prefix for `id` and `sku`; with a spread this wide it is fair to call
  it 1-3% rather than take the top of the range.
- mixed shape: **739.3 -> 748.5 cycles/row, +1.2%**, i.e. a wash inside the
  spread. Here the branches that actually mispredict are inside
  `PyUnicode_FromStringAndSize`'s UTF-8 decode and the allocator, not in the
  walk; the null test that this change can hoist is a smaller share than the
  births it sits next to.

The deformer work removed is a few instructions per value against a decode
dominated by CPython object births, which is the same conclusion the project's
own meters reached for the earlier loop rewrite: instruction removal from this
loop does not move the row time, because the loop is not where the row's time
goes. The specialisation is kept because the bench shape does improve and
because it removes branches and dependent loads without changing behaviour,
while keeping the per-batch layout in one place for the parts that *can* be
precomputed.
# Server-side associative reduce pushdown

`Cursor.aggregate()` used to ship every batch to the client and reduce it there
(`python/src/fastpostgres/native/aggregate.cpp`). The new path does the
associative reduction where the tuples already are and publishes only the
scalars. Same query, same API, same semantics; the payload becomes
`32 + 12*nops` bytes instead of the batch.

Measured: 16,251,040 B -> 1,176 B for `sum(int8) + count_all` over 2,000,000
rows at the driver's 100,000-row batch (-99.993%, 13,818x less), with the same
median time 11% lower (see `bench/results/team-programming-ab.log`).

## The op

`FASTPOSTGRES_OP_REDUCE = 7` (`extension/include/fastpostgres/ring.h`).
The request reuses the ring's single slot:

| field | meaning |
|---|---|
| `a1` | the stream name, which is also the data segment (the pool names both alike) |
| `a2` | the plan (below) |
| `batch` | rows to fetch, as for NEXT |

The worker validates `a1` against the ring name (a batch must never clobber the
ring), fetches one batch from the existing SPI cursor, accumulates, publishes a
tiny frame into `a1`, and returns the batch's row count in `r->nrows`. The
client keeps calling until `nrows` is 0, exactly as with NEXT.

No ring layout changed, so `FASTPOSTGRES_RING_MAGIC`/version stay at 05. An old
worker answers op 7 with `unknown op`; `dbapi.aggregate` recognises that on the
first request (no row was consumed) and falls back to the old client-side walk.
`FASTPOSTGRES_AGG_PUSHDOWN=0` forces the old path for debugging.

## The plan

```
<op>[:<i|n><column>][,<op>[:...]]
```

- op: `1` COUNT_ALL, `2` COUNT, `3` SUM, `4` MIN, `5` MAX — the same numbers
  the C++ module exports as `AGG_*`.
- column: `i<index>` (0-based; negative counts from the end) or `n<name>`.
  Names escape `,` and `\` with a backslash. Integer columns are explicit so a
  column literally named `1` is not an index.
- COUNT_ALL has no column and uses the frame's `nrows`.

Resolution happens against the cursor's `TupleDesc`, so the client needs no
metadata frame and the plan is self-describing. The grammar is one pass over a
`pstrdup`'d copy; a malformed plan, a missing column, a column out of range or
an unsupported kind is an `ereport(ERROR)` with a `reduce: ` message, translated
to `ProgrammingError` by the DB-API.

## The frame

`FPREDUCE` (`extension/include/fastpostgres/fastpostgres_shm.h`; both sides include the one header):

```
"FPREDUCE", version u32, flags u32, nops u32, pad u32, nrows u64
per op: is_null u8, is_float u8, pad[2], value u64 (int64 or IEEE double bits)
```

`is_null` is only set by an empty MIN/MAX. SUM/COUNT write their identities (0)
so combining per-batch scalars is plain addition. `is_float` selects the
Python constructor; COUNT/COUNT_ALL are always integers. The client parser is
`_client.read_reduce(seg)` in `aggregate.cpp`; it builds a handful of Python
objects per batch, never per row.

## Semantics and lifetimes

The worker loop is the same shape as the C++ one: NULLs are skipped by
SUM/MIN/MAX and not counted by COUNT; integer sums accumulate in `unsigned long
long` and wrap (the server's `sum(bigint)` is `numeric` and exact — this path
inherits that existing caveat); `bool` sums as 0/1; `numeric` becomes `float8`
via `numeric_float8`, as the columnar encoder already did. MIN/MAX over floats
are ordered doubles. `dbapi.aggregate` combines per-batch results identically
on both paths (`_agg_combine`).

Only the planned columns are read (`heap_getattr`), so a sum over one column of
a wide result never deforms the rest.

The frame lives in the data segment the client already maps; its lifetime is
the usual one: valid until the next request on that ring. The client parses it
immediately, before the next request, so no copy is needed. The plan and the
accumulators are per request, allocated in the SPI context and released at
`SPI_finish`; nothing survives the request.

**Error path fix.** Exercising repeated rejections exposed a latent bug in
`worker.c:ring_set_error`: `PG_CATCH` runs with `CurrentMemoryContext ==
ErrorContext`, and `CopyErrorData()` allocates in the current context. The old
code then called `FlushErrorState()` (which resets ErrorContext) and
`FreeErrorData()` on the freed pointer — heap corruption that showed up as a
segfault on the second rejected request. `ring_set_error` now copies in
`TopMemoryContext` and each `PG_CATCH` restores the pre-request context before
handling, so a rejected plan leaves the worker healthy and the ring reusable.

## Generalising to a DAG

The frame and op are already a fold, not a special case. A plan is an ordered
list of leaves; to make it a DAG add two things:

1. **A node type in the plan.** Replace the flat entry with a small
   postfix/RPN program: `SUM <col>`, `MUL <k>`, `ADD`, `MIN`, `PROJECT`. The
   parser becomes a stack machine; the accumulators become the stack.
2. **A partial-aggregate slot kind.** Today a slot is a scalar. To combine
   child nodes (or several workers), add a variable-sized slot — kind `partial`
   carrying count/sum/min/max, the classic four-value aggregate state — and a
   `COMBINE` op that folds two partials. That is also what a partitioned or
   parallel plan needs; the per-batch combine in `_agg_combine` is the
   single-node version of it.

The rest is unchanged: the worker still fetches a batch, the client still folds
`nops` results per batch, and the frame is still `O(nodes)`, not `O(rows)`.
COUNT_DISTINCT, AVG (sum+count), variance (Welford state) and GROUP BY (a hash
of group key -> partial state) all fit that shape.

## What remains

- **One column, numbers and bool.** Text, dates, numeric (beyond the existing
  lossy float8 conversion) and arrays are still client-side or SQL-side work.
- **No grouping, no DISTINCT, no AVG/variance.** AVG is two ops away, but the
  plan has no divide; DISTINCT needs a hash state and a spill policy.
- **Single worker.** The pushdown reduces one batch per request; partitioning
  the table across rings would need the partial-aggregate slot above.
- **SUM overflow still wraps at int64**, matching the old path rather than
  PostgreSQL's exact `numeric` sum.
- The client-side C++ reduce is kept as the fallback and as the reference used
  by `test_pushdown_matches_the_client_side_reduce`.

---

# Server-side associative reduce pushdown

`Cursor.aggregate()` used to ship every batch to the client and reduce it there
(`python/src/fastpostgres/native/aggregate.cpp`). The new path does the
associative reduction where the tuples already are and publishes only the
scalars. Same query, same API, same semantics; the payload becomes
`32 + 12*nops` bytes instead of the batch.

Measured: 16,251,040 B -> 1,176 B for `sum(int8) + count_all` over 2,000,000
rows at the driver's 100,000-row batch (-99.993%, 13,818x less), with the same
median time 11% lower (see `bench/results/team-programming-ab.log`).

## The op

`FASTPOSTGRES_OP_REDUCE = 7` (`extension/include/fastpostgres/ring.h`).
The request reuses the ring's single slot:

| field | meaning |
|---|---|
| `a1` | the stream name, which is also the data segment (the pool names both alike) |
| `a2` | the plan (below) |
| `batch` | rows to fetch, as for NEXT |

The worker validates `a1` against the ring name (a batch must never clobber the
ring), fetches one batch from the existing SPI cursor, accumulates, publishes a
tiny frame into `a1`, and returns the batch's row count in `r->nrows`. The
client keeps calling until `nrows` is 0, exactly as with NEXT.

No ring layout changed, so `FASTPOSTGRES_RING_MAGIC`/version stay at 05. An old
worker answers op 7 with `unknown op`; `dbapi.aggregate` recognises that on the
first request (no row was consumed) and falls back to the old client-side walk.
`FASTPOSTGRES_AGG_PUSHDOWN=0` forces the old path for debugging.

## The plan

```
<op>[:<i|n><column>][,<op>[:...]]
```

- op: `1` COUNT_ALL, `2` COUNT, `3` SUM, `4` MIN, `5` MAX — the same numbers
  the C++ module exports as `AGG_*`.
- column: `i<index>` (0-based; negative counts from the end) or `n<name>`.
  Names escape `,` and `\` with a backslash. Integer columns are explicit so a
  column literally named `1` is not an index.
- COUNT_ALL has no column and uses the frame's `nrows`.

Resolution happens against the cursor's `TupleDesc`, so the client needs no
metadata frame and the plan is self-describing. The grammar is one pass over a
`pstrdup`'d copy; a malformed plan, a missing column, a column out of range or
an unsupported kind is an `ereport(ERROR)` with a `reduce: ` message, translated
to `ProgrammingError` by the DB-API.

## The frame

`FPREDUCE` (`extension/include/fastpostgres/fastpostgres_shm.h`; both sides include the one header):

```
"FPREDUCE", version u32, flags u32, nops u32, pad u32, nrows u64
per op: is_null u8, is_float u8, pad[2], value u64 (int64 or IEEE double bits)
```

`is_null` is only set by an empty MIN/MAX. SUM/COUNT write their identities (0)
so combining per-batch scalars is plain addition. `is_float` selects the
Python constructor; COUNT/COUNT_ALL are always integers. The client parser is
`_client.read_reduce(seg)` in `aggregate.cpp`; it builds a handful of Python
objects per batch, never per row.

## Semantics and lifetimes

The worker loop is the same shape as the C++ one: NULLs are skipped by
SUM/MIN/MAX and not counted by COUNT; integer sums accumulate in `unsigned long
long` and wrap (the server's `sum(bigint)` is `numeric` and exact — this path
inherits that existing caveat); `bool` sums as 0/1; `numeric` becomes `float8`
via `numeric_float8`, as the columnar encoder already did. MIN/MAX over floats
are ordered doubles. `dbapi.aggregate` combines per-batch results identically
on both paths (`_agg_combine`).

Only the planned columns are read (`heap_getattr`), so a sum over one column of
a wide result never deforms the rest.

The frame lives in the data segment the client already maps; its lifetime is
the usual one: valid until the next request on that ring. The client parses it
immediately, before the next request, so no copy is needed. The plan and the
accumulators are per request, allocated in the SPI context and released at
`SPI_finish`; nothing survives the request.

**Error path fix.** Exercising repeated rejections exposed a latent bug in
`worker.c:ring_set_error`: `PG_CATCH` runs with `CurrentMemoryContext ==
ErrorContext`, and `CopyErrorData()` allocates in the current context. The old
code then called `FlushErrorState()` (which resets ErrorContext) and
`FreeErrorData()` on the freed pointer — heap corruption that showed up as a
segfault on the second rejected request. `ring_set_error` now copies in
`TopMemoryContext` and each `PG_CATCH` restores the pre-request context before
handling, so a rejected plan leaves the worker healthy and the ring reusable.

## Generalising to a DAG

The frame and op are already a fold, not a special case. A plan is an ordered
list of leaves; to make it a DAG add two things:

1. **A node type in the plan.** Replace the flat entry with a small
   postfix/RPN program: `SUM <col>`, `MUL <k>`, `ADD`, `MIN`, `PROJECT`. The
   parser becomes a stack machine; the accumulators become the stack.
2. **A partial-aggregate slot kind.** Today a slot is a scalar. To combine
   child nodes (or several workers), add a variable-sized slot — kind `partial`
   carrying count/sum/min/max, the classic four-value aggregate state — and a
   `COMBINE` op that folds two partials. That is also what a partitioned or
   parallel plan needs; the per-batch combine in `_agg_combine` is the
   single-node version of it.

The rest is unchanged: the worker still fetches a batch, the client still folds
`nops` results per batch, and the frame is still `O(nodes)`, not `O(rows)`.
COUNT_DISTINCT, AVG (sum+count), variance (Welford state) and GROUP BY (a hash
of group key -> partial state) all fit that shape.

## What remains

- **One column, numbers and bool.** Text, dates, numeric (beyond the existing
  lossy float8 conversion) and arrays are still client-side or SQL-side work.
- **No grouping, no DISTINCT, no AVG/variance.** AVG is two ops away, but the
  plan has no divide; DISTINCT needs a hash state and a spill policy.
- **Single worker.** The pushdown reduces one batch per request; partitioning
  the table across rings would need the partial-aggregate slot above.
- **SUM overflow still wraps at int64**, matching the old path rather than
  PostgreSQL's exact `numeric` sum.
- The client-side C++ reduce is kept as the fallback and as the reference used
  by `test_pushdown_matches_the_client_side_reduce`.
