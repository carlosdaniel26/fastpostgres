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
(`bench/results/codec-transport.log` for the format and transport isolation matrix,
`bench/results/fastpostgres-vs-arrow.log` for the query-level comparison), measured on one
machine with the versions in the footers: PostgreSQL 18, pyarrow 26.0.0, ADBC
1.13.0. Where a claim is *not* measured, the text says so. The list of things
this design gives up in exchange is in [`LIMITATIONS.md`](../docs/LIMITATIONS.md).

---

## 1. The numbers, so the rest of the document has an anchor

**Query level, the same columnar output on both sides** (the harness's
`ZERO-COPY READ` rows: each stack's columnar form, held and not consumed):

| rows | this repo | ADBC + Arrow | difference |
|---|---|---|---|
| 100K | 8.27M rows/s | 2.25M rows/s | **+268%** |
| 1M | 7.95M rows/s | 2.29M rows/s | **+247%** |
| 10M | 8.70M rows/s | 2.32M rows/s | **+275%** |

**Format only**, 1M rows, transport held out entirely, one process
(the `fpcolumn` vs `arrow-ipc` cells):

| | this repo | Arrow IPC |
|---|---|---|
| payload | 47.2 MiB | 45.7 MiB |
| encode | **3.3 ms** (15.0 GB/s) | 7.7 ms (6.2 GB/s) |
| decode + six aggregates | **1.8 ms** | 1.8 ms |
| encode + decode | **5.1 ms** | 9.5 ms |
| copies of the payload | **1** | 6 |
| peak RSS | **101 MB** (2.1× the payload) | 399 MB (8.7×) |

**Transport only**, the *same payload bytes*, codec out of the loop:

| | shared memory | UNIX socket |
|---|---|---|
| throughput | **11.04 / 10.54 GB/s** | 7.14 / 6.51 GB/s |
| median latency | **3.6 / 4.5 µs** | 32.0 / 36.5 µs |
| p99 latency | **6.5 / 8.6 µs** | 367.2 / 383.2 µs |
| CPU, producer + consumer | **4.6 / 4.6 ms** | 7.0 / 7.4 ms |
| copies | **1** | 2 |

Read together: at the query level the gap is 3.6–3.8×, at the format level it is
1.9×, and at the transport level it is 1.5–1.6× on throughput and 8–9× on median
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
sink, 3 for the aggregate kernels' temporaries. Peak RSS **8.7× the payload**,
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
| encode | 3.3 ms | 7.7 ms | **this is the real format gap**: 2.3× |
| decode | 1.8 ms | 1.8 ms | a scan; both are memory-bound, level |

The decode is *not* where the formats separate, and saying otherwise would be
dishonest: both sides read ~25 MB of numeric columns and offset tables and are
within 1% of each other. The separation is in encode (writing the bytes out)
and, at query level, in the transport (moving them).

That 2.2× encode gap is worth dwelling on, because the project found it the hard
way. The first version of this matrix had Arrow *winning* the format cell, and
investigation showed the harness, not the formats, was responsible:

- the C++ unit was built at `-O2` while pyarrow ships SIMD kernels — at `-O3`
  the frame's decode alone went from 2.67 ms to 1.76 ms;
- the encoder copied each text **value** with its own `memcpy` — about 2M small
  copies per run — where a serializer holding contiguous buffers copies each
  **column** twice (offsets, then bytes).

Both are fixed. Arrow's numbers barely moved (7.3 → 7.5 ms). That asymmetry is
the evidence: the gap was on this side of the harness, and once removed, the
remaining 2.3× is the format's own structure — three copies of the same bytes
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

**Cost:** the frame cannot be reused across queries,
cannot be compressed, and cannot be encrypted in place. **Evidence:** 1 copy
against 2 in the encode path; 15.0 GB/s against 6.2 GB/s.

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
**Evidence:** the decode numbers (1.8 vs 1.9 ms) — modest, because both sides
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
a 1133× range) moves the columnar path from 10.5 to 16.7 cycles/row: it is a
sequential stream the prefetcher keeps fed, not a working set that fits in cache.
At 1M rows about 1.2 lines per row come from L3 or beyond, and it still
costs 17 cycles — the flatness is the result, not residency. The meter is
`just cache-stat`, the run is `bench/results/cache-stat.log`, and the analysis is the
cache section of [`NOTES.md`](NOTES.md).

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

