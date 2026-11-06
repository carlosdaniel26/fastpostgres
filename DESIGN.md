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

### 1. `engine_eventfd.c` — a pollable doorbell

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

### 2. `worker.c` — a small pool of workers dispatching many rings

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

## What remains

- **Scheduling.** `pool_overflow_slot` picks the least-loaded worker, but a
  better policy would overflow onto a worker whose rings are currently *idle*
  (all eight eventfds quiet) and keep busy workers at one ring. The eventfd
  registry has the information; the prototype does not use it yet.
- **Error isolation.** A PG error on one ring aborts the worker's transaction
  and therefore every stream it serves. Per-stream subtransactions (savepoints)
  would contain it; the prototype accepts the coupling.
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
