# DESIGN — the storage-level columnar path (H9/B2)

## What this is

H9's original shape was a *lease* of PostgreSQL heap pages: pin a buffer, hand
the client the page, let it read the tuple bytes where they lie.  That cannot be
built.  A shared buffer is an anonymous `mmap` with no descriptor and no path;
there is no object to pass, and the pin/COW protocol would still leave the
client with row-major pages, line pointers, `t_hoff`, per-value alignment and
1-byte varlena headers — a second deformer, not a columnar scan.

The surviving shape is the one H9 itself names as the alternative: **make the
storage columnar**.  This branch implements the smallest real slice of that
idea:

1. `fastpostgres_columnar_load(query, capacity)` runs a read-only SELECT once
   and writes every value into its column's run in an anonymous **memfd**,
   using the existing FPCOLUMN layout.
2. The memfd is sealed immutable and described to the client as
   `FPQCOL1 <backend pid> <fd> <bytes> <rows>`.
3. The client opens `/proc/<pid>/fd/<fd>` read-only, maps it once, and scans
   the columns in place: zero-copy column views, a C++ reduce, and a full-byte
   scan.  No server round trip, no transpose, no per-row copy, no scratch.

It is **not** a table AM.  There is no catalog entry, no `relfilenode`, no
`pg_class` row, no tuple headers, no MVCC metadata, no WAL, no buffer manager,
no planner integration.  It is a bulk-loaded, read-only snapshot, and it is
honest about that.  What a real AM would have to add is listed at the end.

## Storage layout

The memfd contains exactly one FPCOLUMN v2 payload
(`extension/include/fastpostgres/fastpostgres_shm.h`):

```
"FPCOLUMN"  version u32  flags u32  ncols u32  nrows u64
per column:
    name_len u32, name bytes, kind u8, pgtype u32
    null bitmap        ceil(nrows/8) bytes, bit set = NULL
    data_len u64
    pad to 8-byte alignment relative to the payload base
    data:
      fixed-width kind  nrows * kind_width bytes, in row order
                        (a NULL keeps its zero-filled slot, so
                         data_len == nrows * width)
      text / bytea      (nrows + 1) * int32 offset table,
                        then the concatenated value bytes
```

The client's existing reader (`fastpostgres_reader_init` /
`fastpostgres_reader_next`) consumes this with no changes.  The fixed-width
runs are 8-byte aligned so a consumer can view them as a typed array; the
`Column.__array_interface__`, `.offsets` and `.bytes` accessors work unchanged.
Kind mapping and widths live in the same header both sides include, so there is
one source of truth.

The payload is built by `fastpostgres_columnar_build()` — the same function the
ring worker uses for a batch — directly into the memfd mapping.  The load path
does **not** build a frame and then copy: the memfd is the destination.

## memfd lifetime and the capability

The backend does:

```
memfd_create("fastpostgres-columnar", MFD_CLOEXEC | MFD_ALLOW_SEALING)
ftruncate(fd, capacity)                 capacity: 0 = 1 GiB default, max 16 GiB
mmap(PROT_READ|PROT_WRITE, MAP_SHARED)
madvise(MADV_HUGEPAGE)                  before the first write
SPI_execute(query, read_only=true)      one materialized SPI tuptable
fastpostgres_columnar_build(...)        one pass into the mapping
ftruncate(fd, exact_payload_bytes)      release the virtual slack
F_ADD_SEALS(F_SEAL_SHRINK | F_SEAL_GROW)
munmap()
```

The descriptor stays open in a 64-entry process-local table for the backend's
lifetime.  PostgreSQL closes it at backend exit.  The client's `open` of
`/proc/<pid>/fd/<fd>` is the capability; the mapping it creates holds the
inode, so an already-attached client keeps reading after the backend exits.
A client that has the descriptor string but no open mapping must attach while
the backend is alive.

