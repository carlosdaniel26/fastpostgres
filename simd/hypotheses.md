# Hot path: hypotheses, not yet measured

## What this directory used to hold

The kernel layer lived here: a CPU probe, a dispatch table, an AVX2 copy ladder,
an AVX2 and an AVX-512 null test, their scalar twins, and two harnesses.
Measured end to end it was worth about **0.5%** of the read phase - the null
scan 0.26 ns/row, the copy 2.6 ns/row on the benchmark's shape - which did not
pay for the assembly, the dispatch, the scalar twins that kept it honest, the
harness, or the "reads only what it writes" contract that had to be policed
(there is a guard-page check for that below). What is left of it is three lines
of C in `fastpostgres_shm.h` and a `memcpy`.

What the numbers below are still for: they are the record of what a
micro-optimization is worth against its share of the pipeline.

Static work only: the three hot loops were replicated 1:1 and compiled with the
flags the extensions actually use (`-O3`, baseline x86-64; `-O2` for the client
TU), and the generated code was read. No benchmark was run, so every number
below is either an instruction count from that assembly or a bound derived from
it. The `%` are fractions of the loop, not of the pipeline: the pipeline share
of each loop is the unknown, and it is why the last section exists.

Host: Ryzen 7 7700 (Zen 4) — AVX-512F/DQ/BW/VL/VBMI/VBMI2/BITALG/VPOPCNTDQ, no
512-bit license downclock, 512-bit ops double-pumped, 32 MiB L3, 1 MiB L2/core.
Relevant to width choices: masks are cheap here, gathers are not.

## What the compiler emits today

| loop | per value | what the asm shows |
|---|---|---|
| `columnar.c` fixed-width pass | **10 instr** | switch **is** unswitched (loop cloned per kind); 1 null byte load + test + branch; 1 strided 8-byte `Datum` load; 1 store of `width`; 3 pointer bumps |
| `columnar.c` varlena pass | **~25 instr + `call *8(%rax)`** | 14 instr head (null test, `Datum` load, varlena header, length, **per-value `p + len > end` check**), 2 (len ≤ 32), 2 (table load + indirect call), 7 tail; **4 stack-slot accesses per value**, because the call forces the loop's state to memory |
| `write.c` ingest (`col_datum` inlined) | **~26 instr** | kind switch **not** unswitched (compare chain inside the loop), 6 instr of null bit test, `memcpy` per value, store + bumps |
| all three, with `-mavx512f,avx512bw,avx512vl` | — | **0 vector instructions emitted** |

Two consequences worth stating plainly:

- The kernel table's indirection costs more than the kernel on the text path: the
  call is why `p`, `end`, `vals` and the offset base live on the stack, and why
  there is a `memcpy@PLT` branch at all. Inlining the copy (H2) is worth more
  than making the copy clever.
- Enabling the ISA in the C buys nothing (measured 0). The loops are strided-load
  plus per-value-branch; only a layout change turns them into vectorizable code.

## The allocation finding

`columnar_build` `palloc`s `vals` (`8 B x ncols x nrows`) and `nulls_arr`
(`1 B x ncols x nrows`) per chunk — 7.2 MiB at 100k rows x 8 cols. Anything
above `ALLOC_CHUNK_LIMIT` (8 KiB, `aset.c:85`) becomes a dedicated `malloc` block
that is returned to `malloc` when the context is reset (`aset.c:63-66`), and the
SPI context is dropped at `SPI_finish` — so the scratch is a fresh multi-MiB
allocation every chunk. The `b1e4a97` note already blames the page faults for
the greater part of the R3 loss (167 vs 128 ns/row integrated); the allocator
path above is the mechanism.

The ingest does not have this problem: `write.c:251` already keeps a `batch_ctx`
and reuses it with `MemoryContextReset` per batch. The encode is the asymmetric
one.

## Hypotheses

`F` is the fraction of the loop's time removed; the phase share is the unknown,
so the estimate is `F x share_loop x share_phase`.

**H1 — nullmaps are k-masks.** The wire bitmap is 1 bit/row, exactly a k-register.
`movq` + `kmovq` loads 64 rows; `vpmovm2b` expands to bytes; `vptestmb` +
`kmovq` builds the target bitmap. Sub-product: a `kortestb` accumulator gives a
`has_nulls` flag for the header, and both readers stop scanning the column
(`segment.cpp:168`, `write.c:244`).
*F* ~ 12-23% of the ingest loop (the null bit test is 6 of 26 instr; the
`no_nulls` flag is invariant per column and is tested per value today).
Alone: 2-5% of the ingest phase. Its real job is to make H3 possible.
*Falsifier*: a null-free column where the flag is already hoisted shows no gain.

**H2 — the copy ladder becomes one masked load/store.** `bzhi` + `kmovq` +
`vmovdqu8 {k}{z}` + `vmovdqu8 {k}` covers 0..64 B with no branch and no fault on
masked lanes (the same trick `pg_popcount_avx512.c:136-156` uses to clip a tail),
so it keeps the "reads only what it writes" invariant that `kernels.h:31` wants.
*F* ~ 40% of the varlena loop (25 instr + call + 4 stack accesses -> ~16, no call,
no spill, no `memcpy` branch, ceiling raised 32 -> 64 B).
Estimate: **3-6% of the encode phase and 3-6% of the ingest phase**.
*Falsifier*: constant lengths — the ladder is then 2 uops and the preditor wins.

**H3 — per-(kind, atttypid) kernels, chosen once per column, iterated in blocks.**
Ingest: 26 instr/value -> ~5-8 (contiguous load + `vpmovsx`/`vcvtps2pd` +
masked store). Encode: needs H4/H8 for a contiguous source first.
Estimate: **10-25% of the ingest phase**, 7-14% of the encode phase when paired
with H8.
*Falsifier*: `table_multi_insert` dominating the ingest; then the share is small.

**H4 — tile the transpose (R3 redux).** The R3 failure had two named causes:
a fresh page-faulted scratch and 6 MiB sweeps in L3. A tile of ~4k rows
(4k x 16 x 8 = 512 KiB, L2-resident) with a **reused** scratch addresses both,
and the sweeps become one pass instead of `ncols`.
Estimate: **2-6% of the encode phase alone**; it exists mainly as H3's enabler.
*Falsifier*: if H8 lands, H4 is moot — the scratch is gone and there is nothing
to transpose. H8 > H4.

