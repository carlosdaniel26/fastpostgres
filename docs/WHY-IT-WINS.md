# Why this beats Arrow here, and why Arrow does not do it this way

Two questions, answered in that order:

1. **Why does this repository outperform Apache Arrow** on the workloads it
   measures?
2. **Why doesn't Arrow do the same thing** — is the technique unknown, or
   forbidden, or simply not their problem?

The short answers, before the long ones. To question 1: it is not one trick. It
is a small set of decisions that compound — §4 takes six of them one at a time:
the producer writes the bytes once, the layout is fixed so the reader walks it
instead of parsing it, the decoded form is a view rather than a reconstruction,
the client's columns never cross a language boundary, the bytes reach the client
through shared memory rather than a pipe, and the producer itself lives inside
the server. To question 2: Arrow *cannot* adopt them without becoming a different
product, and the one time the ecosystem tried, it built the idea outside Arrow
and then removed it from Arrow on purpose.

Everything numeric below is from the two logs committed in this repository
(`docs/codec-transport.log` for the format and transport isolation matrix,
`docs/fastpostgres-vs-arrow.log` for the query-level comparison), measured on one
machine with the versions in the footers: PostgreSQL 18, pyarrow 26.0.0, ADBC
1.13.0. Where a claim is *not* measured, the text says so. The list of things
this design gives up in exchange is in [`LIMITATIONS.md`](LIMITATIONS.md).

---

## 1. The numbers, so the rest of the document has an anchor

**Query level, same output on both sides** (Python tuples on the left column, an
Arrow table converted with `to_pylist` on the right — the harness's `TUPLE READ`
rows):

| rows | this repo | ADBC + Arrow | difference |
|---|---|---|---|
| 100K | 6.42M rows/s | 1.28M rows/s | **+401%** |
| 1M | 7.19M rows/s | 1.27M rows/s | **+467%** |
| 10M | 7.25M rows/s | 1.27M rows/s | **+472%** |

**Query level, columnar output, no Python objects on either side** (one column
summed in place — the `ZERO-COPY READ` rows):

| rows | this repo | ADBC + Arrow | difference |
|---|---|---|---|
| 100K | 8.20M rows/s | 2.23M rows/s | **+269%** |
| 1M | 8.25M rows/s | 2.28M rows/s | **+261%** |
| 10M | 8.32M rows/s | 2.18M rows/s | **+281%** |

**Format only**, 1M rows, transport held out entirely, one process
(the `fpcolumn` vs `arrow-ipc` cells):

| | this repo | Arrow IPC |
|---|---|---|
| payload | 47.2 MiB | 45.7 MiB |
| encode | **3.1 ms** (16 GB/s) | 7.8 ms (6 GB/s) |
| decode + six aggregates | **1.7 ms** | 1.9 ms |
| encode + decode | **4.8 ms** | 9.7 ms |
| copies of the payload | **1** | 6 |
| peak RSS | **100 MB** (2.1× the payload) | 383 MB (8.4×) |

**Transport only**, the *same payload bytes*, codec out of the loop:

| | shared memory | UNIX socket |
|---|---|---|
| throughput | **11.18 / 11.17 GB/s** | 6.93 / 7.40 GB/s |
| median latency | **3.6 µs** | 32.8 / 33.0 µs |
| p99 latency | **6.1 / 7.5 µs** | 411 / 408 µs |
| CPU, producer + consumer | **4.5 / 4.4 ms** | 7.1 / 6.5 ms |
| copies | **1** | 2 |

Read together: at the query level the gap is 3.6–5.7×, at the format level it is
2×, and at the transport level it is 1.5–1.6× on throughput and ~9× on median
latency. The factors multiply rather than overlap — which is what the isolation
matrix was built to show, and what the rest of this document explains
mechanism by mechanism.

---

## 2. The two pipelines, stage by stage

### This repository

```
PostgreSQL executor
   │  tuple bytes (the heap's own representation)
   ▼
extension: row frame or columnar frame
   │  written ONCE into the shared segment (memcpy of whole columns)
   ▼
shared memory segment (mmap in both processes)
   │  no kernel copy: the client reads the pages the producer wrote,
   │  and a semaphore doorbell plus a sequence counter announce each chunk
   ▼
client: decoded columns are POINTERS INTO THE PAYLOAD
   │  (bounds-checked while walking, then never copied again)
   ▼
aggregates / Python objects, built from those pointers
```

Copies of the payload: **1**. Allocations: **one segment per process**, grown
append-only rather than one per frame. Kernel copies: **0** — the handshake is
two semaphore doorbells per chunk (`sem_post`), not a `read`/`write` of the data.

