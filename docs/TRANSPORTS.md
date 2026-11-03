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
  processes spin for the whole run: ~2 cores at full tilt for 1.74 M hops/s. The
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
(`bench/internals/transport/peer.c`): one hop is the client's request
descriptor and the worker's answer, with no data segment created and no payload
moved. Both cells run the same loop and the same ring, so only the ops table
differs. Record: [`transport-shm-vs-regipc.log`](../bench/results/transport-shm-vs-regipc.log).

| | hops/s | p50 | p99 | µs of CPU per hop | at 100k rows/batch |
|---|---|---|---|---|---|
| shm | 163.6 k | 6.362 µs | 10.019 µs | 6.24 | 16.36 G rows/s |
| regipc | 1.74 M | 0.521 µs | 0.612 µs | 1.14 | 174.32 G rows/s |

**10.6x the hops and 12.2x less wall time per hop, for 5.5x less CPU per hop** —
with the caveat the table cannot show and the chart does: the regipc cell spends
its CPU *spinning* (both processes, ~2 cores for the duration), while the shm
cell spends it *sleeping*. At 100k rows per batch that is 174.3 G rows/s of
*directory* work, not rows delivered — the payload is the data segment's job, and
[`fastpostgres-vs-psycopg.log`](../bench/results/fastpostgres-vs-psycopg.log)
is where delivered rows are measured.

What the numbers say, then, is narrow and useful: **the doorbell is no longer what
a batch costs.** At 6.4 µs per handoff, a batch takes about as long to announce as
a 100k-row `memcpy`-shaped publish takes to write; at 0.52 µs it does not. The
question left is how much of a *real* read was the doorbell, which is what the
end-to-end record below measures.

### End to end: a real read, delivered in rows

[`just internals transport-e2e`](BENCHMARKS.md) runs both engines through the
whole path in the dev image — PostgreSQL executes the statement, the ring's
background worker publishes batches into the data segment, the client decodes
them — one fresh process per (engine, scenario, rep), with the transport passed
in `FASTPOSTGRES_TRANSPORT`. The regipc cell works in a container because the
device node travels: `docker run --device /dev/regipc`. Record:
`bench/results/transport-e2e.log`.

Three scenarios, each on both engines, median of 3 processes, one client and one
ring per process, 64 MiB data segments, a 100,000-row batch ask:

| scenario | engine | delivered | p50 per read | client µs of CPU per row |
|---|---|---|---|---|
| scan, 1M rows in 10 batches of 100k | shm | 2.48M rows/s | 403.675 ms | 0.234 |
|  | regipc | 2.44M rows/s | 409.074 ms | 0.237 |
| lookup, 500 primary-key reads of one row | shm | 9.62k stmts/s | 0.099 ms | 30.346 |
|  | regipc | 14.95k stmts/s | 0.064 ms | 66.668 |
| paged, 500 filtered 100-row pages | shm | 3.43k stmts/s | 0.275 ms | 0.755 |
|  | regipc | 4.61k stmts/s | 0.202 ms | 2.060 |

**At the scan's batch size the doorbell does not change the delivered rate at
all: 2.49 M rows/s either way (−0.4%, inside the run-to-run spread). A 1M-row
read is 13 handoffs; 13 × 6 µs is 0.002% of a 400 ms read.** What it does change
is the small read, where the handoff *is* a measurable fraction of a statement:
**+68% statements per second on the primary-key lookups** (p50 0.110 ms → 0.064 ms)
and +20% on the 100-row pages, and the record shows what that costs — the client
process's CPU per statement rises from 37.5 µs to 69.4 µs, because the regipc
client polls the ring's counters while the shm client sleeps on a futex. The
trade is the isolated table's, one level up: latency bought with CPU, and only
where the doorbell is a visible share of the work.

