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

1. `fastpostgres_columnar_load(query, capacity, handoff)` runs a read-only
   SELECT once and writes every value into its column's run in an anonymous
   **memfd**, using the existing FPCOLUMN layout.
2. The memfd is sealed immutable and described to the client as
   `FPQCOL1 <backend pid> <fd> <bytes> <rows>`.  The descriptor itself is
   delivered over an `AF_UNIX` control socket with `SCM_RIGHTS`, and
   `SO_PEERCRED` is checked against `PQbackendPID`.
3. The client maps the received descriptor once and scans the columns in place:
   zero-copy column views, a C++ reduce, and a full-byte scan.  No server round
   trip, no transpose, no per-row copy, no scratch, no `/proc` lookup.

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
connect(client control socket)          optional third argument
sendmsg(record + SCM_RIGHTS(fd))        the capability transfer
```

The descriptor stays open in a 64-entry process-local table for the backend's
lifetime; PostgreSQL closes it at backend exit, and the client's mapping holds
the inode, so an already-attached client keeps reading after the backend exits.

The capability is the **received descriptor**, not a lookup by pid.  Each
`columnar.load()` publishes a fresh abstract `AF_UNIX` address
(`@fpqcol-<pid>-<serial>`), passes it as the optional third argument, and the
backend connects to that one-use address and sends a `SOCK_SEQPACKET` record
`(magic, backend pid, bytes, rows)` with the sealed memfd attached.  The client:

- accepts exactly one connection, reads the descriptor with `recvmsg`, and
  closes the listener;
- reads `SO_PEERCRED` from the accepted socket and requires the peer pid to be
  `PQbackendPID` for this libpq session and to match the record's pid — so a
  local process that guesses the address cannot substitute its own descriptor;
- maps the received fd read-only, closes it (the mapping holds the object), and
  names the mapping with a process-unique handle.

Because the descriptor is transferred, the client and backend need **not** share
a UID.  The original `open("/proc/<pid>/fd/<fd>")` path is still there, unchanged,
as the fallback for a server that lacks the three-argument function or a failed
transfer (same-UID only); the two-argument SQL function still returns the same
`FPQCOL1` string.  The A/B in `bench/results/ws-os-scm-ab.log` exercises the SCM
path: every load reports `handoff: scm`, with the peer uid/gid recorded.

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

### SCM_RIGHTS handoff, re-measured (`ws/os-scm`, 100K, 3 alternating runs)

`bench/results/ws-os-scm-ab.log`, medians.  Same script, same `fpq-baseline`
control, with the new handoff in the change image:

| path | result |
|---|---|
| baseline heap + frame, `Cursor.aggregate` | 7.26M rows/s, 13.78 ms |
| baseline heap + frame, `stream()` views | 8.21M rows/s, 12.19 ms |
| memfd `ColumnarTable.reduce` | **79.25M rows/s, 1.26 ms — 10.92x** |
| memfd full-byte scan (`scan_all`) | 127.35M rows/s, 0.79 ms, 4.94 GB/s |
| one load (server pass + SCM handoff + map) | 11.48 ms |
| handoff itself (accept + peercred + recvmsg) | 0.056 ms |
| load + 1 reduce | 12.74 ms vs 13.78 ms — 7.6% faster (a wash in noise) |
| load + 2 reduces | 14.00 ms vs 27.57 ms — 1.97x |
| load + 10 reduces | 24.10 ms vs 137.83 ms — 5.72x |

The handoff adds ~0.5% to the load and no per-byte work: the 3.88 MB payload
never moves, only the descriptor does.  The client performs no `/proc` open on
this path; the three-argument load reports `handoff: scm` and
`peer=(<backend pid>, <uid>, <gid>)` from `SO_PEERCRED`.

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
  endpoint; the client maps it `PROT_READ` and never writable.  The SCM_RIGHTS
  control socket is one-use and abstract-namespaced, and `SO_PEERCRED` pins the
  sender to this session's `PQbackendPID`; the fd itself is the capability, so
  sharing a UID is no longer part of the trust boundary.

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
2. **fd handoff without `/proc` — done in `ws/os-scm`.** The `AF_UNIX` +
   `SCM_RIGHTS` transfer is the primary path, with `SO_PEERCRED` checked
   against `PQbackendPID`; the descriptor string and `/proc` attach remain as
   the same-UID fallback.  **Still open:** a cross-UID integration test (the
   container's client and server both run as `postgres`), and a lifecycle test
   that the one-use listener is reclaimed on every error path.
3. **A stress test for the seals and the control socket.** The load path and
   its two focused client tests are exercised in the suite; a hostile
   `ptrace_scope`/UID boundary and a `sendmsg`-fails-mid-transfer injection are
   still worth automating (the code falls back to `/proc`, but that failure is
   not covered by a test).
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
a descriptor passed tenant-to-client over `SCM_RIGHTS` (checked with
`SO_PEERCRED`, no shared UID needed), a client mapping, and an in-place scan
with no per-row copy.  The repeated-scan
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
in a per-portal queue (`carry.cpp`, also used by the FPROWS path) and written first
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

# Server-side aggregate reduce: exactness, more kinds

Status: committed with tests and an A/B
(`bench/results/ws-reduce-ab.log`). Builds on the reduce pushdown from the
transport merge; the client-side walk in
`python/src/fastpostgres/native/aggregate.cpp` remains the fallback and the
reference.

## What changed

### 1. FPREDUCE v2 — a frame that can carry the answer

The old frame had a 12-byte slot with a 64-bit value and an `is_float` bit.
That is enough for a double or a wrapped int64, and not enough for what the
server actually returns: `sum(bigint)` is numeric, and `min(text)` is text.
v2 keeps the same 32-byte header and grows a slot to 32 bytes:

    is_null u8, rtype u8, pad[2], aux_off u32, aux_len u32, pad[4],
    lo u64, hi u64

`hi:lo` is a two's-complement int128 for an integer SUM, a signed int64 for
COUNT/MIN/MAX, or a double's bits. `rtype` says which, and also names the
temporal/text/numeric cases. Text, bytea and numeric values are written to an
aux byte pool after the slots and addressed by `aux_off`/`aux_len`. Both sides
share the definition and the reader in
`extension/include/fastpostgres/fastpostgres_shm.h`; the client decodes the
slot through `fp_decode_reduce_value()`, so `read_reduce()` and the fallback
return the same Python types.

The header is still the same for every request, and a reduce result is still a
handful of bytes on the wire: 96 B for the measured `sum + count_all` plan.

### 2. Exact integer SUM

The worker accumulates into a signed `__int128` and publishes all 128 bits.
The client turns that into a Python int and adds per-batch values with Python's
arbitrary precision. Neither side wraps, so the result equals the server's
`sum(bigint)` (which is numeric) and not merely its low 64 bits. A batch is at
most `batch` rows; at the maximum int64 value, 128 bits covers every batch a
ring can fetch by a wide margin. The fallback walks the column into the same
`__int128` and returns a Python int, so both paths agree.

`sum(int2/int4)` on the server returns int8; that is a subset of the same
result and compares equal as a Python int.

### 3. AVG

AVG is not associative across batches, so the DB-API expands it into
`AGG_SUM + AGG_COUNT` in the internal plan and divides once at the end. Every
batch still ships only scalars. The division matches PostgreSQL's numeric
semantics rather than "16 decimal places":

- `select_div_scale()` keeps sixteen significant base-10000 digits, so a small
  average gets 20 decimals and a huge one gets none;
- it rounds half away from zero;
- the result's scale is at least the sum's own numeric scale.

`_pg_avg()` in `dbapi.py` mirrors that on the exact int/Decimal numerator, so
`avg(int)`, `avg(bigint)` and `avg(numeric)` equal the server's numeric.
Float columns return a float, as `avg(float8)` does.

### 4. MIN/MAX over more kinds, and COUNT over all of them

- **text/varchar/bpchar**: the worker keeps the best bytes and compares with
  `varstr_cmp` and the attribute's collation — the server's ordering, not a
  UTF-8 memcmp. The value travels in the aux pool and arrives as `str`.
- **date/time/timestamp/timestamptz**: compared as their wire integers and
  published as such; the client builds `datetime.date`/`time`/`datetime`.
- **numeric**: `numeric_cmp` for MIN/MAX and `numeric_add` for SUM, with the
  value published through `numeric_out` and decoded to `Decimal`. This is exact
  where the columnar wire is not (it widens numeric to float8).
- **COUNT** reads no value, so it now works for every kind, including text,
  instead of rejecting anything outside the numeric kinds.
- SUM over text/date/time is refused with a `reduce: ` error; there is no
  guess to make.

The fallback mirrors the numeric kinds (text/bytea through the columnar
offset table, temporal integers through the same decoder). Two deliberate
approximations remain in the fallback, documented below.

## Measurement

`bench/internals` job `bench/jobs/aggregate-pushdown.py`: 2,000,000 rows,
`sum(price)` + `count_all`, verified against the server. Full record in
`bench/results/ws-reduce-ab.log`.

| shape | baseline rows/s | baseline bytes | change rows/s | change bytes |
|---|---|---|---|---|
| batch 100k | 9.69M | 16,251,040 | 10.90M | 2,016 |
| one request | 11.92M | 16,250,056 | 12.90M | 192 |

The byte count is the worker's published frame, not an estimate: it drops from
the whole int8 column to 96 B per request. Throughput moves +8–12% in this run,
which is a couple of host-noise points beyond the traffic saved — the point of
the change is the semantics, not a new speed record.

## What remains

- **GROUP BY / DISTINCT are not in this slice.** A grouped aggregate needs a
  multi-row result (one scalar tuple per group) and a group key in the request;
  the scalar FPREDUCE frame and `Cursor.aggregate()`'s dict are both shaped
  around one answer. The honest next step is a separate grouped frame plus a
  `Cursor.aggregate_group()`, not a widening of this one. Shipping a half
  version that materialised groups in Python would lose the property that makes
  this path worth having (no per-row objects on the client).
- **One worker per portal.** The ring already splits work by partition: N
  rings reduce N partitions in parallel, and the client's per-batch combine is
  exact, so a caller can combine partitions itself without losing precision.
  Splitting a *single* scan across workers needs a scan partitioner that the
  ring does not have; it is not a cheap addition and is not attempted here.
- **Fallback numeric is float8.** The columnar wire carries numeric as float8,
  so with `FASTPOSTGRES_AGG_PUSHDOWN=0` a numeric SUM/AVG/MIN/MAX is the old
  lossy float path. The default (worker) path is exact. Fixing the fallback
  would mean a new numeric wire kind, which is a codec change, not a reduce
  change.
- **Fallback text MIN/MAX is bytewise.** The client has no collation in the
  columnar descriptor, so its comparison matches a C collation, not necessarily
  the server's. The worker path is exact and is the default. Adding the
  collation to the column descriptor would fix the fallback.
- **Output-function text types.** `name` and other types that map to the TEXT
  wire kind but are not varlena text are refused by the worker's MIN/MAX (the
  fallback's columnar frame has already normalised them to text bytes). A
  `NameStr`/output-function comparator in `reduce.cpp` would close that gap.
- **Numeric accumulation memory.** `numeric_add` allocates a new value per row
  in the batch's memory context; it is bounded by batch size and freed at
  `SPI_finish`, but a per-batch reset context would keep a very large batch's
  peak lower.
- **A huge text value must fit the segment.** MIN/MAX over text copies the
  winning bytes into the scalar frame; a value larger than the ring's segment
  fails with `reduce: result does not fit the segment` rather than falling
  back. That is the same capacity contract as the columnar frame.

# Sealed-memfd data segments: the default

Status: default. `FASTPOSTGRES_MEMFD=0` (or `off`/`false`/`no`) restores the
named `/dev/shm/<seg>` data segment. The measurement and the test matrix are in
`bench/results/ws-memfd-default-ab.log`.

## Why

The data segment used to be a named `/dev/shm/<seg>` file. That made every
reader's trust boundary a mode bit: the worker had to widen the file to 0666 so
a client running as another user could map it, and any local process that could
open the name could also read (or, depending on the ring, drive) the segment.
The ring's control segment stays 0600 and is still a named file; the payload no
longer has to be.

## Mechanism

For each data segment the worker:

1. `memfd_create("fastpostgres-seg", MFD_CLOEXEC | MFD_ALLOW_SEALING)`,
2. one `ftruncate` to the capacity the client asked for,
3. `mmap` read-write and `MADV_HUGEPAGE` before the first write,
4. `F_ADD_SEALS(F_SEAL_SHRINK | F_SEAL_GROW)`,
5. `sendmsg(SCM_RIGHTS)` on the ring's existing AF_UNIX `SOCK_SEQPACKET`
   handoff connection.

The client maps the descriptor read-only, keeps the mapping (and the memfd
descriptor) for the process lifetime, and revalidates the header per batch. A
request that grows past the sealed capacity makes the worker replace the object
and send a new descriptor with a new handoff generation; the client's cache sees
the generation and remaps. There is no path that widens an existing object, so
a reader's size and identity cannot change under it.

`F_SEAL_WRITE` is deliberately not set: the ring recycles one mapping for every
batch, and a write seal would fault the next publish. Shrink/grow are the seals
that matter to a reader.

## Authentication

The handoff socket is a second trust boundary, because the descriptor (not a
path) is what the client maps. Both ends check `SO_PEERCRED`:

- the worker connects to the client's abstract address and only sends
  descriptors when the peer pid is the ring's `client_pid`; a mismatch logs once
  and keeps the `/dev/shm` path;
- the client accepts the queued connection and only registers it when the peer
  pid is the worker pid the worker published (`srv_pid`) before `worker_ready`.

`GET_SEALS`/`ftruncate` are exposed to the regression test through
`_client.segment_fd(seg)`; the test proves both seal bits and that shrinking and
growing are refused with `EPERM`.

## Fallback, never silent

The named segment remains for the cases that need it:

- `FASTPOSTGRES_MEMFD=0` (cross-user clients, debugging, a kernel without
  sealing);
- the eventfd engine, whose pool worker polls many rings and has no per-worker
  handoff socket/segment cache. Instead of opening a listener the pool will
  never connect to (and then reading `/dev/shm` after a 2 s accept), the client
  now chooses `/dev/shm` before the bootstrap and emits one `RuntimeWarning`.
  The whole suite is green in that mode;
- a failed `memfd_create`/`ftruncate`/`mmap`/`F_ADD_SEALS`/handoff. The worker
  logs the reason once and creates the named segment; it does not hand over an
  unsealed object.

## What the measurement says

Same command, baseline vs this branch, three alternating runs per size, plus a
`fpq-transport` (merge/transport) control on the same host. Medians:

| cell | baseline | change | delta |
|---|---|---|---|
| 100K stream() zero-copy | 8.14M | 9.02M | +10.8% |
| 100K DBAPI | 3.62M | 7.64M | +111.0% |
| 100K core | 3.56M | 7.49M | +110.4% |
| 100K 4 processes | 8.52M | 23.44M | +175.1% |
| 3K stream() zero-copy | 6.21M | 11.02M | +77.5% |
| 3K DBAPI | 7.35M | 10.62M | +44.5% |
| 3K core | 4.80M | 5.21M | +8.5% |
| 3K 4 processes | 3.82M | 4.35M | +13.9% |

The baseline is `main`, so those gains are mostly the merge/transport work this
branch builds on. Against `fpq-transport` (the actual base, same host) the
medians overlap: 100K stream 9.02M vs 9.03M, DBAPI 7.64M vs 7.85M, core 7.49M
vs 7.66M, 4 processes 23.44M vs 23.07M; the 3K stream/DBApi cells are inside
that host's spread. **The default memfd costs nothing measurable.** The 3K read
stays below baseline latency, where the data-segment handoff is a larger share
of the work.

## What remains

- **eventfd + memfd.** The eventfd pool could carry memfds too, but `shm.cpp`'s
  segment cache and handoff socket are per-worker globals today. Supporting it
  needs a per-ring handoff registry (and a per-ring segment cache key) in the
  pool. Until then the fallback is explicit, not silent.
- **Cross-user clients.** The memfd default assumes the client and worker can
  pass descriptors and that the worker's peer is the client pid; the named
  0666 segment remains the cross-user answer. A same-UID check is not enforced
  on the client side because the kernel already scopes abstract-socket delivery
  and the pid check is the stronger statement.
- **Descriptor lifetime.** The client keeps one memfd per mapped segment for
  the process lifetime so `segment_fd()` can inspect the seal. That is a few
  descriptors per ring; tying it to the ring's teardown would let a long-lived
  process that churns ring names reclaim them.
- **Seal diagnostics.** `F_ADD_SEALS` failure is logged once per worker and
  falls back. A future version could expose the chosen backing in the ring
  header so a tool can see which path a running ring took without looking at
  `/dev/shm`.

# The per-fetch fixed cost at a small batch

Status: implemented in `extension/transport/engine_shm.cpp`, default on at a
300 us budget, with its measurement. `FASTPOSTGRES_SPIN_US=0` restores the pure
semaphore sleep.

## What the fixed cost was

The map-cache workstream removed the worker's per-batch `shm_open`/`mmap`
cost. At a 3000-row fetch the worker still paid a visible fixed cost, and
bench/NOTES.md ("Where the time is now") priced it at ~25 us. Instrumenting
`ring.next_rows()` on the base tree splits the worker's 261.3 us per fetch:

| phase | us/fetch |
|---|---|
| `PortalRunFetch` (the executor) | **259.1** |
| column headers | 1.1 |
| SPI connect/find/finish | 0.5 |
| cursor name + segment path | 0.4 |
| per-batch memory context / arrays | 0.2 |
| segment map (cache hit) + publish + cleanup | 0.0 |

So the executor is 99% of the worker's turn, and reusing the cursor, the
`RowReceiver` or its buffers would pay for itself only inside the remaining
2.2 us. The only reducible piece outside the executor is the handoff: the
driver's worker wall is ~294 us at 3k against those 261 us inside the worker,
and the difference is the doorbell round trip.

## Mechanism

Both sides of the `shm` engine now poll the sequence counter for a bounded
budget before falling back to `sem_timedwait`:

- the client spins while `resp_seq < seq` after posting a request;
- the worker spins while `req_seq == resp_seq` after posting a response.

The budget defaults to 300 us and is capped by the hook's own timeout. It is
used only for batches at or below `FASTPOSTGRES_SPIN_MAX_BATCH` (16384 by
default): at 100k rows the partner's turn is milliseconds, the spin would pay
its whole budget and then sleep anyway, and the pipelined DBAPI read already
hides the worker behind the client's decode. Setting the env var to 0 disables
the spin, and `FASTPOSTGRES_SPIN_MAX_BATCH=0` removes the batch gate.

The counter stays the truth. A spun response leaves its `sem_post` in the
semaphore, so a wake from `sem_timedwait` is re-checked against `resp_seq`
rather than trusted; a stale post costs one loop and no incorrect return. The
worker already worked this way, and the client now does too. `spin_us` in the
ring's tally records the time spent before a fallback, so the record says what
the policy cost.

## Measurement

`bench/dispatch/worker-map-batch.py 3000 100`, five interleaved runs per side,
medians; the 100k control is `benchmarks.sh 100000`. Full record:
`bench/results/ws-sd-3k-ab.log`.

| side | worker | total | rows/s | worker CPU |
|---|---|---|---|---|
| baseline (main@7e3d2e6) | 104.85 ns/row | 191.08 ns/row | 5.23M | 95.16 ns/row |
| base (merge/transport) | 98.46 | 187.18 | 5.34M | 88.44 |
| change | **95.68** | **183.41** | **5.45M** | 178.20 |

Incremental (base -> change): worker -2.8%, total -2.0%, throughput +2.1% at
3k. Against the protocol baseline it is -8.7% / -4.0% / +4.2%, but most of that
is the merge this branch builds on. The 100K control does not move: DBAPI
7.70M -> 7.71M, and the other cells stay inside the harness' +/-5-10% spread.

The price is explicit: the spin doubles the worker's on-CPU time at 3k (88 ->
178 ns/row) because it waits on the core instead of sleeping. That is the
trade a bounded spin always makes, and it is why it is capped, batch-gated and
overridable. The wall time it buys is ~8 us of worker turn and ~11 us of total
per 3000-row fetch.

## What remains

- **The executor.** 259 of the 261 us inside the worker is `PortalRunFetch`;
  changing that means changing how the portal is driven, not the transport. A
  custom `DestReceiver` for the columnar frame (the rows frame already has one)
  would skip SPI's tuptable, but the executor's per-row work stays.
- **The rest of the doorbell.** The spin removes the sleep/wake, not the two
  `sem_post` calls or the Python/C call frame around the handoff. Replacing the
  post with a shared-memory hint the other side can poll is the next step, and
  the eventfd engine is the existing answer for a worker that polls many rings.
- **A longer spin is not the answer.** 300, 500 and 800 us all landed within
  noise of each other at 3k; beyond the partner's turn the extra budget only
  burns CPU. An adaptive budget (the last observed turn) could spend less on a
  mixed workload, but it adds state for a gain the fixed cap already captures.
- **Large-batch reads.** The gate deliberately leaves them on the old path.
  The DBAPI at 100k is pipelined, so the worker's turn is already hidden; the
  spin would only cost CPU there.

# Decoupling the ring count from `max_worker_processes`

Status: prototype, committed with its measurement. Opt-in
(`FASTPOSTGRES_TRANSPORT=eventfd`); the semaphore engine and its 1:1 worker
shape are unchanged and remain the default.

## The ceiling

One ring used to mean one dynamically registered background worker — a full
PostgreSQL backend — so `max_worker_processes` (8 by default, one held back)
capped concurrent shared-memory reads at seven rings system-wide. The DB-API
noticed the budget running out and silently moved subsequent reads to libpq.
That is the right failure mode, but it makes the ring a fixed-size resource:
more concurrent readers means a bigger server setting, and one idle ring costs
an entire backend.

The reason is the doorbell. A `sem_t` wakes one waiter, and `sem_timedwait`
cannot be put in a `poll()` set, so a process that wants to serve several rings
has to sleep on one at a time (or poll all of them and pay a syscall per ring
per turn). The ring protocol itself does not care: the request/response slot,
the sequence counters, the data segment and its publish barriers are
transport-neutral, and `transport.h` already isolates the wakeup.

## Mechanism

Two additions, both behind that seam:

### 1. `engine_eventfd.cpp` — a pollable doorbell

Each ring gets two eventfds: client→worker and worker→client. The client
creates them (it also creates the ring segment) and hands the worker its copies
over `SCM_RIGHTS` on an abstract Unix socket named after the ring
(`\0fastpostgres_fd_<seg>`). The client's listener exists before the bootstrap
SQL call; the worker connects after it maps the segment, receives the pair, and
only then sets `worker_ready`. `wait_ready` accepts and sends while it waits,
so the handshake is part of readiness rather than a second protocol.

The data path does not change: a post is an eventfd write, a wait is a poll
followed by a read that drains the counter, and the truth is still
`req_seq != resp_seq`. A stale wake is an event that advanced nothing; the
counter check suppresses it exactly as the semaphore engine does. `kill(client_pid,
0)` on the worker's 100 ms idle tick is still the liveness check. The client's
fds live beside its mapping in a process-local table keyed by the ring address,
**not** in the shared ring header: a forked child dropping an inherited Ring
must not close a number that now belongs to the parent, and a Ring object
deallocated after a newer one took the same name must not close the newer
object's fds. (The first prototype did publish them in the header; the fork
test caught it.)

Eventfd counters coalesce, so a missed read of a stale wake costs nothing: the
next poll sees the count. Writes ignore `EAGAIN` (the counter is already
non-zero, i.e. a wake is already pending).

### 2. `worker.cpp` — a small pool of workers dispatching many rings

The registry is a per-database `/dev/shm/fp_pool_<dboid>` file (a worker is
connected to one database) holding eight slots of up to 32 ring names. The SQL
bootstrap (`fastpostgres_ring_start`) routes an eventfd ring to the pool
instead of registering a worker of its own:

1. It first tries to give the ring a worker of its own (capacity starts at 1).
   If `RegisterDynamicBackgroundWorker` succeeds, that is the original 1:1
   shape, with the same throughput and isolation.
2. If the server is at `max_worker_processes`, the ring is appended to an
   existing pool worker instead, and that worker's capacity grows by one (up to
   `FASTPOSTGRES_RINGS_PER_WORKER`, default 8). That is the decoupling: an
   eighth ring no longer needs an eighth backend.

A pool worker maps every ring in its slot, attaches its two eventfds, sets
`worker_ready`, and then one `poll()` covers all of them. On a wake it services
one request: `req_seq`/`resp_seq` decide whether anything was asked, the ring's
own per-ring state decides which cursor to fetch, and the answer goes back on
that ring's eventfd. One executor turn at a time is the honest model for one
backend.

Per-ring state is what makes multiple portals safe: the existing worker kept a
single `have_stream` global and one cursor, which assumed one stream per
worker. The pool tracks a stream name and a `have_stream` flag per ring. All
streams share one transaction and one snapshot (a stream already held a
transaction for its whole life), and the last CLOSE commits. A PG error aborts
the transaction and drops every stream on that worker — a real coupling,
documented below. The row frame's carried row (a tuple that did not fit in the
segment) was a single global; it is now keyed by portal, so stream A's carry
can never be written into stream B's batch.

Teardown: STOP, a dead client, or SIGTERM closes the ring's cursor, commits if
it was the last stream, sets `worker_ready=0`, detaches the eventfds, removes
the ring from the registry (so its capacity is reusable) and unmaps it. A
worker with no rings exits after a two-second grace, which keeps a process'
rings on one worker across a create/close cycle without pinning a backend
forever.

## What the measurement says

Driver: `bench/internals/threads/ring_scaling.py`, N processes × one ring ×
three object-free reads (`Cursor.aggregate`, columnar batches reduced in C++,
no Python object per row), 100k rows per read, batch 25k. Six interleaved
pairs of container runs, 3 reps per setting per run; the table is the median
of the six. Full record, raw values and controls in
`bench/results/team-networking-ab.log`.

| N | baseline rows/s | baseline served | change rows/s | change served | delta |
|---|---|---|---|---|---|
| 1 | 6.54M | 1/1 | 6.33M | 1/1 | −3.3% |
| 2 | 12.76M | 2/2 | 11.71M | 2/2 | −8.2% |
| 4 | 23.44M | 4/4 | 21.93M | 4/4 | −6.4% |
| 8 | 31.11M | **7/8** | 22.64M | **8/8** | −27.2% |

The host carries ~2 cores of background load, so n≤4 cells move ±5–10% run to
run; a same-image alternation at n=2 (semaphore vs eventfd, six times) was
12.51M against 12.58M, i.e. the per-ring path is at parity. The n=8 cells do
not overlap between the six baseline and six change runs.

Three controls make the result readable:

- **The engine is free.** With `max_worker_processes=16` (an eighth slot, so no
  multiplexing) the change reaches 32.58M on 8 workers against the baseline's
  32.93M at N=8 (−1.1%), and N=1–4 show no systematic gap beyond the host's
  noise. Eventfds cost nothing measurable per read.
- **The loss is the shared worker, not the pool.** With
  `FASTPOSTGRES_RINGS_PER_WORKER=1` (overflow disabled) the change reproduces
  the baseline exactly — 7/8 served, 31.13M against 31.11M — while still using
  the new engine.
- **The baseline cannot run N=8 at all.** Its eighth `aggregate()` raises
  `NotSupportedError`; a DB-API `execute()` would silently take libpq. There is
  no baseline number for what the eighth ring does, because it does nothing on
  the ring.

So: **the decoupling removes the ceiling, but it does not make concurrent
reads faster, and past the budget it makes the aggregate slower.** That is
structural, not an implementation defect. A backend is one OS thread; two
rings on one worker serialize their executor turns, and when every read is long
and every ring is busy, the ring sharing a worker is the straggler that sets
the wall time. One ring per worker up to the budget is the right policy, and
that is what the pool does — it only multiplexes when the alternative is
libpq.

The win is capacity and shape, not throughput:

- an application can hold more than seven live rings, so the eighth and later
  readers stay on the shared-memory path instead of falling back;
- many *mostly idle* rings (typical of an app with many open cursors /
  connections) cost almost nothing: a pooled worker sleeps in one `poll()` for
  all of them, where the 1:1 model spent a backend per ring;
- the ring budget is now a policy (`FASTPOSTGRES_RINGS_PER_WORKER`) rather than
  a hard function of a server GUC.

And the price is explicit: at N=8 on a seven-slot server, one worker carries two
rings and the aggregate drops from the unattainable 31.1M to 22.6M. If the
eighth read matters more than the ~27%, the pool is a win; if all eight are
long, parallel scans, raising `max_worker_processes` (or running a second
server) is still the better answer.

## ws/net-n8: scheduling, isolation, and the limit that remains

This follow-up keeps the default `shm` path and the eventfd opt-in untouched,
and changes the pool worker in three ways. The full record and raw values are in
`bench/results/ws-net-n8-ab.log`.

### 1. Overflow prefers an idle worker

The registry grew a v2 load hint: each worker publishes `busy` (a ring has an
unanswered request) and `last_service_ms` with plain atomics in a registry
mapping it keeps for its lifetime. The attach policy still gives a ring its own
worker while a slot exists; when it has to multiplex, it now prefers, in order,
an idle worker (no outstanding request and nothing served in the last 25 ms), a
worker that already has spare capacity, and the least-loaded one. A mostly idle
ring therefore does not land on a worker that is in the middle of a batch. In
the all-busy benchmark every worker is busy, so this tie-breaks the same way the
old least-loaded policy did — which is why the N=8 number below does not move.

### 2. Each worker serves its ready rings round-robin

The old loop serviced only the first ready ring per `poll()`. It now walks every
ready ring once per turn, starting after the one served last, so no index owns
the head of the loop. One request is one batch, and a batch is the executor's
unit of work: that is the finest time slice a single backend can offer, since
the executor cannot be preempted mid-batch. This is fairness (a silent ring
cannot be starved by a chatty one), not extra throughput.

### 3. One savepoint per request

Every request now runs inside `BeginInternalSubTransaction`. A PG error is
caught at the request boundary: the savepoint rolls back, only that ring loses
its stream (its portal died with the savepoint), the worker's outer transaction
and every other ring's cursor and snapshot survive. The row-frame carry had to
follow: it was keyed by `Portal *`, which is exactly the pointer a savepoint
abort frees, so it is now keyed by the cursor name. A stream that fails drops
only its own carry; the next OPEN starts clean. Teardown (STOP, dead client)
uses the same contained close, so one bad stream cannot take the worker's other
rings down on the way out.

### What it measures

Three interleaved rounds of the ring-scaling driver (3 reps each, medians),
same tree, same image, one docker run at a time. `pre` is `merge/transport`
without this branch (`fpq-transport`); `post` is this branch.

| config | n=1 | n=2 | n=4 | n=8 | served |
|---|---:|---:|---:|---:|---:|
| `pre` eventfd, default pool | 9.73M | 19.73M | 36.04M | 34.47M | 8/8 |
| `post` eventfd, default pool | 9.45M | 19.29M | 34.25M | 34.93M | 8/8 |
| `post` default shm (1:1) | 9.67M | 19.79M | 37.49M | 54.26M | 7/8 |
| `post` eventfd, 1 ring/worker | 9.61M | 18.78M | 34.38M | 53.60M | 7/8 |
| `post` eventfd, 16 worker slots | 9.57M | 18.03M | 34.76M | 55.71M | 8/8 |
| `fpq-baseline` (main, shm) | 6.64M | 13.27M | 24.98M | 34.71M | 7/8 |

The before/after delta on the same tree is −2.9%, −2.2%, −5.0%, +1.3% at
N=1/2/4/8: inside this host's ±5–10% noise, so the savepoint and dispatch work
costs nothing measurable. At N=8 the pool is still **−35.6%** against the 1:1
path on the same image (34.93M against 54.26M): one worker carries two rings and
their batch turns serialise. With an eighth slot (`max_worker_processes=16`)
the same eventfd image reaches 55.71M at 8/8, and with overflow disabled it
reproduces the 1:1 shape (53.60M, 7/8).

### What scheduling cannot fix

The eighth ring is a *pinned stream*: its portal lives in one worker's
transaction, so every batch of it must run on that worker's OS thread. Serving
the two rings fairly splits one worker's capacity between them; it does not
create a second worker. While all eight readers are long scans, the seven
workers are all busy and the pair's wall time sets the run, so the aggregate is
bounded below the 1:1 path no matter how the queue is ordered. The only ways to
close the N=8 gap are more worker slots — the `max_worker_processes` control,
which is at parity — or splitting a stream across workers, which this portal
model does not do. (The old `fpq-baseline` cell is *lower* than the change only
because `merge/transport` made the whole object-free path faster; compare
same-tree cells, not across the merge.)

## What remains


- **Scheduling across a pinned stream.** The pool now prefers an idle worker
  when a ring has to overflow, and each worker serves its ready rings
  round-robin (one batch per ring per turn). That helps the mixed case — a new
  ring should not land on a worker that is in the middle of a read — but it
  cannot move a stream between workers: a portal lives in its worker's
  transaction. When *every* ring is busy, the worker carrying two of them is
  still the straggler, and no dispatch order changes that (measured in the
  ws/net-n8 section below). Re-homing a stream at a read boundary, not a better
  queue, is what would be needed next.
- **A worker that is interrupted.** A PG error is now contained by a savepoint
  per request, and only the failing ring loses its stream. A worker process that
  dies (OOM, SIGKILL) is still not contained: its rings lose their worker and
  the clients see the timeout; re-attaching them to a surviving worker is not
  implemented.
- **Snapshot semantics.** Multiplexed streams share one transaction and one
  snapshot instead of one transaction each. Read-only streams normally have
  their own snapshot; this is a deliberate simplification of the prototype and
  is documented here rather than hidden.
- **Cross-user clients.** The eventfd handshake requires the worker to connect
  to a socket the client owns in the same network namespace, and the client and
  worker must be able to pass fds (same UID in practice). The semaphore engine
  crosses users because the segment is mode 0666; the pool does not. The
  prototype is for the same-user / same-container deployment the benchmark
  uses.
- **One free slot to start.** The first pool worker still needs one
  `max_worker_processes` slot. If other applications' 1:1 ring workers have
  consumed the whole budget, the first eventfd ring cannot start. The pool
  removes the per-ring cost, not the cost of having a pool.
- **Per-database pools.** A worker is connected to one database, so the
  registry is per database OID; a client reading several databases gets one
  pool per database. That is correct, just not shared.
- **No client-backend portal variant.** The alternative — the client's own
  backend runs `PortalRunFetch` with a DestReceiver writing into the segment —
  needs a round trip over libpq (or another notification channel) per batch and
  would not use the shared-memory doorbell at all. It would give true
  parallelism per ring only if the client's backend were also the executor,
  which is what the ring exists to avoid. It remains a possible future engine
  behind the same seam, not this prototype.
- **The test suite's default is still `shm`.** `FASTPOSTGRES_TRANSPORT=eventfd`
  runs the whole suite green (including a new 8-ring test that exceeds the
  budget and proves the multiplex); the default keeps the 1:1 engine until the
  eventfd handshake has a deployment story for cross-user clients.

# The ORM materializer: a two-mode contract (ws/orm-semantics)

This is the consumer-side companion to the storage path above, and it is not a
storage change.  The native kernel is still `python/src/fastpostgres/native/materialize.cpp`:
allocate the model instance, fill its `__dict__` with the row's columns, append
it.  What was missing was an honest answer for a model the kernel cannot honor:
before this workstream it produced objects with no `InstanceState`, so a
relationship, a validator or `inspect()` failed later, after the read had
succeeded.  `fastpostgres.orm.materialize` now checks the mapper once, per
class, and picks one of two modes.

### The contract

| | fast path (flat model) | fallback (anything else) |
|---|---|---|
| object | model instance, columns in `__dict__` | stock ORM instance + real `InstanceState` |
| session | none | none |
| identity | no key, no identity map | detached key when the PK was materialized, else transient |
| `inspect(obj)` | `NoInspectionAvailable` | a real `InstanceState` |
| relationships | absent by construction | unloaded; access raises `DetachedInstanceError` (stock detached behavior) |
| assignment | plain `__dict__` write; no validation, no history, never persisted | normal instrumentation: validators run, history is recorded |
| `__repr__` | the class's `__repr__`, else Python's default | same |

The fast path is selected only when `mapper.relationships` is empty, there is
no `inherits`/`polymorphic_on`, no `version_id_col`, no `mapper.validators`, no
composites, no deferred or raiseload columns, and every mapper property is a
single-column `ColumnProperty` (so no synonyms or multi-column properties).
A partial `keys` projection also falls back: the native kernel has no way to
represent an unloaded attribute, and an instance without instrumentation turns
a missing attribute into `UnmappedInstanceError`, which is worse than a slow
correct object.

The fallback is built with `manager_of_class(model).new_instance()` and
`instance_dict`, not `model(**values)`: `__init__` is not called and the row is
written straight into the instance dict, exactly as query loading does.  When
the primary key columns are present, `make_transient_to_detached` gives the
object its real identity key, so it behaves like a loaded-then-expunged object:
`session.merge(obj)` is the attach path, and a relationship read fails loudly
rather than returning an empty collection for a row nobody loaded.  A model
quirk in that conversion leaves the object transient rather than turning the
read into an error; it still has real instrumentation.  For a polymorphic
mapper the row's discriminator selects the mapped subclass, so a
`polymorphic_identity="sub"` row comes back as the subclass rather than a base
instance; if the discriminator is not in the materialized keys, the base class
is used.

### Defaults, validators, events

- Column `default=`/`server_default=` are INSERT-time values.  Stock ORM
  hydration does not apply them either; a read row already carries the stored
  value.  They are never applied here, and a partial projection never invents
  one.
- `@validates` triggers the fallback.  Hydration itself writes the state dict
  directly, like stock query loading, so a stored value is not re-validated;
  the validator is there for the assignment that follows, which the fast path
  cannot honor.
- `__init__`, mapper init/load/refresh events and attribute set events are not
  fired.  Query loading does not call `__init__` or run validators either; the
  missing piece is the event surface, which is part of the ORM's state machine
  and therefore outside this contract.  Python-level descriptors (hybrids,
  `@property`) still run as written; one that reaches for a relationship or the
  session will fail the same way it would on a detached object.

### Identity and repr

Neither mode adds a `__repr__`.  If the model defines one it runs; otherwise
the address-bearing `object.__repr__` does.  Fast instances have no ORM
identity map, so two materializations of the same primary key are two distinct
objects.  Fallback instances carry a key but are still not in an identity map
(there is no session), which is why `merge` is the documented attach path.

### ResultSet

`merge/read-path`'s DBAPI `fetchall()` returns a lazy `ResultSet`, not a list.
`materialize` recognizes it and calls its `to_tuples()` once: that decodes the
whole columnar frame in C, while the native `PySequence_Fast` path would walk
the ResultSet's Python iterator and build a throwaway tuple list first.  This
is consumer-side only and does not touch the transport.

### Measured

`bench/results/ws-orm-semantics-ab.log`, 100K rows, 3 alternating runs,
`reps=5`, medians:

| cell | baseline `fpq-baseline` | change `fpq-orm-semantics` |
|---|---|---|
| driver tuples | 213.6 ns/row (4.68M) | 119.0 ns/row (8.40M) |
| stock ORM | 3373.4 ns/row (0.30M) | 3585.2 ns/row (0.28M) |
| materialize e2e | unavailable | **475.7 ns/row (2.10M)** |
| materialize only (fill) | unavailable | **161.1 ns/row (6.21M)** |
| materialize fallback | unavailable | 3793.3 ns/row (0.26M) |

Baseline stock ORM -> change e2e is -85.9% ns/row and 7.09x; against the same
image's stock ORM control it is -86.7% and 7.54x.  The fill alone is 22.25x the
same image's stock ORM.  The fallback is 5.8% slower than that image's stock
ORM, which is the point: it is stock, not a fast lie.  The driver control moved
between images (-44.3%) because the base carries the transport work; the stock
ORM control moved +6.3%, so read the materializer numbers against it.

### What remains

1. **No session semantics on the fast path, by design.** It is a read-path
   materializer: no identity map, no dirty tracking, no expiry, no lazy loads.
   The fallback covers models that need those features, but the fast path does
   not grow into an ORM.
2. **Plan caching is per class.** The mapper shape is probed on the first
   `materialize` call and cached.  Mapper options changed at runtime afterwards
   (for example toggling `raiseload` on an attribute) are not re-probed.  Mapper
   shape is normally fixed by configuration; a public cache clear would be the
   next small addition if callers need it.
3. **Partial projections have no fast representation.** They fall back so that
   unloaded attributes behave correctly.  A sentinel/"unloaded" value in the
   native fill could keep them fast later, but it would have to reproduce
   SQLAlchemy's deferred-load error semantics exactly to be worth it.
4. **Events and `__init__` are not replayed.** Documented above; replaying them
   would require constructing the state machine the fast path exists to avoid.
5. **Fallback is one instance at a time in Python.** It is intentionally the
   correctness path; a bulk `new_instance` loop is already the main cost, and
   optimizing it would not change which models are eligible.