### ADBC + Arrow

```
PostgreSQL executor
   │  tuple bytes
   ▼
libpq wire protocol
   │  text or binary, per column, over a socket
   ▼
driver (client process): parse the wire format
   │  copy 1: values -> Arrow arrays (numpy / buffer allocation)
   ▼
Arrow record batch (immutable, owned)
   │  copy 2: arrays -> serialized IPC stream, if the bytes must leave
   │  copy 3: stream -> the sink they are written to
   ▼
consumer: parse flatbuffer metadata, rebuild array objects
   │  copy 4..6: temporaries materialized by the compute kernels
   ▼
aggregates / Python objects
```

Copies of the payload in the codec cell: **6** — 2 in the encode path, 1 to the
sink, 3 for the aggregate kernels' temporaries. Peak RSS **8.4× the payload**,
because dataset, Arrow buffers, IPC stream and kernel temporaries are alive at
once, where the frame is built into the buffer it ships.

Both pipelines are correct, both carry identical bytes (the matrix checksums
prove it), and both produce the same answers. They differ in how many times
those bytes are rewritten on the way.

---

## 3. Where exactly the time goes

Splitting the format column, per 1M rows:

| stage | this repo | Arrow IPC | why |
|---|---|---|---|
| encode | 3.1 ms | 7.8 ms | **this is the real format gap**: 2.5× |
| decode | 1.7 ms | 1.9 ms | a scan; both are memory-bound, 12% apart |

The decode is *not* where the formats separate, and saying otherwise would be
dishonest: both sides read ~25 MB of numeric columns and offset tables and are
within 12% of each other. The separation is in encode (writing the bytes out)
and, at query level, in the transport (moving them).

That 2.5× encode gap is worth dwelling on, because the project found it the hard
way. The first version of this matrix had Arrow *winning* the format cell, and
investigation showed the harness, not the formats, was responsible:

- the C++ unit was built at `-O2` while pyarrow ships SIMD kernels — at `-O3`
  the frame's decode alone went from 2.67 ms to 1.76 ms;
- the encoder copied each text **value** with its own `memcpy` — about 2M small
  copies per run — where a serializer holding contiguous buffers copies each
  **column** twice (offsets, then bytes).

Both are fixed. Arrow's numbers barely moved (7.3 → 7.8 ms). That asymmetry is
the evidence: the gap was on this side of the harness, and once removed, the
remaining 2.5× is the format's own structure — three copies of the same bytes
(arrays, stream, sink) against one write into the buffer that ships.

---

## 4. The six mechanisms, one at a time

### 4.1 The producer writes into the buffer it is going to ship

The columnar frame's encoder in this repository *is* the payload: it takes the
destination buffer and `memcpy`s each column's data into it, once, aligned, with
the offsets table written ahead of the text bytes. There is no intermediate
representation and no second pass, because the destination layout is the source
layout.

Arrow IPC must go values → arrays → stream → sink, because the stream is a
*serialization of* the arrays and the sink may be a file, a socket, or another
process. Each hop exists so that the reader on the other side can be anyone.

**Cost:** the frame cannot be reused across queries (the arena is append-only),
cannot be compressed, and cannot be encrypted in place. **Evidence:** 1 copy
against 2 in the encode path; 16 GB/s against 6 GB/s.

### 4.2 The layout is fixed and known on both ends, so the reader walks it

The frame's header is five fields — magic, version, flags, column count, row
count (28 bytes). Each column then carries: name length, name, a one-byte kind,
the PostgreSQL type, a null bitmap, a data length, padding to 8-byte alignment,
then the data — and for text, an int32 offset table followed by the concatenated
bytes. That is the whole format. A reader walks forward once, allocating nothing:
`fastpostgres_reader_next` is a few pointer bumps, and it *refuses* a column whose
length would run past the payload.

Arrow's IPC stream starts with a flatbuffer message: schema, field nodes, buffer
descriptions, offsets, dictionary batches. That metadata is what makes the bytes
readable by a process that has never seen the writer — and it is also a parse
that must complete before the first value can be read.

**Cost:** anything not in the frame's kind vocabulary (13 kinds today) cannot be
carried; a reader that does not know the version must refuse the payload rather
than guess, which is exactly what `FASTPOSTGRES_COLUMNAR_VERSION` enforces.
**Evidence:** the decode numbers (1.7 vs 1.9 ms) — modest, because both sides
spend their time streaming memory, but the frame's walk is what leaves room for
the aggregates in the same budget.

### 4.3 The decoded form is a view, not a reconstruction