The comparison removes the Python layer on both sides — both stacks land the same
columnar form — and the gap is still 3.5× (7.95M against 2.29M rows/s), so what
it prices is the transport and the frame, not the object model. The DB-API's
tuple read is the compatibility interface, not the headline: it pays CPython's
per-object cost (`read_rows: 309 cycles/row over 400000 rows`, about 81 ns per
row at 3.8 GHz for six columns, i.e. ~13 ns per value for the object, its
refcount and its tuple slot), and an application that wants Python rows pays
that on either stack.

Where the boundary does separate the two designs is *how many times* the bytes
change hands before the object exists. The frame's row is decoded from the
pointer the reader already holds; a driver that converts on the way out pays the
wire bytes → Arrow arrays → Python objects chain, each layer a pass of its own.
`frame+desc+list` at 2 cycles/row says there is nothing left to squeeze around
the decode: of the ~139 ns a delivered row costs at query level, ~81 ns is the
object and the rest is the query and the server-side frame build.

### 4.5 Shared memory instead of a pipe

This is the largest single factor at query level and the smallest code
difference. `AF_UNIX` `SOCK_SEQPACKET` is a good transport — the kernel copies
each message into the reader's buffer, wakes it, and the reader copies out.
Shared memory removes *the copy*, not the syscalls: the consumer maps the same
pages the producer wrote, so the bytes are never in the kernel, and the handshake
is a semaphore doorbell pair (publish, ack — the ring's own mechanism, see
`transport_bench.cpp`) plus the `seq` counter in the segment header. Both
transports pay for wakeups; only one pays the kernel for every byte.

Measured on identical bytes: **11.04 against 7.14 GB/s**, **3.6 against 32.0 µs
median**, **6.5 against 367 µs p99**, **1 copy against 2**, and the socket's cost
lands mostly on the consumer (**4.2 against 2.4 ms of CPU** — the copy plus the
wakeups to drain the kernel buffer). The cache counters are worth quoting here for
the opposite reason: they cannot see this advantage at all, because the socket's
copy happens in the kernel where user-space events do not reach. That blind spot
is itself the finding — user space sees shared memory touch the bytes twice and
the socket once, and the second touch is the copy.

The tail deserves its own sentence, because it is easy to misread as noise.
`p99 == max` in every transport cell, and the socket's median (32 µs) sits far
below its mean (46 µs): half the chunks arrive in 13–32 µs and the other half
wait hundreds of microseconds. That is *one chunk in flight* working as designed — a chunk that
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
the sweep is in [`NOTES.md`](NOTES.md).

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
| write once into the shipping buffer | 1 copy, 2.3× encode | no reuse across frames, no compression, no in-place encryption |
| fixed layout, walked not parsed | no metadata parse, 28-byte header | version lock; unknown kinds refused; 13-kind vocabulary; reader must be compiled for the frame |
| decode as a view | free decode, cache-warm aggregates | lifetime protocol instead of a copy; producer must wait for the consumer's ack |
| one language boundary, not three | the per-value Python cost is paid once, on bytes already in hand | the client must be native code; Python-level conveniences cost extra |
| shared memory transport | 1.5–1.6× throughput, 8–9× median, 1 copy | same host only; the segment is the interface, so its permissions are the boundary; crash/lifetime handling is yours; not a public surface |
| producer inside the server | no conversion at all | extension install; per-major-version OID pinning; per-version testing |

The complete list of what is deliberately not supported — remote clients,
`numeric`, `jsonb`, `10M+` spread measurement, and the rest — is in
[`LIMITATIONS.md`](../docs/LIMITATIONS.md), which also has the FAQ of objections worth
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
[`LIMITATIONS.md`](../docs/LIMITATIONS.md) for what is not claimed.

The predictions that would falsify this document, in order of how easy they are
to test:

1. **Make the consumer slow** (add work per chunk). The socket's ~370 µs second
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