This requires the client and backend to be the same UID (the container runs
both as `postgres`); `/proc/<pid>/fd` is gated by ptrace-read permission.
`load()` therefore attaches eagerly, while the backend is certainly alive.
A future version should hand the fd over an `AF_UNIX` `SCM_RIGHTS` socket
(the pattern team/formats uses for its per-batch segment) instead of relying
on `/proc`; that is the cleaner capability transfer and removes the UID rule.
It is not in this slice.

`F_SEAL_WRITE` is set only after the build mapping is dropped: the seal refuses
to attach while a writable mapping exists, so the code unmaps first and then
seals.  After that the object is immutable to everyone, including a same-UID
process that can open the descriptor; the client's mapping is read-only and the
fd cannot be mapped writable.  The descriptor, not the mode bits, remains the
capability.

## What was measured

`bench/results/team-os-ab.log`, 100K rows, 3 alternating runs per image,
medians:

| path | result |
|---|---|
| baseline heap + frame, `Cursor.aggregate` | 7.38M rows/s, 13.54 ms |
| bulk-loaded memfd, `ColumnarTable.reduce` | **80.44M rows/s, 1.24 ms — 10.9x** |
| baseline heap + frame, `stream()` views | 8.29M rows/s, 12.06 ms |
| memfd full-byte scan (`scan_all`) | 127.65M rows/s, 0.78 ms, 4.95 GB/s |
| one load (server pass + map) | 12.39 ms |
| load + 1 reduce | 13.63 ms vs 13.54 ms — 0.7% slower (a wash) |
| load + 2 reduces | 14.87 ms vs 27.08 ms — 1.82x |
| load + 10 reduces | 24.82 ms vs 135.42 ms — 5.46x |