When the client decodes the columnar frame, a "decoded column" is a pointer into
the payload, a stride, and a null bitmap. Nothing is allocated, nothing is
transformed. The aggregates (`Σ id`, `Σ stock`, …) fold directly over the payload,
and the cost of doing that is measured: **16.7 cycles per row** for the whole
columnar path (the cell's own timers split it **11.8 encode + 6.6 decode**, the
last one being the six streams and the six aggregates: 6.8 loads/row, 1.95 L1
misses/row of which 1.23 reach L3). The row frame costs **84.6 cycles/row** for the
same bytes in C++ — 97.5 loads/row, because a per-value walk cannot be
vectorized — and **307 cycles/row** when the client decodes it into Python rows,
which is where a delivered row's wall clock actually goes. The columnar path is
memory-shaped (a stream); the row frame is instruction-shaped (a walk).

What the cache counters do **not** support is the flattering version of the
story. Sweeping the payload across L1, L3 and 1.5× L3 sizes (44 KiB → 47 MiB,
a 1133× range) moves the columnar path from 11.4 to 17.2 cycles/row: it is a
sequential stream the prefetcher keeps fed, not a working set that fits in cache.
At 1M rows about 1.2 lines per row come from L3 or beyond, and it still
costs 17 cycles — the flatness is the result, not residency. The meter is
`just cache-stat`, the run is `docs/cache-stat.log`, and the analysis is the
cache section of [`simd/hypotheses.md`](../simd/hypotheses.md).

Arrow is zero-copy too, in the part of the path people usually mean: wrapping
existing buffers in arrays costs nothing, and the record-batch wrap is counted as
**0 copies** in this repository's own accounting. What it cannot avoid is the
aggregation: `pyarrow.compute` kernels materialize temporaries (3 copies in the
matrix) and the reader first reconstructs Arrow's array objects from the stream.

**Cost of aliasing:** a view is only valid while the writer isn't rewriting. This
repository solves that with an explicit protocol (one chunk in flight, ack after
the consumer is done), not with a copy — which is the entire trade in one line:
**Arrow buys safety and portability with copies; this design buys speed with a
protocol.** The protocol's cost is visible too: see 4.5, where the "tail" is the
protocol working as designed.

### 4.4 There is one language boundary, not three

The fair, same-output comparison is the `TUPLE READ` row: both stacks produce
Python rows. ADBC gets there by building an Arrow table and then calling
`to_pylist` — allocation, refcounting and the GIL for every value. That is
ADBC's public API, not Arrow's format, and the document would be dishonest
without saying so.