**H5 — 64-byte column alignment (wire v3).** Today only 8 B is guaranteed
(`fastpostgres_shm.h:52`, `:264`). Needed for 64 B masked stores to avoid line
splits, and it falls out for free if H8 reserves the regions.
Estimate: ~0 alone; prerequisite for H2/H3's 64 B shapes.

**H6 — let the compiler vectorize the surrounding C.** Measured: **0%**, the
compiler emits no vector code for these loops even with AVX-512 enabled. Postgres
has the in-tree pattern for it (`pg_attribute_target` + a runtime availability
function, `pg_popcount_avx512.c:97-111`), but it only pays after the layout work.

**H7 — 256-bit with masks, not 512.** On Zen 4 the masks are the win and 512-bit
is double-pumped anyway. Zero perf by itself; removes the Intel downclock risk
and keeps `zmm` pressure low. Any new kernel that dirties `ymm`/`zmm` 0-15 must
return with `vzeroupper` (the AVX2 kernel avoids the question by staying 128-bit,
`kernels_avx2.S:5`).

**H8 — the allocation *is* the response buffer.** (The interesting one.)

## H8 in detail

Today the chunk is copied three times: the executor materializes the fetch into
`SPI_tuptable` (a tuplestore of `HeapTuple`s), `heap_deform_tuple` writes the
7.2 MiB row-major scratch, and the column passes write the payload into the
mmap. Per value: one write to the scratch, one strided read back, one write to
the segment. H8 writes the value once, straight into its column's region — the
mmap *is* the heap, and the allocator is a bump pointer per column.

Two shapes:

- **A (keeps the pull protocol).** Reserve a region per column up front and
  backpatch `data_len`; the reader stops walking sequentially and reads a column
  offset table instead. Removes the scratch, the faults and the strided sweeps;
  keeps the tuplestore.
- **B (streams).** Run the query with a `DestReceiver` and publish chunks as they
  fill — i.e. what `push_receive` (`worker.c:377`) already does, with the
  per-row frame replaced by per-column regions. Removes the tuplestore as well.
  Costs a protocol change: `ring.next()` becomes a pull-driven queue, which is
  what `push_ready`/`push_free` already implement. `simd-hot-path.md` (Stage 3)
  says "update or delete" push mode; this is the argument for updating it.

The constraint that decides the design: **the byte area of a varlena column has
no known size until the last row.** The offset table is exact (`(nrows+1) x 4`)
and can be written in place, but the bytes cannot. Options: a reserved capacity
per text column with an error if exceeded (chunks are cheap to retry), or a
sizing pass over the tuples (which the cursor path can do, since the tuplestore
is random-access). Fixed-width columns are exact and free.

About "free the ones not selected": in this format there is nothing to free. A
null varlena consumes zero bytes (only an offset entry) and a null fixed-width
value consumes its slot by design (`columnar.c:154`, so `data_len == nrows x
width`). The only place a *not-selected* value costs anything is that zeroed
slot, plus a `memset` per null row. Reclaiming it means a dense-values + bitmap
layout (the Arrow layout): the writer skips the `memset`, the reader pays a
prefix `popcount` to map row -> slot — free for a sequential reader, and on
AVX-512 it is `vpopcntq` over the null mask. Only worth it for null-heavy
columns; for null-free ones it is identical.

Where a real local heap *is* needed is the temporaries, not the values:
`pg_detoast_datum_packed` may allocate for toasted/compressed input, and
`OidOutputFunctionCall` always does. Those want a per-chunk context with
`MemoryContextReset` (the `batch_ctx` pattern) — and today they land in the SPI
context, which is fine, as long as the *values* do not.

Note what H8 does to the other hypotheses: on the encode it subsumes H1, H3 and
H4 (no scratch, no strided loads, no faults — the per-value work becomes 1 load
+ 1 store), so the SIMD work that remains on that path is H2 plus a mask for
nulls. On the ingest the source is already column-major, so H3 remains the win
there. Alignment (H5) comes for free, and the client's `synth_segment` generator
(which builds the same frame) needs the same change.

## H9 — the scan side: the value is already in the page

The script we control is the marshaller; the values themselves are already
materialised by Postgres, and the honest anatomy is this:

- a heap scan pins the buffer page and the slot can point **into** the pinned
  page (`TTSOpsBufferHeapTuple` / minimal tuples): `slot_getallattrs` gives
  Datums that are by-value for fixed width and **pointers into the page** for
  varlena. There is no copy at that point — PG is already zero-copy at the page.
- the copies that do exist have reasons: the buffer pin must be released before
  the page can be evicted or reused; toast/compression makes
  `pg_detoast_datum_packed` allocate; a sort/hash/spool materialises; and a
  virtual slot copies the Datums out of the page (varlena still by reference).

So "let the client read the pages the scan walked" is technically a **buffer
lease**: pin, publish `(relfilenode, block, offset)` into the segment, hand the
client a COW/shared mapping, unpin when it releases. The pieces already exist in
this repo — `push_free`/`push_ready` are the release handshake, and the worker
already detects a dead client (`kill(client_pid, 0)`, `worker.c:290`) to unpin.

Why it probably is not the win it looks like:

- the pages are **row-major**: line pointers, `t_hoff`, a null bitmap, per-value
  alignment and padding, 1-byte vs 4-byte varlena headers. A columnar consumer
  still has to gather strided, and it must re-implement the tuple layout in the
  client (a second deformer, in Python or C) or accept a row-shaped result.
- it removes one store per value (the write into the segment) — 1 of ~10
  instructions on the fixed-width pass — and pays with a longer pin lifetime and
  the deform moved to the reader.
- hint bits are flipped inside a pinned page (`t_infomask`), so a client may not
  trust header bits it cached; and TOAST/compressed values are not in the page at
  all, so the lease needs a copy fallback for them.

Verdict: worth it only for a **row-order** consumer, or if the storage itself is
columnar — i.e. a custom table AM (the Citus-columnar / zedstore shape), where
the pages are already per-column runs and a lease hands out exactly what the
client wants. On the query side, PG's own version of "reuse the value that is
already there" is an index-only scan; the extension cannot force the planner.

## H10 — reuse is the dictionary, not the pointer