Each cell's record carries the proof: `conn.shm_reads` advanced exactly once per
timed read (no libpq fallback), the ring's own header says `transport` 1 or 2 and
the harness reads that header a second time from the container's `/dev/shm`, the
digest (count and column sums) is identical on both engines, and the regipc cells
carry the module's `leaf.calls` (13 and 2,000 timed calls) — ring 0 ran. What the
numbers do not carry: the worker's CPU (the record measures the client process
only, and a regipc worker spins too), and any concurrency.

### The Arrow read, both engines

The records above answer what a handoff costs, but on the workload suite's
schema: 100k-row batches, one bulk scan, one client. The Arrow comparison
(`just rivals adbc`, catalogued in [`BENCHMARKS.md`](BENCHMARKS.md)) is the other
delivered-rows measurement — a six-column, 1M/10M-row read with each stack's own
players — and its committed record was measured on one engine, so it cannot say
what the transport costs that read. This record adds the engine as a second
factor: per size, one fresh process per engine runs
`bench/rivals/sqlalchemy_bench.py` (`FASTPOSTGRES_TRANSPORT` is read at
import and a ring's engine is fixed at creation, so no process can serve both),
and the ADBC players ride inside those runs and are recorded once — they do not
depend on the engine.

```bash
just rivals arrow-transport            # 1M and 10M, both engines
just rivals arrow-transport --quick    # 100k, for a smoke run
just charts arrow-transport            # docs/assets/arrow-transport.png
```

The regipc cell needs the module and the device, so the harness runs the dev
image with `docker run --device /dev/regipc`, as the end-to-end record does; a
container that cannot see the node fails the run with the remedy, never silently
dropping an engine. Every fastpostgres record carries its cell's proof: the
control rings the cell's processes left in the container's `/dev/shm` all name
the engine that was asked for, their requests are answered, and the harness's own
probe read was served by the ring, with the same digest (count + column sums) in
every cell. Record: `bench/results/arrow-transport.log`.

**At this read's scale the engine does not move the delivered rate.**

| read, 1M / 10M rows | shm | regipc | regipc/shm |
|---|---|---|---|
| DBAPI (typed tuples) | 6.23M / 6.64M rows/s | 5.87M / 6.54M | 0.94x / 0.98x |
| stream()+sum (zero-copy) | 8.28M / 8.25M | 7.93M / 8.18M | 0.96x / 0.99x |
| core interface | 6.05M / 6.45M | 5.99M / 6.66M | 0.99x / 1.03x |
| SQLAlchemy CORE | 1.32M / 1.25M | 1.37M / 1.24M | 1.04x / 0.99x |
| 4 processes | 10.62M / 6.03M | 11.71M / 6.97M | 1.10x / 1.16x |
| ADBC + Arrow, arrow+sum (once per size) | 2.33M / 2.32M | — | — |

Every single-process ratio is inside ±6%, the run's own spread (the 1M tuple
read spreads ±1.0% over its reps on shm and ±5.8% on regipc); the only path that
leans one way at both sizes is the 4-process read, the noisiest one. That is the
end-to-end scan's result on a different schema, batch size and set of players: a
1M-row read is ten 100k-row batches, and ten handoffs at either engine's price
are not a measurable share of a read that takes 120-170 ms.

**The Arrow family reproduces the committed record**: `arrow+sum` 2.33M/2.32M
rows/s here against 2.28M/2.24M in
[`fastpostgres-vs-arrow.log`](../bench/results/fastpostgres-vs-arrow.log)
(+2%/+4%), `table+to_pylist` 1.29M/1.24M against its 1.27M/1.27M, and
`stream()+sum` 8.28M/8.25M against its 8.23M/8.42M. The row-decode paths run
below the committed record's numbers (DBAPI 6.2-6.6M against 7.27-7.33M, the
4-process read 6.0-10.6M against 8.9-13.4M); the client's own report puts the
decode at ~345 cycles/row at 1M against ~315 in the committed run, so the gap is
the machine's state (background load, the governor), not the code — and it is the
same on both engines, which is what this record is about.

## Reproducing

```bash
make -C bench/internals/transport          # the harness binary (no root, no PostgreSQL)
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