The columnar comparison removes the Python layer on both sides and the gap is
still 3.6× (8.25M against 2.28M rows/s). Two numbers explain it, both from this
repository's own pricer: `read_rows: 309 cycles/row over 400000 rows (decode
307.0, frame+desc+list 2.0)` — about 81 ns per row at 3.8 GHz for six columns,
i.e. ~13 ns per value, which is the cost of *allocating and filling one Python
object* in CPython (the object, its refcount and its tuple slot). So the tuple
path does not escape the language boundary — it pays the same per-object cost
everyone pays — but it pays it once, on a pointer it already holds, where the
rival pays it after two other layers (wire bytes → Arrow arrays → Python objects)
have each done a pass of their own. `frame+desc+list` at 2 cycles/row says there
is nothing left to squeeze around it: of the ~139 ns a delivered row costs at
query level, ~81 ns is the decode and the rest is the query and the server-side
frame build.

### 4.5 Shared memory instead of a pipe

This is the largest single factor at query level and the smallest code
difference. `AF_UNIX` `SOCK_SEQPACKET` is a good transport — the kernel copies
each message into the reader's buffer, wakes it, and the reader copies out.
Shared memory removes *the copy*, not the syscalls: the consumer maps the same
pages the producer wrote, so the bytes are never in the kernel, and the handshake
is a semaphore doorbell pair (publish, ack — the ring's own mechanism, see
`transport_bench.cpp`) plus the `seq` counter in the segment header. Both
transports pay for wakeups; only one pays the kernel for every byte.

Measured on identical bytes: **11.18 against 6.93 GB/s**, **3.6 against 32.8 µs
median**, **6.1 against 411 µs p99**, **1 copy against 2**, and the socket's cost
lands mostly on the consumer (**4.5 against 2.3 ms of CPU** — the copy plus the
wakeups to drain the kernel buffer). The cache counters are worth quoting here for
the opposite reason: they cannot see this advantage at all, because the socket's
copy happens in the kernel where user-space events do not reach. That blind spot
is itself the finding — user space sees shared memory touch the bytes twice and
the socket once, and the second touch is the copy.

The tail deserves its own sentence, because it is easy to misread as noise.
`p99 == max` in every transport cell, and the socket's median (32.8 µs) sits far
below its mean (41.1 µs): half the chunks arrive in 14–33 µs and the other half
wait ~410 µs. That is *one chunk in flight* working as designed — a chunk that
arrives while the consumer is busy waits for the consumer. It is the price of the
ack protocol from 4.3, and it is why the shared-memory number is not just "the
socket with a faster copy".

That price is now measured rather than asserted: letting the producer run two
chunks ahead takes the same bytes from **11.1 to 20.0 GB/s (+81%)**, at the cost of
a p50 that grows from 5 µs toward 10 µs — and with four or more chunks in flight
throughput falls back and the median grows roughly linearly with the window,
because what it then measures is queueing. So the protocol is not optimal for
throughput; it is optimal for *latency*, which is what the query path needs, since
it is not transport-bound. `transport_bench --in-flight N` is the experiment, and
the sweep is in [`simd/hypotheses.md`](../simd/hypotheses.md).

**Costs:** same host only; the segment itself is the interface, so its
permissions and lifetime are the security boundary (same user, single host, an
execution endpoint rather than a public surface — see `LIMITATIONS.md`); no
remote clients; segment creation and crash handling are the application's
problem. Arrow's answer to all four is a byte stream that anyone can read,
anywhere, forever.

### 4.6 The producer lives inside the database

The extension holds PostgreSQL's own binary values — `DateADT`, `Timestamp`,
`pg_uuid_t`, raw `bytea` — and ships them verbatim, tagged with a compact kind so
the client can wrap them instead of parsing digits and hex. There is no
conversion step at all, in either process.

ADBC is a client. It can only ask the server for wire bytes and then convert them
into Arrow arrays in the client's process, on the client's CPU. That is not a
defect; it is what "driver" means when you do not control the server. An
extension is not a driver — it is code running next to the executor, which is why
it can hand over the heap's representation and skip the conversion entirely.

**Cost:** a C extension in the server, per-major-version pinning of the OID
mapping, an install step, and a fixed vocabulary of kinds. None of that is
available to a library that must work against every PostgreSQL ever shipped.

---

## 5. Why Arrow does not do it this way

Not ignorance, and not (only) politics. Ordered from contract to economics:

### 5.1 The contract is the bytes, not the machine

Arrow's columnar format is defined as self-describing and language-independent:
a buffer layout plus metadata, with no pointers, no addresses, no assumptions
about who wrote it. Its stated purpose is data interchange — one writer, many
readers, possibly on different hardware, possibly years apart. A scheme where the
reader dereferences the *producer's* memory is outside that contract by
construction. It isn't a better format that they failed to write; it is a
different contract.

### 5.2 A reader that dereferences foreign memory is a security problem

Arrow readers parse untrusted streams. They validate metadata structure — lengths,
offsets, field counts — precisely because the input may be hostile, and they can
do so without knowing anything about the producer. A raw pointer into another
process cannot be validated from the bytes alone, cannot be bounded, and cannot
survive the producer exiting. This repository's reader *can* bound-check every
view because it knows the payload's layout exactly and owns both ends; that is a
privilege Arrow's readers do not have and should not want.

### 5.3 Immutability is a feature they sell

"The batch you were handed cannot change under you" is a guarantee users build
on. Arrow keeps it by owning its buffers. Zero-copy in Arrow means *sharing an
immutable buffer between libraries in one process* (the C data interface, which
is safe), not sharing a mutable ring between processes, which requires a
protocol — one every implementation would have to implement identically and the
spec would have to version. The IPC format chose framing over shared memory for
that reason: framing is portable, a ring is a runtime.

### 5.4 They tried it, and took it out

Arrow's **Plasma** store was exactly the missing piece: a shared-memory object
store for Arrow buffers, with the same idea this repository uses — the producer
publishes bytes into memory both sides map. It was deprecated in Arrow **10.0.0**
(2022) and removed in a later release; Ray's object store is its successor.

That is the strongest available evidence for the answer to "why don't they do
it": they did, at the level where it fits (a *runtime* that owns both ends), and
removed it from the level where it doesn't (a *format* that must outlive its
author). The idea is not unknown. It is deliberately placed elsewhere.

### 5.5 Their complexity budget is spent on breadth

Arrow maintains a specification with versioned compatibility, a dozen language
implementations, SIMD kernels, Parquet and Flight and Flight SQL, and type
completeness (nested types, decimals, dictionaries, time zones). Avoiding one
memcpy and one wakeup per 1 MiB of same-host traffic does not make that list,
because for most Arrow users the bottleneck is not the pipe — it is a distributed
filesystem, a network hop, or a Python object. This repository serves exactly one
case: one PostgreSQL server, one client, one host, one shape of data. Narrowness
is what lets it spend its entire budget on the six mechanisms above.

### 5.6 PostgreSQL makes it structural

An extension can put a producer *inside the server's address space*. A driver
connects to the server as a client. ADBC will never be able to hand over the
heap's tuples directly, because it is not in the heap's process, and adding an
extension to every PostgreSQL installation is not something a driver can assume.
The advantage here is therefore not really a faster format — it is a different
**topology**: the reader is inside the writer's memory. Arrow cannot adopt that
without either changing what it is (a portable interchange format) or shipping a
store, which it did, and then removed.

---

## 6. What this costs — the same six mechanisms, as trade-offs

| mechanism | what it buys | what it costs |
|---|---|---|
| write once into the shipping buffer | 1 copy, 2.5× encode | no reuse across frames (append-only arena), no compression, no in-place encryption |
| fixed layout, walked not parsed | no metadata parse, 28-byte header | version lock; unknown kinds refused; 13-kind vocabulary; reader must be compiled for the frame |
| decode as a view | free decode, cache-warm aggregates | lifetime protocol instead of a copy; producer must wait for the consumer's ack |
| one language boundary, not three | the per-value Python cost is paid once, on bytes already in hand | the client must be native code; Python-level conveniences cost extra |
| shared memory transport | 1.6× throughput, 9× median, 1 copy | same host only; the segment is the interface, so its permissions are the boundary; crash/lifetime handling is yours; not a public surface |
| producer inside the server | no conversion at all | extension install; per-major-version OID pinning; per-version testing |

The complete list of what is deliberately not supported — remote clients,
`numeric`, `jsonb`, `10M+` spread measurement, and the rest — is in
[`LIMITATIONS.md`](LIMITATIONS.md), which also has the FAQ of objections worth
answering in public.

---

## 7. When each is the right answer

| situation | use |
|---|---|
| one PostgreSQL server, one local client, read-heavy analytics | this repository |
| remote clients, or clients on other hosts | Arrow/ADBC — a shared segment cannot cross a network boundary |
| readers in several languages, or bytes stored for later | Arrow (or Parquet) — the metadata is the product |
| data must outlive the process that wrote it | Arrow/Parquet |
| you cannot install an extension in the server | ADBC, or any driver |
| mixed workloads where analytical reads are a small slice | a normal driver; the extension's benefit is concentrated in the read path |
| you need ecosystem tooling (pandas, Polars, DuckDB, Spark) | Arrow, because that ecosystem consumes its format natively |
| the fastest path for a *single* host and a *known* pair of processes | shared memory and a walkable frame, which is what this repository measures |

---

## 8. How to check all of it yourself

```bash
just codec-transport      # the 2x2 matrix: format and transport, isolated
just codec-charts         # the four figures from that log
just bench                # the query-level comparison against ADBC + Arrow
```

Then read, in this order: [`CODEC-TRANSPORT.md`](CODEC-TRANSPORT.md) for the
method and the "what the first run was actually measuring" section,
[`METHODOLOGY.md`](METHODOLOGY.md) for the claims-to-evidence table, and
[`LIMITATIONS.md`](LIMITATIONS.md) for what is not claimed.

The predictions that would falsify this document, in order of how easy they are
to test:

1. **Make the consumer slow** (add work per chunk). The socket's ~410 µs second
   mode should grow faster than shared memory's, because every socket chunk pays
   a kernel copy of its bytes into the reader's buffer and shared memory's pays
   only the doorbell.
2. **Change the chunk size.** Shared memory should stay flat; the socket should
   improve as chunks grow and degrade as they shrink.
3. **Rebuild the codec at `-O2`.** The *decode* on this side should get about 50%
   slower (measured: 2.67 ms against 1.76 ms) and Arrow's should not move.
4. **Restore the per-value `memcpy` in the encoder.** Encode on this side should
   go back to roughly 8.5 ms against Arrow's 7.8, i.e. the ordering of the format
   comparison flips on one line of code — which is the real lesson of §3: flags
   and copy granularity matter as much as formats when comparing them.
5. **Point both stacks at a remote server.** The transport advantage should
   vanish (it cannot exist across a network), leaving the format difference
   (2× encode, 1 copy against 6) as the surviving claim.