Break-even is about one extra scan.  The store is 3,877,790 bytes, 0.51x the
7,651,328-byte heap relation (tuple headers, line pointers, alignment padding
and the heap's text storage are not carried).

The full-byte scan reads every value byte in place and is 15.4x the baseline
`stream()` — but that comparison is one-sided: `stream()` only builds column
views without touching the value bytes, while `scan_all` reads all of them.
It is kept as a ceiling, not as the headline.  The like-for-like claim is the
reduce: both sides reduce the same numeric columns with the same C++ code and
the result is checked equal (`verify.reduce: ok`).

Client page faults: 3 minor during load's first map, 1 minor on the first
full scan, 0 on every warm scan.  The small count is THP: the memfd is advised
`MADV_HUGEPAGE` before its first write, and on this host `/dev/shm` is
`huge=advise`, so the 3.88 MB payload spans two 2 MiB pages.  The header faulted
the first, the scan the second.

## MVCC / vacuum / WAL / UPDATE — the trade, stated plainly

- **Snapshot, not MVCC.** The load runs the SELECT at one snapshot and copies
  the visible tuples.  There are no `xmin`/`xmax`, no command IDs, no version
  chains, no visibility map, no hint bits.  A reader sees exactly the rows that
  were visible at load time, forever.  Later commits are invisible; a transaction
  that loads and later rolls back still leaves the client holding the bytes it
  copied (the memfd is not transactional).
- **No vacuum, no bloat.** The object is immutable, so there is nothing to
  vacuum and no dead tuples to reclaim.  The flip side is that a stale snapshot
  can never be refreshed in place: refresh means running a new load, which
  allocates a new memfd and a new fd.  Old mappings stay until the client drops
  them (and the backend's fd is closed at exit).
- **No WAL, no durability, no replication.** A memfd is anonymous memory
  backed by tmpfs.  It is not written to the relation file, not WAL-logged, not
  streamed to standbys, and it dies with the backend/container/host.  There is
  no crash recovery: after a crash there is nothing to recover because the
  object never existed in the cluster's storage.  This is a cache/snapshot
  format, not a system of record.
- **No UPDATE, DELETE or INSERT.** The store is read-only.  A changed source
  row is an entirely new load; there is no row identity to update in place.
  A workload with frequent writes should stay on the heap path.
- **No planner, no indexes.** The planner does not know the table exists; the
  SQL surface is an explicit function call.  There is no predicate/projection
  pushdown beyond the SELECT text the caller passes to `load`, no statistics,
  no index scan, no join.  A projection at load time is the only pushdown.
- **Cost model.** A load is one heap scan + one `heap_deform_tuple` pass +
  one write per value + page faults on first touch.  It is roughly the price of
  one columnar frame build (measured 12.1 ms vs 13.8 ms for the baseline
  aggregate), so the first query is a wash and every later projection of a
  numeric column is ~1.3 ms.  Selective projections win more: the load still
  pays for every selected column, but a reduce pays only for the columns it
  walks.
- **Memory.** The source heap and the memfd coexist during the load.  The
  store is ~0.5x the heap for this shape, but for wide tables with long text it
  can approach the value bytes.  The SPI tuptable also materializes the whole
  result in the backend, so a load is bounded by `capacity` (1 GiB default,
  16 GiB max) and by backend memory; a streaming builder would be the next
  step for large loads.
- **Security.** The SQL function is executable by PUBLIC by default.  It is a
  read-only SELECT, but it can allocate a large memfd and materialize a large
  SPI tuptable; a restricted deployment should `REVOKE EXECUTE` or wrap it.
  The returned fd is a capability to immutable bytes, not a SQL-execution
  endpoint; the client opens it `O_RDONLY` and never maps it writable.

## What remains

1. **A real table AM, if the storage is to be first-class.** A `TableAmRoutine`
   would need: a `relam`/catalog row, a relfilenode and smgr integration; a
   page format that is already columnar (or a colocated column-run file);
   `xmin`/`xmax` or an equivalent snapshot/version scheme; WAL records for every
   write and a redo path; `VACUUM`/`ANALYZE`; `pg_class` statistics; planner
   hooks (`amcostestimate`, path generation); tuple locks and `SELECT FOR
   UPDATE` semantics; and a policy for `ALTER TABLE`/rewrite.  That is a large
   multi-week project, not a prototype, which is why this branch stopped at the
   bulk-load slice.
2. **fd handoff without `/proc`.** Add the `AF_UNIX` + `SCM_RIGHTS` path, so
   the client and backend need not share a UID and the descriptor is delivered
   as a capability rather than found by pid.
3. **A stress test for the `/proc` capability and seals.** The path is
   exercised in the container (same UID) and by the suite, but not across a
   hostile `ptrace_scope`/UID boundary; the `SCM_RIGHTS` handoff above is the
   right replacement rather than a longer `/proc` workaround.
4. **Streaming load.** Build per-column regions from a `DestReceiver` or a
   cursor instead of materializing the whole SPI tuptable, with a sizing pass
   or a growable arena; then the 16 GiB cap and the backend memory bound go
   away.
5. **Page-aligned column runs and `MADV_COLLAPSE`.** Today one payload is one
   contiguous pack; a selective reader faults its neighbours' pages.  Aligning
   each column to a page (and, for large stores, to a huge page) makes a
   projection fault only what it reads, and lets `MADV_COLLAPSE` pay for itself
   at ~50 MiB and up, where the L2 TLB overflows.
6. **Dictionary / compression / null-density.** H10's dictionary is a natural
   column kind for low-cardinality text; null-heavy fixed columns could drop
   the zero slot and use a prefix popcount like Arrow's layout.  The current
   format keeps both costs.
7. **Client convenience.** `ColumnarTable` already exposes numpy-compatible
   views, but a `copy=False` Arrow/`pyarrow` bridge, a `sum` that returns exact
   numeric for large ints, and a lifecycle/`close()` for the mapping are
   straightforward follow-ups.  Integer sums wrap in 64 bits today, inherited
   from `Cursor.aggregate`.
8. **Concurrency and sharing.** One load belongs to one backend.  Sharing a
   loaded table across connections would need a broker (or the ring worker) to
   own the fd and hand it to each client; the FD-per-load model is deliberately
   simple.

## Verdict

This is a clean, measured partial prototype, not a fake storage engine.  The
mechanism is real: a bulk load into a sealed memfd in the exact FPCOLUMN layout,
a client mapping, and an in-place scan with no per-row copy.  The repeated-scan
win is large and repeatable (10.9x on the same reduce); the one-shot case is
within 0.7% of the baseline (a wash), which is what the copy cost predicts.  The `SELECT *` full scan is fast
but not an apples-to-apples win over the baseline's view construction.  What is
missing — MVCC, WAL, vacuum, updates, durability, planner integration, a real
table AM — is the price of making this a storage engine rather than a snapshot,
and it is not paid here.

# DB-ENCODE: a streaming columnar receiver and a wire offset table

Status: on `ws/db-encode`, measured; full record in
`bench/results/ws-db-encode-ab.log`.

## What was there

`fastpostgres_stream_next()` fetched a batch with `SPI_cursor_fetch()`, which
materialises every result tuple in an SPI tuplestore, then called
`fastpostgres_columnar_build()` over the `HeapTuple *`. The builder itself was
already one pass into the column regions (H8-A), but it still paid for the
tuplestore and its per-batch allocation, and it still compacted the frame on
every batch to close the gaps left by the text-area reservation.

## H8-B: encode from the DestReceiver

`fastpostgres_columnar_fetch()` runs the portal with its own `DestReceiver`
(`columnar_receive`) instead of fetching into a tuplestore. The receiver:

1. samples the first eight rows (heap-tuple copies, in a small per-batch
   context) to reserve the varlena areas exactly as the pull path did;
2. reserves the frame for the requested row count, reducing that count until
   the reservation fits the segment;
3. writes each later slot's values straight into its column's region — one
   `slot_getallattrs()` deform and one store per value, no row-major datum
   scratch and no `HeapTuple` materialisation beyond the eight-row sample.

A row the frame cannot take is already consumed by the executor, so it is kept
in a per-portal queue (`carry.c`, also used by the FPROWS path) and written first
by the next request. The queue is per portal, like the old single carried row,
so one worker's stream cannot leak a row into another's batch. The final
(partial) batch still compacts, because its null bitmap and varlena offset
stride shrink to the real row count.

The wire v2 frames produced by this path were verified byte-identical to
`main@7e3d2e6` on bench, mixed/null-heavy and binary shapes, full and partial
batches (12/12 payload SHA-256 checksums and sizes).

## Wire v3: the column offset table

The full batch still had to compact because a text column's byte area is
reserved from a sample and ends short. v3 makes that unnecessary:

```
FPCOLUMN v3 header: magic, version, flags, ncols, nrows, columns_off
                    at columns_off: ncols × u64 header offsets
per column:         name_len, name, kind, pgtype, nulls, data_len, data
```

`fastpostgres_columnar_write_begin()` reserves and zeroes the table;
`write_column()` records each header offset. The reader (`fastpostgres_reader_next`)
follows the table and no longer needs the previous column's `data_len` to find
the next header, so a reservation may leave a gap. On a full batch
`columnar_finalize()` only patches the exact `data_len` and returns the furthest
used byte; on a partial batch the existing compaction writes the table entries at
the new header positions. Column contents are unchanged (verified: names, kinds,
pgtypes, null bitmaps and data regions hash identically to the main baseline on
all shapes and batch splits).

The frame grows by 8 bytes plus 8 per column (the generator's 1000×6 payload:
43 728 → 43 784 bytes), and v2/v3 readers refuse each other by the version check.

## Measurement

Same command (`benchmarks.sh 100000 1000000`), same host, one image at a time;
the workstream base is `merge/transport` (303366d), not only the protocol's
`main@7e3d2e6` baseline, because our branch already contains the merge.

| `stream()` rows/s | main@7e3d2e6 | merge/transport | ws/db-encode |
|---|---|---|---|
| 100K | 8.56M | 9.01M | **13.75M** |
| 1M | 8.50M | 8.85M | **13.94M** |

That is **+52.7% / +57.6% over the branch base** and **+60.6% / +64.0% over the
protocol baseline**; DB-API improves **+50.5% / +65.6% over the base**.

Encode-only, from temporary instrumented images (base: timer around
`publish_columnar_shm`; change: timer around `columnar_fetch` minus a
`FPQ_SCAN_ONLY` run of the same executor):

| | merge/transport | ws/db-encode |
|---|---|---|
| encode 100K / 1M | 34.1 / 34.8 ns/row | 46.7 / 43.7 ns/row |
| whole worker NEXT 100K / 1M | 102.8 / 101.0 ns/row | 68.3 / 70.0 ns/row |

The one-pass row-major write is more expensive per encoded row than the old
column-major sweep — six column regions (six cache lines) per row instead of one
sequential sweep. That is H8's honest trade: the receiver removes the
tuplestore, and the net worker time drops **−33.6% / −30.7%**. `stream()` moves
further than that because the shorter worker turn is on its critical path while
the client's per-chunk decode is unchanged; the fix for the encode-only number
is a small tiled row-major buffer written column-major (H4), which is
deliberately not in this change: it is the scratch this item exists to remove.

## What remains

- **Cache locality.** A 1–4k-row tile would recover the encode-only gap; it
  reintroduces a bounded scratch and is a separate, measurable item.
- **v3 compatibility.** The reader/extension must come from the same header
  build. A cross-version client needs negotiation or a v2 emitter.
- **Payload gaps.** Full batches publish the reservation's header positions, so
  `payload_size` includes unwritten interior gaps. The shm path never copies
  them; a copying transport would.
- **Partial batches still compact**; a frame could instead carry a separate
  row capacity, at the cost of a more complex reader.

# Read-path follow-ups (`ws/arch-readpath`)

Base `merge/transport`. Three changes on the lazy `ResultSet`; the wire format
is unchanged. Full record, commands and raw output are in
`bench/results/ws-arch-readpath-ab.log`.

## 1. Block-wise decode

The lazy result copied each columnar batch out of the segment, then built one
tuple per `decode_row()` call. The body was already specialized per column, but
the call rebuilt the six `FastpostgresColumn` views and re-ran the kind switch
for every row, so iteration paid a per-row setup cost that materialisation did
not.

`_client.decode_rows(columns, start, count)` decodes a contiguous window with
one specialized pass per column in C; `fp_decode_columns_range` makes the null
map and data offsets relative to the window. `ResultSet` uses it for iteration
(1024 rows per call), unit-step slices and `to_tuples()` (4096 rows per call).
Indexing and `fetchone()` still call `decode_row`, so a consumer that asks for
one row still decodes only that row. An early-stopping iterator pays one block
(1024 rows), not the whole frame.

Measured on the benchmark shape (same command, baseline vs change):

| cell | 100K | 1M |
|---|---:|---:|
| per-row iteration, baseline | 3.53M | 3.76M |
| per-row iteration, change | **4.29M** | **4.83M** |
| `fetchall()` (lazy default), baseline | 3.55M | 4.00M |
| `fetchall()` (lazy default), change | **7.64M** | **8.63M** |

The remaining honest regression is `fetchall()` followed by `to_tuples()`:
3.60M -> 3.06M at 100K and 3.81M -> 3.30M at 1M. The baseline built tuples
straight from the row frame; the lazy path already paid the column copy and then
decodes it. That is the price of the result owning its bytes, not a decode
regression. `force_tuples=True` / `FASTPOSTGRES_MATERIALIZE=1` stay on the old
row-frame path and keep their old speed, which is the right switch for a caller
that knows it wants tuples.

## 2. Lease instead of copy (opt-in)

`FASTPOSTGRES_LEASE=1` (or `Connection(lease=True)`, `aio.connect(lease=True)`)
keeps the ring checked out for the result's lifetime and stores each batch as
views over the two data segments instead of copying each column. There are two
segments, so only the newest two batches stay borrowed; before a segment is
posted again, the frame that still borrows it is copied into owned buffers. A
result that fits one (large) batch is therefore entirely copy-free. The request
size is raised to `FASTPOSTGRES_LEASE_BATCH` (default: one normal 100k batch) so
a caller can choose how much the worker should fetch at once.

The lease is released when the result is materialised, explicitly closed, or
collected. `to_tuples()` releases it as soon as the values are Python objects,
so a consumer that knows it wants rows does not pin a partition afterwards.

The cost is pool occupancy: a live leased result owns one ring, so a process
whose pool has N partitions can hold at most N-1 other reads without falling
back while it exists. This is why it is opt-in. It wins for a single consumer,
short-lived results, large text/bytea columns (the copy is a larger fraction),
and a pool with spare partitions; it loses for many concurrently live results,
for results that span many batches (the eviction copies remove the advantage
and cost the pipelining overlap), or when a caller materialises immediately and
would have been better served by the forced row-frame path. With
`FASTPOSTGRES_LEASE_BATCH=1000000`, the 1M read moved `fetchall()` 8.63M ->
9.83M and iteration 4.83M -> 5.19M; at 100K, 7.64M -> 8.60M and 4.29M ->
4.81M.

## 3. libpq fallback parity

A read that cannot get a ring (extension absent, pool exhausted, or a failed
columnar read re-run) used to return the old list of tuples while the
shared-memory path returned `ResultSet`. `Connection.exec_lazy()` now returns a
native `LazyResult` that holds the `PGresult`; `ResultSet` accepts it as a row
source and decodes a row (or a block of rows) on demand. `force_tuples` and
non-read statements keep the eager `exec()` result and its list.

The shape is the same, the lifetime is not: the `PGresult` must stay alive
until the result is materialised or collected, and libpq has already put the
whole text result in memory. There is no columnar frame to copy and nothing to
decode block-wise per column, so the fallback decodes row by row (one C call per
row) rather than pretending to have the shared-memory representation. That is
the exact reason it is parity of shape and laziness, not of storage or speed.

## What remains here

- **`to_tuples()` after a lazy read** still pays the copy; a caller that wants
  tuples should use `force_tuples=True` or lease mode, and the two paths are
  measured separately now rather than conflated.
- **Lease eviction is serial.** Above two batches, the frame copy happens before
  the next post, so the worker is idle for that copy; a third segment or a
  decode-in-place consumer would overlap it.
- **Lease + a small pool** can fall back or wait while a result is alive; it is a
  per-connection policy, not a replacement for the default copy.
- **The libpq lazy result holds a whole `PGresult`.** A streaming cursor over
  libpq would need `PQsendQuery`/`PQgetResult` plumbing, which is a different
  path; this change only removes the eager Python tuple build.

# Safe object reuse for decoded values (workstream `ws/object-reuse`)

Status: implemented and measured, opt-out via `FASTPOSTGRES_OBJ_ARENA=0`.
Client-only; no wire change.

## Why not the immortal arena

The removed H11 arena was worth ~2x on the object-birth step because it wrote
the CPython header around a value on the spot and stamped the refcount immortal.
It could only be append-only: an object whose storage belongs to an arena can
never be deallocated, so every read added resident pages (~791 MiB after one
10M-row read, ~3.9 GiB after several). The speed was real; the lifetime was not.

The safe version keeps the birth and gives the object a lifetime. Values from
the row frame (and the columnar fallback) are born into cells of a private
subtype of `float`, `int` or `str`, whose `tp_free` returns the cell to a
size-class free list. The allocation is reused across reads, and the whole pool
is a fixed reservation whose worst case is capped.

## Mechanism

`python/src/fastpostgres/native/codec.cpp`:

- **Private subtypes.** `_fastpostgres.born_float`, `.born_int`, `.born_str`
  are static types based on `PyFloat_Type`, `PyLong_Type` and `PyUnicode_Type`.
  Their `tp_free` is `cell_free`, so `float_dealloc` / `long_dealloc` /
  `unicode_dealloc` hand the cell back instead of calling `PyObject_Free`.
  `isinstance(x, float/int/str)` is true and all comparison/hash/repr/number
  slots are inherited. The types are not importable and have
  `Py_TPFLAGS_DISALLOW_INSTANTIATION`; `__reduce__`/`__reduce_ex__` reduce a
  born value to the exact built-in type, so copy, deepcopy and pickle keep
  working and never see the subtype.
- **One reservation, bump + free lists.** `fp_objpool_init()` reserves
  `FASTPOSTGRES_OBJ_ARENA` bytes (default 512 MiB) with
  `MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE`. Each thread takes 2 MiB chunks from
  it with an atomic bump pointer; cells are rounded to 16 B and drawn from
  thread-local size-class free lists (up to 64 KiB, larger values use the real
  constructor). `tp_free` first tests whether the pointer is inside the
  reservation; a normally allocated subtype instance (for example via
  `base.__new__`) is outside it and gets `PyObject_Free`. Inside, the cell size
  is recovered from the object itself (`PyFloatObject`, the int digit count in
  `lv_tag`, the compact-ASCII length), so there is no per-object header.
- **Layout verification at init.** Nothing about CPython's layout is assumed
  blindly: the compact-ASCII header is copied from a string the interpreter
  built, the int tags come from real longs, and `pool_verify()` proves repr,
  hash, compare, round-trip and cell reuse for each type. Any mismatch leaves
  the pool off and the normal constructors run. The free-threaded build
  (`Py_GIL_DISABLED`) keeps the pool off.
- **Bounded fallback.** When the reservation is full, or a value is not compact
  ASCII / is larger than 64 KiB, the decoder calls the ordinary constructor.
  Correctness never depends on the arena having room.

## Measurement

Full record: `bench/results/ws-object-reuse-ab.log`. Same image, same command,
pool off vs on, 1M rows (`FASTPOSTGRES_MATERIALIZE=1`, eager row values):

| metric | pool off | pool on | delta |
|---|---|---|---|
| DB-API eager rows/s | 3.77M | 4.89M | +29.7% |
| core interface rows/s | 3.84M | 4.86M | +26.6% |
| `read_rows` cycles/row | 676.4 | 418.7 | −38.1% |
| `stream()` rows/s (control) | 9.18M | 9.26M | +0.9% (noise) |
| peak RSS at 1M | 379.0 MiB | 378.0 MiB | flat |

The RSS check is ten consecutive 1M-row reads in one process: 49.3M values
born, arena `used` stays at 202752 KiB and peak RSS at 377.6 MiB, unchanged
from the first read. That is the property the immortal arena did not have.
Against `fpq-baseline` (main) the same run is +22.9% DB-API rows/s and −28.7%
`read_rows` cycles/row at the same ~378 MiB.

`tests.sh` is green: extension `make installcheck`, then 61 client tests
(`OK`, one skipped). The added test covers compare/copy/deepcopy/pickle and a
second read reusing the first read's cells.

## What remains

- **Cardinality-aware dictionary.** Values are reused as *storage*, not by
  value: a repeated low-cardinality column still gets one object per row. A
  per-chunk open-addressed dictionary (H10) keyed by the wire bytes could share
  the object itself for `bool`/status/repeated text. That needs either a scan to
  decide it is worth it or a bounded insert-then-disable policy; it is a
  separate win and would compose with this pool.
- **Text encoding.** Only compact ASCII and the three value types are born in
  place; non-ASCII text, `bytea`, `date`/`time`/`timestamp` and `uuid` still use
  their constructors. The compact-ASCII case is the benchmark's shape; a
  Latin-1/UTF-8 cell layout is the next sizeable slice but adds more
  version-specific header knowledge.
- **Free-list classes.** The current free lists are per exact 16 B size class.
  Fragmentation across a long-lived process with many different text lengths is
  bounded by the cap, but a size-class ladder would use the reservation better.
- **Per-type opt-out.** There is one process switch. A deployment that cares
  about `type(x)` being the exact built-in (some serializers do) can set
  `FASTPOSTGRES_OBJ_ARENA=0`; a per-kind switch would let it keep the float/int
  win without text.