"Boa parte dos valores é reaproveitável" is the implementable half of H9, and it
does not need pages or COW: it needs a **dictionary per chunk**. Low-cardinality
columns (bool, status, category, repeated text) can carry a distinct-value table
plus u32 codes, so the client creates **one object per distinct value** and
INCREFs it per row (the Arrow dictionary layout is the zero-copy form). The wire
change is a new column kind; the cost is a hash per value (worker or client, but
someone pays it), so it only pays when cardinality << nrows — which is exactly
the case for the columns that currently dominate object creation.

On the Python side this is also the only honest reading of "the object is born
around the value": a `PyLong`/`PyUnicode` **owns** its digits/chars (inline in
the object, not a pointer), so no integer or str can alias the segment. Only
view types can — `memoryview`, numpy, Arrow. For those, reuse means one shared
value and N codes; for tuple-per-row objects it means one object and N refcount
bumps.

## H11 — the object is born around the cell

Write only the `PyObject` header in front of a value that is already in its
final place, so there is no value copy and no allocator. Layout is the first
half of the question, measured on CPython 3.14.8 (x86-64, 64-bit, GIL build,
`Py_DEBUG` off):

| wire kind | object | inline field | bytes | verdict |
|---|---|---|---|---|
| bool | `Py_True`/`Py_False` | — | 0 | singletons: already 1 INCREF |
| int16/int32, -5..256 | small-int pool | — | 0 | **immortal since 3.12**: already your trick |
| int64 (large) | `PyLong` | base-2^30 digits at +24 | 28/32/**36** | the wire int64 is *not* the digit bytes; the client still writes 2-3 digits per value |
| float4/float8 | `PyFloat` | `double` at +16 | **24** | the perfect fit: 16 B of header and the value is already there |
| text | `PyUnicode` (compact ASCII) | inline bytes at +40, +NUL | 40+len+1 | works, but the header layout is version-specific (3.12 moved `state`) |
| date/time/timestamp | `PyDateTime` | several derived fields | 32/40/48 | the client writes 6-7 fields; still kills the allocator |
| uuid | `uuid.UUID` | an `int` attribute | 64 | not in place: needs an int object plus the wrapper |

The half that is not layout: **lifetime**. A `PyFloat`/`PyLong` handed to the
user has no dealloc hook into our arena, so its storage can never be reclaimed
with the chunk — the arena must be immortal, or the storage must be owned by a
view type with `bf_releasebuffer`. CPython's own answer to this is to make the
objects immortal (small ints, `None`, `True`/`False`, PEP 683); that is the
price, and it is why the trick only closes for storage that is *permanently*
stable.

Two realizations that survive that constraint:

- **Per-distinct-value pool** (this is H10, wearing the cell costume): the cell
  is born once per distinct value, reused with an INCREF, immortal for the
  process. Bounded memory, no per-row allocation, no per-row copy. This is
  literally the CPython small-int cache design, and it needs no ABI knowledge.
- **Client-owned arena + layout mode**: the client allocates one arena per
  result and hands it to the worker (`arg` already carries a segment name; a
  third segment or a memfd fits), the worker writes each value at its cell's
  *inline offset*, and the client writes only the header. Keeps the shared
  segment's lifetime out of the picture. Costs: 24-40 B of padding per value, a
  per-Python-version offset table, and a format committed to CPython's ABI
  (`Py_DEBUG`, free-threaded 3.13 changes `PyObject_HEAD` to a biased refcount),
  which poisons the frame for every other consumer (numpy/Arrow, other
  languages). Values that escape the result still can't be freed with the arena.

If the consumer can take views, H11 is unnecessary — the view *is* the cell, and
`__array_interface__`/`.offsets`/`.bytes` already do it. If the consumer needs
real tuples, the cheap 80% of H11 is: allocate the objects from a per-chunk
arena (one malloc, contiguous, cache-friendly traversal) with the value written
next to its header — the allocator churn goes away, the 8-byte value store stays,
and nothing depends on CPython's private layout.

## H11 measured — what variant 2 buys

CPython 3.14.8, this host, per operation, and then the bench table's row shape
(2 float8, 2 large int, 2 text of 8 and 20 B, 1 bool, 1 cached int, the tuple):

| primitive | ns | | ns/row |
|---|---|---|---|
| `PyFloat_FromDouble` | 13.0 | row, objects built | 123.6 |
| `PyLong_FromLong` (large) | 18.9 | row, headers only | **28.1** |
| `PyLong_FromLong` (cached) | 2.2 | — of which `PyTuple_New(8)` | 18.0 |
| `PyUnicode` 8 B | 23.4 | | |
| `PyUnicode` 20 B | 24.2 | | |
| `PyTuple_New(8)` | 18.0 | | |

**4.4x on the materialisation step (77% less).** Of the 105 ns of object work,
the in-place birth of the four fixed-width values costs ~7 ns and the two text
headers ~8 ns; the remaining 18 ns is the tuple, which is itself in-place-able
(`PyTupleObject` is a header plus an inline `ob_item[]`) — that is the next
floor, around 5 ns/row.

Staged, so the fragile part stays optional:

1. **fixed-width cells only** — float and int layouts are stable; a float header
   is 2 stores, an int is 5 (refcount, type, `ob_size` and its base-2^30 digits,
   because the wire int64 is *not* the digit bytes). ~45% off, no ABI knowledge.
2. **+ text in place** — 5 stores for the header, but the `state` bit positions
   are version-specific (they moved in 3.12). Reaches the measured 77%.
3. **+ birthed tuples** — past 77%, arena-allocated rows.

Worker-side price, not in the table above: the cells are 3-4x the packed wire
bytes (24 B for a float8, 40+len+1 for text), so the arena must be reused across
chunks and pre-faulted, and the worker's store footprint grows.

### H11 safe vs unsafe, and the ceiling inside the real benchmark

Same host, the benchmark's own row shape (`id`, `sku`, `name`, `price`, `stock`,
`active`), per row, including the tuple and the deallocations:

| | ns/row | |
|---|---|---|
| current (allocate + init + copy) | 89.9 | |
| safe (arena-backed subtypes, value copied into the cell) | 76.7 | **-15%** |
| in-place, immortal (unsafe) | 20.5 | -77% |

- `PyUnicode_FromKindAndData(1B, …)` 19.5 vs `PyUnicode_FromStringAndSize` 20.4:
  the UTF-8 scan is not where the text cost is. Nothing to win there.
- The 4.4x needs objects that are never deallocated. `float_dealloc` on an exact
  float calls `PyObject_Free()` on the pointer, so an arena-born exact float is
  heap corruption the first time the user drops a reference. Only a **subtype**
  with its own `tp_free` is safe (measured: the free hook fires — 10M arena
  frees for 10M objects), and that costs the `tp_alloc` round trip and the
  dealloc bookkeeping: -15%, not -77%. A subtype also changes `type(x)` while
  keeping `isinstance(x, float)` true — a real semantic change for a driver.
- Tier 2 would be writing the compact-ASCII `PyUnicode` header by hand on a str
  subtype (~5 stores instead of 19.5 ns), which is safe for the same reason but
  adds the per-version `state` bit layout. That is where the remaining -30% of
  the step lives.

Ceiling in `bench-sqlalchemy.log` (100k): CORE 1.27M rows/s = **787 ns/row**, ORM
0.17M = **5900 ns/row**. The materialisation is ~90 ns/row — 11% of CORE and 1.5%
of ORM. So H11 is worth about **-9% CORE / -1.5% ORM** unsafe, **-1.5% CORE /
-0.2% ORM** safe (tier 1), and maybe **-6% CORE** at tier 2. Everything the
driver controls on the CORE path — server encode, decode loop, object birth — is
about 28% of it; the rest is SQLAlchemy's own per-row processing. The "vs
psycopg2" headline (+12.1% at 100k) can move to at most ~+40% if all of it were
free.

### H11 implemented: an immortal arena in codec.cpp

Client-only, no wire change. `codec.cpp` carves cells out of one append-only
arena (`FASTPOSTGRES_OBJ_ARENA`, default 512 MiB, `0` turns it off), writes the
CPython header around a value written on the spot, and stamps the interpreter's
immortal sentinel on the refcount, so `Py_INCREF`/`Py_DECREF` return early and
the deallocator is unreachable. No layout is assumed: `fp_objpool_init()` copies
the string header from a real compact-ASCII string, the int tags from real longs,
and then proves the born objects (repr, hash, compare, round-trip, and that
decrefs do not move the refcount). Anything unexpected leaves the pool off and
every value falls back to its normal constructor.

`read_rows` on the 8-column synth shape, 100k rows, identical checksum with the
pool off and on (`PYTHONHASHSEED` fixed), 0 fallbacks:

| | ns/row |
|---|---|
| pool off | 383.5 |
| pool on | **183.0** |

**2.10x on the client read path.** In the real benchmark at 100k, fastpostgres
CORE rows/s, three runs each way:

| | run 1 | run 2 | run 3 |
|---|---|---|---|
| pool off | 1.37M | 1.41M | — |
| pool on | 1.68M | 1.58M | 1.60M |

**+16.5% throughput** (719 -> 617 ns/row); the "vs psycopg2" headline goes from
+30% to +48..62%. ORM is unchanged inside the noise, because that row is
SQLAlchemy hydration. `just test` is green.

The price is the design: the arena is append-only because an immortal object can
never be recycled, so the memory is never returned — ~29 MiB per 100k-row read
of this shape (118 MiB for the benchmark's six reads), mapped for the life of the
process. With a cap that is a fixed pool, not a leak that grows without bound:
once it fills, everything falls back. Worth note: the values are *exact* types
(`type(x) is float` still true), so nothing but memory is given up — and the same
measurement says the loop around the objects (per-value call + switch) is now
~165 ns/row of that 183, which is the next safe lever.

## H12 — the row frame: ship the server's own tuple bytes (implemented)

Everything above is about decoding; this one is about not encoding. The worker
used to lay a result out twice: SPI's `DestSPI` copies every row into a tuptable
(`heap_copytuple`, the whole row), and `columnar_build` then transposes those
rows into FPCOLUMN, per value. Neither copy reads a tuple as anything but a byte
string.

The row frame (`FPROWS`, `rows.c`) drops both. A DestReceiver of our own feeds
`PortalRunFetch` — the call `SPI_cursor_fetch` wraps, minus its hardcoded
receiver — and each tuple is memcpy'd once, straight out of the buffer page for
a plain scan, into the segment: a per-column descriptor, then per row a length
word, padding to MAXALIGN, and the tuple. The client walks that tuple in place
(`decode_tuple`, mirroring `heap_deform_tuple`: align, addlength, nulls at
`t_bits`) and hands each attribute to the same births the columnar path uses.
Copies from the page to the Python object: **3 -> 2**, and the 8 B per column per
row `Datum` staging array is gone.

The frame carries only what the client can walk: fixed-width values of their wire
kind, plus varlena text/bytea. Anything else — numeric, jsonb, name, money,
arrays, dropped attributes — takes the columnar frame for that batch, so values
and type OIDs are unchanged. `stream()`/`read()` keep the columnar frame: the
zero-copy column views need the transpose.

Measured, `just bench 100000`, against `bench-after.log` (same host, same shape,
object arena not exhausted — 2,690,077 objects, 0 fallbacks, as in the baseline):

| | runs | CORE 100K | vs psycopg2 |
|---|---|---|---|
| baseline (per-value loop, columnar frame) | 1 | 1.65M rows/s | +57.5% |
| the hoisted loop, columnar frame | 2 | 1.59-1.70M | +57..63% |
| the hoisted loop, row frame | 5 | **1.78-1.86M** | **+72..81%** |

So the whole gain is the frame, not the loop: about **+9%** of the row time
(606 -> ~545 ns/row), with the loop rewrite inside the noise here. That is worth
stating plainly, because the estimate was the other way round — item 1 read as
the big one off the synth harness, where the loop around the births was 165 of
183 ns/row on an 8-column shape. Against SQLAlchemy CORE the same absolute saving
is a much smaller fraction: SQLAlchemy's own per-row work is the bulk of the row,
the driver's share is ~20%, and the client's decode loop is a slice of that. The
hoisted loops still pay where they are used (the columnar frame: the fallback for
numeric/jsonb/name, and `read_rows` on a columnar chunk), and `stream()` was
never on this number at all.

Why the frame is worth ~9%: the worker used to make three passes over every row
— SPI's `heap_copytuple` into a tuptable, a deform into the `Datum` staging
array, then the transpose into FPCOLUMN — and now makes one, a memcpy out of the
buffer page into the segment, which is the copy SPI was already paying. The
client's decode is unchanged in kind: a tuple walk instead of flat column
strides (a few more instructions per value) and the same births. The bandwidth
that disappears is the *middle*: 90-odd bytes of row copied into private memory
and then re-laid-out per value, for a row the executor had already materialised
in a slot. Skipping it is a server-side win, so it shows up as a share of the
row time rather than a share of the driver's work.

Neither frame changes the pipeline's shape: the worker and the client alternate
per batch (the ring is synchronous, no double buffering), so the two sides add
rather than overlap, and what the worker stops doing comes straight off the
total.

### The pass that hid in the slot type

The receiver's fast path — one memcpy out of the slot's tuple — only applies to
slots that have a tuple: `TTSOpsBufferHeapTuple` (a plain scan) and
`TTSOpsHeapTuple`. Virtual and minimal slots both answer NULL for
`get_heap_tuple`, so `ExecFetchSlotHeapTuple(.., false)` *materialises* one:
`heap_form_tuple` out of a projection's values, `heap_tuple_from_minimal_tuple`
out of a tuplestore's. The receiver then copied that. Every projected row (one
expression anywhere in the list, a function scan, some joins) and every sorted
or aggregated row therefore paid two passes over every value plus a palloc —
invisibly, because the benchmark's plain column lists match the scan's tupdesc
and never took that path.

Instrumented receiver, counting which branch ran: plain columns, 700,000 fast
path / 0 values; `id + 0`, `active || ''` and `ORDER BY id` each 0 / 700,000.

The values are now written straight into the segment (`slot_getallattrs` plus
the `heap_compute_data_size`/`heap_fill_tuple` the rebuild path already used),
so a projected row is one pass again. A/B on the same probes, fresh process, 0
fallbacks, best of five:

| row | shape | materialise + copy | values |
|---|---|---|---|
| ~100 B | plain columns | 162 ns | 160 ns |
| ~100 B | one expression | 246 ns | **240 ns** |
| ~1.2 KB | plain columns | 494 ns | 481 ns |
| ~1.2 KB | one expression | 651 ns | **585 ns** |
| ~1.2 KB | ORDER BY | 1690 ns | 1682 ns |

That is the shape of the thing: one pass over the row, so it is worth ~10% of a
projected read at 1.2 KB, ~2% at 100 B, and nothing at all on the benchmark's
plain columns (which is why the CORE number did not move). The minimal-slot case
is a wash in time — a minimal-to-heap conversion is itself a copy — but one
allocation per row lighter.

What is left, and what each is worth:

- **the memcpy out of the buffer page**: ~90 B/row, 3-5 ns/row, ~1% of a CORE
  row in this shape. Removing it means the client reading PostgreSQL's own pages,
  and on Linux those are anonymous — `shared_memory_type=mmap` is the default, a
  `MAP_SHARED|MAP_ANONYMOUS` segment made before the postmaster forks, with no
  name, no fd and no shmid, so a process that was not forked from it cannot map
  it. Even with an fd (SysV plus `shmat`, or `/proc/pid/map_files`, which wants
  ptrace rights Docker's seccomp profile denies) the frame would owe buffer pins
  held until the client acks, a buffer-index-to-address translation, and the
  toast relation. For ~1%.
- **the value store into the object cell**: this is what H11-v2 would move to
  the worker — the same stores, plus a client-published CPython layout table and
  3-4x the server write footprint. ~2-4% at best, on the fragile side of this
  project's line.
- **the kernel zero-filling fresh arena pages**: ~200 B/row (118 MiB for 600k
  rows), ~10-15 ns/row — larger than the server's copy, and the price of an
  arena whose pages an immortal object can never be recycled out of. The only
  lever is fewer or smaller objects: views instead of tuples (`stream()`), or the
  cgroup-sized pool noted below.

### Where the time is now

`ring.next_rows()` is the worker's whole turn — the executor, the row write into
the segment, the ring's syscalls — and the `read_rows` after it is the client.
Timed apart, benchmark shape, 100k rows, one process, best of three:

| batch | worker | client | total | |
|---|---|---|---|---|
| 100k | 35.4 | 121.3 | **157 ns/row** | 6.38 M rows/s |
| 50k | 35.9 | 117.7 | 154 | 6.51M |
| 25k | 36.0 | 93.4 | 129 | 7.73M |
| 12.5k | 36.9 | 86.5 | 123 | 8.10M |
| 3k | 43.8 | 56.9 | 101 | 9.93M |

The client is 67-77% of a read now, and it is the only side batch size moves:
the segment chunk per batch goes from 10.8 MB to 324 KB, which is a working-set
and TLB effect, not a per-row one — the client's read-only mapping never gets
huge pages (`fp_map_ro` does not advise, and a tmpfs mapping cannot be promoted
after the fact, as the page section below measured: `ShmemPmdMapped` stays 0).
What the worker pays for small batches is a fixed cost per fetch of ~25 us (the
ring plus `shm_open`/`fchmod`/`mmap`/`madvise`/`munmap` around every batch),
visible as 35 -> 44 ns/row at 3k.

**And it is worth nothing at the benchmark level.** `_SHM_BATCH = 25000`, three
runs: CORE 1.79-1.87M rows/s against 1.78-1.95M at 100k. Hydration reads every
attribute of every row anyway, so it drags the objects through cache whatever
the batch's footprint was — the microbenchmark's gain was an upper bound that
the consumer absorbs. That is the third ring-level win absorbed this way (the
hoisted loop, the batch size, and H11-v2's premise).

What is left per row at 100k batches: ~20-30 ns of births (already the
4.4x-cheaper immortal cells, against CPython's 13-24 ns per object), ~25-35 ns
of tuple and list, ~10-15 ns of the kernel zeroing fresh arena pages, ~5 ns of
the server's memcpy, and the executor's own per-row cost under the worker's 35.
The remaining room is the API, not the copies: values that are never
constructed (`stream()`), or a consumer that does not need a tuple per row. The
one exception worth naming is latency rather than throughput: at a 3k-row fetch
the ~25 us per-batch cost is 8 ns/row, so caching the worker's segment mapping
across batches (the client's mapping is already cached) would pay for small
interactive reads, and nothing at all for the benchmark's 100k batches.

Two traps, both of which produced wrong values before they produced a failing
assertion, both now guarded by the build:

- **the 1-byte varlena header is byte-order dependent.** `varatt.h` keeps two
  encodings behind `#ifdef WORDS_BIGENDIAN` — tag bit low on little-endian, high
  on big-endian, and the 4-byte header shifts rather than masks. The first mirror
  here was written from the big-endian block, so a 6-byte short `bytea` (`0x0d` =
  tag 0x01, size 6 << 1) decoded as garbage. `rows.c` now fails the build if the
  mirror's byte order disagrees with the server's.
- **a tuple must start MAXALIGNed.** `heap_fill_tuple` aligns a datum by its
  *address*, `heap_deform_tuple` by its *offset*; the two agree only when the
  tuple starts on an 8-byte boundary. The first frame put each tuple immediately
  after its 4-byte length word, which broke the rebuild path the moment anything
  needed detoasting. Hence the padding.

What arrives extended — a toast pointer, or a value compressed inside the tuple
— is resolved by the worker, because the client cannot detoast in place, and a
tuple written before an `ADD COLUMN` is expanded for the same reason. Both are
rebuilt with PostgreSQL's own `heap_compute_data_size`/`heap_fill_tuple`, in a
per-row memory context, so the segment never carries a pointer its reader cannot
follow.

## End to end: the driver's share, and SQLAlchemy's price

One process, one table of 100k rows in the benchmark's own shape, read through
the benchmark's own `read_core`/`read_orm`, with the layers underneath timed
separately as well (median of three, 1 GiB arena, 0 fallbacks):

| stage | fastpostgres | psycopg2 |
|---|---|---|
| worker turn (executor, row write, ring) | 38.5 ns | — |
| client decode (Python objects) | 133.3 | — |
| **driver: cursor execute + fetchall** | **166.7 ns** (0.60M rows/s) | **521.4 ns** (0.19M) |
| the same rows over libpq instead of shm | 533.6 | — |
| **SQLAlchemy CORE** | **542.0 ns** (0.18M) | **943.2 ns** (0.11M) |
| **SQLAlchemy ORM** | **5409.9 ns** | **6254.0 ns** |

Reading down our column: PostgreSQL's executor plus the one write into the
segment is 38 ns, the Python objects built out of it are 133, and everything
above is someone else's. SQLAlchemy's own share is then the same for both
drivers — 375 ns of the CORE row for fastpostgres against 422 for psycopg2 —
so the entire headline is the driver's 355 ns gap. The libpq fallback on its own
lands at psycopg2 parity (534 against 521), which is the other half of the same
sentence: the shared-memory path is the whole of the +79%.

For the ORM row the arithmetic is starker, and split again:

| SQLAlchemy layer | fastpostgres | psycopg2 |
|---|---|---|
| session + result plumbing + `Row` objects, no entities | 1376 ns | 1710 ns |
| **ORM entities on top of that** | **~4034 ns** | **~4544 ns** |

Three quarters of an ORM row is SQLAlchemy instantiating the model, setting six
attributes and doing whatever the identity map does — 4.0-4.5 us per row, the
same whichever driver feeds it. That is the ceiling this project works under:
nothing on the driver side can move ORM more than ~3%, and CORE is capped near
+40% over psycopg2, as H11 said before the row frame took a bite out of it.

One caveat on sizes: at 1M rows the benchmark's own CORE number falls to 1.16M
rows/s from 1.86M at 100k, because the 512 MiB immortal arena fills and every
value after that falls back to the normal constructors. Every measurement above
is at 100k for that reason; the fix is the cgroup-sized pool noted below.

### The CORE read without CORE

Since the CORE layer's 375 ns is its own Result and Row machinery, a statement can
be compiled once — SQLAlchemy's compiler, literal binds — and run as the plain
SQL the ring already speaks, which hands back the driver's tuples.
`Connection.fetchall(select(...))` in `dbapi.py` is that, and it lands where the
driver does:

| | ns/row | rows/s |
|---|---|---|
| driver, cursor execute + fetchall | 152.9 | 0.65M |
| **CORE stand-in** (`conn.fetchall(select(...))`) | **159-166** | **6.0-6.3M** |
| SQLAlchemy CORE | 524.2 | 0.19M |
| SQLAlchemy ORM | 5620.2 | 0.02M |

**-69%** against CORE, and ~**-82%** against psycopg2's CORE (943 ns/row in the
table above), on the same SQL with the same values — the test asserts the
stand-in returns CORE's rows row for row. The only thing it adds to the driver is
one compile per call, and that is a hit in SQLAlchemy's compiled cache as long as
the dialect instance is a singleton (a fresh `postgresql.dialect()` per call
defeats the cache: it cost 24 ns/row before this, 6 after).

The trade is the Row wrapper: the rows are tuples, with no named access and no
Result API. Which is the point — that wrapper is the whole of CORE's 375 ns.

`fastpostgres.core` is the same thing as an *interface* rather than a method:
`create_engine` (a conninfo or a `postgresql+fastpostgres://` URL),
`engine.connect()` as a context manager, `conn.execute(statement)` and
`.all()`/`.first()`/`.keys()`/`rowcount` — the shape CORE code already has, with
the driver's tuples underneath. The benchmark's own `read_core()` runs against it
unchanged:

| engine, through the benchmark's `read_core` | ns/row | rows/s |
|---|---|---|
| SQLAlchemy CORE | 524.2 | 0.19M |
| **fastpostgres.core** | **173.0** | **5.78M** |

-71%, and the 12-15 ns between it and the bare `conn.fetchall` is the engine's
per-call work (a statement built per call, a cache lookup, the Result object),
not per row. One connection, reused, and nothing thread-safe: a read path needs
neither, and a libpq connect per `engine.connect()` would cost milliseconds.

### The arena was measuring itself

The benchmark's default run reads 100K, 1M and 10M in one process, and the object
arena is append-only — so by the time that session's CORE phase runs, the ORM
phases have filled it. The last such run logged **107.7M fallbacks**, and CORE
read the same 1.73M rows/s at 100K that a pre-row-frame chart shows, because every
value after the arena tipped over was built by the normal constructors. The chart
was measuring the pool, not the driver.

Two changes: `benchmarks.sh` runs one process per size, so each load gets a fresh
arena; and the arena's default is no longer 512 MiB but half the cgroup limit
(physical memory when the cgroup is unlimited), capped at 8 GiB — what the H11
note asked for, still overridable with `FASTPOSTGRES_OBJ_ARENA`.

Re-measured, default environment, one process per size, **0 fallbacks everywhere**:

| load | arena used | CORE, psycopg2 -> fastpostgres | ORM, psycopg2 -> fastpostgres |
|---|---|---|---|
| 100K | 118 MiB | 1.03M -> **1.89M** (+84%) | 0.16M -> 0.18M (+10%) |
| 1M | 1.2 GiB | 0.84M -> **1.46M** (+74%) | 0.15M -> 0.15M (+2%) |
| 10M | 3.9 GiB | 0.86M -> **1.68M** (+96%) | 0.14M -> 0.15M (+5%) |

The 10M row is the one to read twice: psycopg2 sheds a fifth of its rate between
1M and 10M while ours gains, so the gap *widens* with size instead of narrowing.
The arena footprint — 3.9 GiB for 89.7M objects — is what the cap has to cover,
which is why sizing it from the machine came before quoting the chart. The chart
now carries that footprint in its footer (and says when a run exhausted it), so
the flattening curve has its cause next to it.

## Where parallelism pays (measured)

Two constraints decide this before any experiment does. The client's decode holds
the GIL — the only `Py_BEGIN_ALLOW_THREADS` in the read path sit around libpq and
the ring's `sem_timedwait` — and the object arena's bump pointer and the segment
map cache (a `std::vector`) are unsynchronized: the GIL is what keeps them
correct, so a threaded decode owes per-thread arenas before it owes anything else.
And a PostgreSQL backend must not use threads at all, so server-side parallelism
is more processes, never threads.

Measured, 8-core container, reads of 100k rows through the DB-API (execute +
fetchall), total rows/s, one ring per worker:

| workers | threads | processes |
|---|---|---|
| 1 | 5.78M | 5.09M |
| 2 | 6.10M (+6%) | 9.02M (+77%) |
| 4 | 6.45M (+12%) | 12.05M (+137%) |
| 6 | — | 11.80M (+132%) |

Threads get the worker's turn and nothing else: 25% of a read is the worker, 75%
is the client's decode, and the GIL serializes the second. Processes get both and
plateau at four, where the cores and the memory traffic run out — each stream
moves ~350 B/row through the segment and the arena.

Two things the measurement turned up:

- **a forked worker inherited the parent's ring name.** `_pool_prefix` was
  computed at import, and gunicorn/uwsgi/multiprocessing all import first and fork
  after, so every worker opened the *same* ring and the *same* segment: the run
  showed 1 shm read out of 8, the rest silently on libpq, at 3.04M rows/s — slower
  than a single process. The stem is per-pid now, a pid change resets the pool
  (the parent's rings are not the child's), and a test forks and checks the names
  differ.
- **the ring budget is `max_worker_processes`.** One background worker per ring, 8
  by default, and the seventh `Ring.start` already fails ("could not register
  background worker"); past that the DB-API marks the pool broken and every read
  falls back to libpq, silently. Practical concurrency is 7 rings system-wide, so
  `FASTPOSTGRES_SHM_POOL` (4 per process) wants lowering when several processes
  share a server, or `max_worker_processes` raising.

What is left for a *single* stream is pipelining rather than paralleling: the ring
is synchronous, so the worker's 38.6 ns and the client's 130 ns add. Two
alternating data segments, with the next request issued before the current batch
is decoded, would hide the worker's turn behind the decode — ~1.2x on a read, ~5%
on CORE — with no threads and no GIL to fight.

### Pipelining, measured

That is built now: the cursor is keyed by a stream name, the segment to publish
into is arg 2 of the NEXT (unused until then), and `Ring.post_rows`/`wait` let
`dbapi._shm_read` ask for the next batch before decoding the current one into the
other segment. A/B on the same harness, 400k-row reads, interleaved runs, best of
three:

| batch | synchronous | pipelined | |
|---|---|---|---|
| 100k | 158-163 ns/row | **132-135** | 1.19x |
| 25k | 120-121 | **86-90** | 1.38x |
| 6k | 122-123 | **84-85** | 1.44x |

End to end, the benchmark at 1M rows (ten batches): CORE **1.46-1.48M -> 1.53-1.54M
rows/s, +5%**, twice, 0 fallbacks. At 100k rows a read is one batch, so there is
nothing to overlap and nothing moves — which is why the first measurement of this
looked like zero.

One piece of housekeeping that the error path needed: if a read fails with a
request posted but not collected, the response is drained before the ring closes,
or the semaphore is left unbalanced for whoever checks the ring out next.

### What is actually parallelizable, per row

The phase split again, with each stage's legal parallel mechanism and what it could
take out of a CORE row (100k rows; copy work is the bytes moved over the measured
16.8 GB/s single-core and 24 GB/s four-thread memcpy):

| stage | ns/row | share of CORE | legal parallelism | best case |
|---|---|---|---|---|
| PostgreSQL executor + our tuple write | 38.6 | 7% | processes only, one worker per slice | 38.6 -> 9.7 at 4: **-5.3%** |
| client decode (C++, GIL held) | 133 | 25% | value copies only (~11 ns of it) | **-1%** |
| SQLAlchemy CORE (Result / Row) | 375 | 69% | none: its own glue | 0 |
| *of which, copy work anywhere in the read* | ~25 | 4.6% | threads (1.43x measured) | **-1.4%** |

Which reverses the intuition and is worth stating plainly: **inside one read there
is no mass parallelism to win.** The copy work is 15% of a driver-level read (25 of
167 ns), and threading it perfectly saves 1.4% of CORE. The whole worker turn is
7% of CORE, so sharding a single query across four workers is 1.05x on CORE
(1.21x on the driver-level read) — and pipelining two segments captures that same
5% without sharded portals, so it dominates the sharded reader for one query. The
ORM row adds ~4.9 us/row of SQLAlchemy hydration above that, all of it Python and
none of it ours to parallelize.

The 3.0x is real, but it is *work* parallelism: four independent reads in four
processes, each with its own GIL, its own worker and its own segment. That is the
lever for "tons of loops and batches", and it needs no new mechanism — only
`FASTPOSTGRES_SHM_POOL` x processes <= 7, or a larger `max_worker_processes`.

### The copies are not the bottleneck

"Parallelise the mass copies" is worth testing with bandwidth rather than cores: a
memcpy is already vectorised, and a threaded one reaches DRAM before it reaches
the core count. Measured with numpy (which releases the GIL so threads can run),
256 MiB per copy, best of five:

| threads | aggregate |
|---|---|
| 1 | 16.8 GB/s |
| 2 | 21.0 GB/s |
| 4 | 24.0 GB/s |
| 8 | 23.6 GB/s |

1.43x at four threads and flat at eight — the wall is the memory system. Against
that, a read of the benchmark's shape moves ~410 B/row (segment in, segment out,
arena), which at the measured 12.05M rows/s aggregate is 4.9 GB/s: a fifth of the
ceiling, and one core's 16.8 GB/s is six times what a single stream needs. A row
costs per-value work — the walk, the switch, the births, the tuple and the list —
not bytes. Even the wide shape (1.2 KB, 481 ns/row) runs at 2.9 GB/s.

So the tempting version of the idea does not pay: threading the client's value
stores would win at most the copy share of a decode (~11 ns of 130 for the arena
writes, before per-thread arenas), and on the server there are no threads to have
at all, because a PostgreSQL backend must not use them. Extreme throughput there
is *work* parallelism across concurrent queries — separate ring workers, measured
at 2.4x on four processes and capped at seven by `max_worker_processes` — not
spreading one copy over cores, and not sharding one query either, which the table
above prices at 1.05x. The only copy-heavy path in the tree is ingest, and it has
no client-side producer since the generators were deleted.

### The three fast paths, in the benchmark

The benchmark's third phase now runs what a consumer can opt into, against CORE on
the same rows (`FAST PATHS` in the output, one process per size):

| rows | SQLAlchemy CORE | core stand-in | `stream()` + numpy | CORE x 4 processes |
|---|---|---|---|---|
| 100K | 1.73M | 6.81M (**+293%**) | 7.52M (**+334%**) | 3.69M (**+113%**) |
| 1M | 1.44M | 6.53M (**+353%**) | 7.76M (**+438%**) | 4.21M (**+192%**) |
| 10M | 1.15M | 3.22M* | 8.15M (**+610%**) | 2.47M* |

\* the 10M object-building rows are pool-limited: that phase reads the table six
times in one process, so the immortal arena fills and they fall back to the normal
constructors. `stream()` never builds an object, so it is unaffected — and it is
the row that scales, +334% -> +610% as the load grows.

The shape of it: the object-free paths are worth 3-6x, the object-building stand-in
is worth 3-4x while the arena can hold its cells, and processes are worth 2-3x and
compose with both.

## Page mechanisms (measured on this host)

Facts first, because they decide which mechanisms exist:

| | here |
|---|---|
| THP anon | `always` |
| THP shmem | `advise` (the segment is tmpfs: nobody advises it) |
| THP defrag | `madvise` |
| PMD | 2 MiB |
| `unprivileged_userfaultfd` | `0` — needs CAP_SYS_PTRACE, so no fault-driven flow control |
| `HugePages_Total` | `0` — `MAP_HUGETLB` unavailable without configuring a pool |
| swap | 63 GiB, so tmpfs pages (the segment) are swappable under pressure |

Where page faults actually are, per 100k-row `read_rows` (faults from
`getrusage`, RSS from `statm`; the 6.8 MiB segment):

| | cold read | per warm read |
|---|---|---|
| pool off | 9336 faults | **8500 faults** (+9.5 MiB RSS) |
| pool on | 3079 faults | **2400 faults** (+27 MiB RSS) |

- the segment is **fault-once per process**: the cold read pays its ~1700 PTE
  faults and every read after that pays none. The map-once/reuse design is
  already page-optimal.
- the real fault cost is the **Python heap churn**: 8500 faults per read with
  the normal constructors, 2400 with the immortal pool (the cells are touched
  once, and `AnonHugePages` shows 26 of 27 MiB backed by 2 MiB pages). The pool
  was therefore also a page win (-6100 faults/read), and what is left is the
  tuple per row.
- `MADV_HUGEPAGE` on an already-populated tmpfs mapping does nothing
  (`ShmemPmdMapped` stays 0): shmem THP only applies to new allocations, so the
  worker would have to advise *before* writing, and it would only cover the
  ramp-up; `MADV_COLLAPSE` would pay a ~1.4 ms copy per 6.8 MiB to save ~1700
  PTE faults, which loses. It only pays for payloads of ~50 MiB and up, where
  4 KB pages (25k of them) overflow the ~3k-entry L2 TLB and the strided column
  sweeps start missing.

**Guard pages as an asm verifier** (the mechanism this project should own). The
kernel layer's contract is "reads only what it writes", and a wrong hand-written
kernel is a crash in a backend, not an exception — but `kernels_bench.c` can only
see wrong *answers*, not an over-read that happened to stay inside the buffer.
Mapped each buffer so its last byte abuts a `PROT_NONE` page and ran the real
kernels: `copy_small` at 0..64, `nullmap_any` at 14 sizes (4095/4096/4097/8192),
the mixed 64+13 tail case, and both **asm cores** at exactly one block. All
inside, and the harness proves itself first (a deliberate 1-byte over-read dies
with SIGSEGV). ~150 lines, and it turns the project's biggest risk class from
silent into deterministic.

Smaller ones:

- **page-aligned columns**: today one payload is one contiguous pack, so reading
  one column's view faults its neighbours' pages too. Page-aligning column
  regions makes a selective (numpy/Arrow) consumer fault only what it reads.
- **a promised slack page instead of the per-value bound check**: if the format
  guaranteed mapped slack at the end of the payload, the `p + len > end` test in
  the text loops (~12% of that loop, measured) is the MMU's job instead.
- **`MADV_DONTNEED`/`MADV_REMOVE` as policy, not speed**: a long-lived worker's
  touched segment pages are allocated forever; the consumed part can be given
  back. Related: tmpfs pages are swappable, so a payload under pressure becomes
  major faults on the next read.
- **the object pool should size itself from the cgroup**: 512 MiB is hostile in a
  small container; `memory.max`/MemAvailable is the right input.

## What would close the numbers

The missing term is the pipeline share of each loop. That is not a new benchmark,
it is three timers and a counter set:

- `clock_gettime` around the fetch, around `columnar_build`, around `write_end`
  (encode), and around the datum loop vs `table_multi_insert` (ingest);
- `perf stat -e page-faults,L1-dcache-misses,branch-misses` per phase — the H8
  claim is a fault/traffic claim, not an instruction claim, so it will show
  there and not in the instruction counts.
