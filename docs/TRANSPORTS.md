# The two transports

A read is delivered in two halves. The **payload** is a batch of rows, published
in a data segment under `/dev/shm` and read in place by the client; that half is
the same for every transport and is not what these pages compare. The other half
is the **doorbell**: telling the other process that a request arrived or that a
batch is ready. That half has two implementations, selectable per connection:

```bash
FASTPOSTGRES_TRANSPORT=shm      # default: the ring's own semaphores
FASTPOSTGRES_TRANSPORT=regipc   # a cross-process call through the regipc module
```

Both are first-class: the same program reads the same rows either way, and the
ring records which engine made it (`FPRING04`, field `transport`), so a segment
cannot be driven by the wrong doorbell.

## shm — the ring's own semaphores

The client writes the request into the control segment and `sem_post`s the
worker; the worker wakes from `sem_timedwait`, publishes the batch, and posts the
response semaphore. Under the hood that is one futex wake and one scheduler
handoff per batch, in each direction. It needs no privileges, no module and no
kernel version, and an idle worker sleeps: it wakes on a doorbell or, every
100 ms, to notice that its client is gone.

## regipc — a cross-process call

The technique is a loadable kernel module (x86-64 only) that turns one `ioctl`
into a *call*: the client puts nine 64-bit values in x0..x8, the module reads them
out of the saved `pt_regs` and writes them, in ring 0, into a page the worker
pinned; the call's return value travels back in RAX. The worker is never woken,
never scheduled, never context-switched. Its own repository — the technique this
tree vendors, with its ABI and its module — is the place the protocol is
specified.

What that means for the ring: the client's request descriptor (`op`, `seq`,
`batch`, `data_bytes`) travels in the carriers instead of being written and
signalled; ring 0 writes it into the worker's leaf, which sits at offset 4096 of
the control segment (`FastpostgresRingRegipc`). Both sides then **poll** the
counters in that segment — the client spins on `resp_seq`, the worker on
`leaf.calls` — for a bounded budget (100 µs), and fall back to the semaphores when
the peer is idle, so an idle worker still sleeps and the protocol still works when
the spin loses. The client posts the request semaphore after the call as the wake
hint for exactly that fallback.

The costs are stated where they are paid:

- **two syscalls per request** (the call and the wake hint) — the call removes the
  *scheduler handoff*, not the syscall;
- **CPU while it spins.** Under load neither side's budget expires, so both
  processes spin for the whole run: ~2 cores at full tilt for 1.76 M hops/s. The
  semaphore engine hands the core back and uses ~1.
- **a module on the host.** Without `/dev/regipc` the client raises rather than
  falling back (`_client.Ring` reports the errno; the DBAPI's ring creation
  already degrades a *connection* to libpq, as it does for any ring failure);
- **a slower stop from SQL.** `fastpostgres_ring_stop()` posts only the
  semaphore, and no leaf call carries STOP, so a regipc worker notices it at its
  100 ms wait timeout. `Ring_stop()` on the client does carry STOP as a call.
- **the trust boundary moves.** A caller that can open `/dev/regipc` can make the
  module write into a registered callee's pinned page. The device's mode is the
  boundary — the module is loaded with `mode=0666` for an unprivileged benchmark,
  and `0600` is the module's own default. It is an experiment, not a product
  surface; its repository documents the remaining invariants (one caller per
  callee, one call in flight, no nesting, dead contexts reaped on the next
  registration).

## The measurement

[`just internals transport`](BENCHMARKS.md) measures the doorbell alone, with the
product's engine code compiled into a two-process harness
(`benchmarks/internals/transport/peer.c`): one hop is the client's request
descriptor and the worker's answer, with no data segment created and no payload
moved. Both cells run the same loop and the same ring, so only the ops table
differs. Record: [`transport-shm-vs-regipc.log`](../benchmarks/results/transport-shm-vs-regipc.log).

| | hops/s | p50 | p99 | µs of CPU per hop | at 100k rows/batch |
|---|---|---|---|---|---|
| shm | 161.8 k | 6.38 µs | 9.84 µs | 6.22 | 16.2 G rows/s |
| regipc | 1.76 M | 0.521 µs | 0.551 µs | 1.14 | 175 G rows/s |

**10.8x the hops and 12.2x less wall time per hop, for 5.5x less CPU per hop** —
with the caveat the table cannot show and the chart does: the regipc cell spends
its CPU *spinning* (both processes, ~2 cores for the duration), while the shm
cell spends it *sleeping*. At 100k rows per batch that is 175 G rows/s of
*directory* work, not rows delivered — the payload is the data segment's job, and
[`fastpostgres-vs-psycopg.log`](../benchmarks/results/fastpostgres-vs-psycopg.log)
is where delivered rows are measured.

What the numbers say, then, is narrow and useful: **the doorbell is no longer what
a batch costs.** At 6.4 µs per handoff, a batch takes about as long to announce as
a 100k-row `memcpy`-shaped publish takes to write; at 0.52 µs it does not. The
question left for the end-to-end comparison is how much of a real read was the
doorbell, which is what the workloads measure.

## Reproducing

```bash
make -C benchmarks/internals/transport          # the harness binary (no root, no PostgreSQL)
just internals transport 100000,1000000 3       # host, needs the module and /dev/regipc
```

The module is vendored for reproducibility
([`extension/transport/regipc/module/`](../extension/transport/regipc/module/))
and builds against the running kernel's headers; the benchmark uses the one the
machine has loaded (ABI version 2), and the harness refuses a different one
instead of measuring a different technique. The harness is host-only by
construction: a container cannot load a module, which is why this comparison does
not run in the image or in CI. The ABI itself is
[`extension/include/fastpostgres/regipc.h`](../extension/include/fastpostgres/regipc.h).
