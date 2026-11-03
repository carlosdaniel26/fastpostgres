# Bulk reads and the wait policy

Scope: what a different wait policy could recover in a bulk read — the proposal
on the table being to stop the client polling and wake the worker only every N
batches. Everything below is arithmetic over the committed records; no run was
made for this note.

Sources: the doorbell-only record (`bench/results/transport-shm-vs-regipc.log`,
no data segment, no payload), the end-to-end record
(`bench/results/transport-e2e.log`, real reads, one process per cell), the Arrow
read (`bench/results/arrow-transport.log`), and the code that produced them
(`extension/transport/engine_regipc.c`,
`extension/include/fastpostgres/ring.h`,
`python/src/fastpostgres/native/ring.cpp`,
`python/src/fastpostgres/dbapi.py`). The end-to-end record measures the client
process only (`transport-e2e.log:27-28`).

## The handoffs a 1M-row scan takes

Thirteen per read. The scan cell reports `13 ring requests` for 1,000,000 rows
(`transport-e2e.log:6194`, `:6196`); the per-read formula is the player's — OPEN,
one NEXT per payload batch, the NEXT that returns 0, CLOSE
(`bench/internals/transport/e2e_player.py:268-271`) — so 10 payload batches + 3
scaffolding. The raw cell agrees: `"ring_requests":13`, `"payload_batches":10`,
`leaf_calls 26` (13 timed + 13 warm-up for 1 timed + 1 warm-up read,
`transport-e2e.log:3115`, `:3112`).

Per-hop prices (doorbell only, 1M hops, median of 3):

| engine | p50 per hop | CPU per hop (both processes) |
|---|---|---|
| shm | 6.362 µs | 6.24 µs (`transport-shm-vs-regipc.log:31`) |
| regipc | 0.521 µs | 1.14 µs (`transport-shm-vs-regipc.log:36`) |

CPU per hop is user+sys over both processes (`transport-shm-vs-regipc.log:16`,
`:26`); the regipc cell spends it spinning, the shm cell sleeping (`:17-18`).

| 13 handoffs, priced at | wall | share of the read |
|---|---|---|
| shm's 6.362 µs | 82.7 µs | 0.021% of 403.675 ms (`transport-e2e.log:6193`) |
| regipc's 0.521 µs | 6.8 µs | 0.0017% of 409.074 ms (`transport-e2e.log:6195`) |
| the difference, 5.841 µs | 75.9 µs | 0.019% |

The third row is the ceiling on a cheaper doorbell: 13 × 5.841 µs = 75.9 µs of a
404 ms read. Only 10 of the 13 hops carry a batch; those are 63.6 µs, 0.016%.
One batch is ~40 ms of the read (403.675 ms / 10), so the doorbell is 0.016% of
a batch's work.

## The same bound for CPU

Per row: 13 × 6.24 µs / 1,000,000 rows = 0.081 ns/row — credited generously, as
the 6.24 µs is both processes and the record's own split is 50/50
(`transport-shm-vs-regipc.log:39`: producer_cpu_s 3.103, consumer_cpu_s 3.132 at
count 1000000), so the client's own half is ~0.041 ns/row.

| engine | client CPU/row, measured (`transport-e2e.log:6240`) | doorbell bound | share |
|---|---|---|---|
| shm | 0.2345 µs (shown as 0.234 at `:6194`) | 0.081 ns/row (both sides) | 0.035% |
| regipc | 0.2368 µs (shown as 0.237 at `:6196`) | 0.015 ns/row | 0.0063% |
| difference | 0.0023 µs | 0.066 ns/row | 0.028% |

For scale: the two cells' measured CPU/row differ by 1.0%
(`transport-e2e.log:6240`), and their per-rep
spans (0.2327–0.2371 and 0.2330–0.2410 µs/row, `transport-e2e.log:40`, `:49`,
`:58`, `:3115`, `:3124`, `:3133`) are 1.9% and 3.4% of the median. The whole
doorbell is ~50x smaller than the smaller of those. The record's own CPU/row
difference, 2.3 ns/row, is ~35x the whole doorbell bound (0.066 ns/row): that
difference is not the doorbell either.

The poll budget by itself is 13 waits × 100 µs (`REGIPC_SPIN_US`,
`extension/transport/engine_regipc.c:76`), ≤2.6 ms over both sides of a 404 ms
read, 0.64%; the client's half, ≤1.3 ms, is 0.55% of the client's ~234 ms of
scan CPU. It is a budget, not a cost — the client's spin ends the moment
`resp_seq` arrives (`engine_regipc.c:317-325`) — and the measured CPU does not
show even the budget: 0.2345 vs 0.2368 µs/row (`transport-e2e.log:6240`).

## The contrast: where the doorbell is a visible share

lookup: 4 handoffs per statement (2,000 requests / 500 reads,
`transport-e2e.log:6211-6214`; `leaf_calls 2080` = 2,000 timed + 20 warm-up × 4).
At the shm price that is 25.4 µs of a 99 µs statement (26%); at the regipc
price, 2.1 µs of 64 µs (3%). The record shows the doorbell's effect exactly
there: +55.4% statements/s for regipc (`:6215`), bought with client CPU — 30.346
→ 66.668 µs/row (`:6212`, `:6214`), because the regipc client polls while the
worker has nothing to do.

paged: the same 4 handoffs per page, 25.4 µs of 275 µs (9%) at the shm price,
+34.5% for regipc (`transport-e2e.log:6229-6233`).

Why that is not bulk: hops per row are 4 in the lookup and 1.3e-5 in the scan —
300,000x fewer. The polling price is a fixed 100 µs window per handoff, and it
is visible only where the worker is idle: in the scan the worker is producing a
100k-row batch between hops and the client is decoding one (the client posts the
next NEXT before decoding the current, `dbapi.py:595-611`), so waits are behind
work. Thirteen 100 µs windows in 404 ms cannot surface.

## Two structural facts

- **The read path is one request at a time.** `ring.h:7-8` (one OPEN/NEXT/CLOSE
  request at a time), `ring.cpp:201-205` (one request in flight, so the ring's
  single op/arg/nrows slot is enough), `ring.cpp:390-392` (the caller must wait
  before posting the next), `dbapi.py:589` (one request in flight; a segment is
  reused only once decoded). The two data segments let the client post the *next*
  batch before decoding the current (`dbapi.py:602-610`), but never a third. A
  policy that waits for N requests to accumulate has no second request to wait
  for until the protocol gains a queue.
- **`req_seq`/`resp_seq` sit in the same cache line.** `ring.h:119-120`; from the
  fields above them (`ring.h:111-120`) `req_seq` is at offset 32 and `resp_seq` at
  40 of the control block, both in the first 64-byte line. The client stores
  `req_seq` (`ring.cpp:245-246`), the worker stores `resp_seq`
  (`engine_shm.c:176`, `engine_regipc.c:485`), so every handoff writes the line
  the other side polls. At 13 handoffs per read this cannot be a lever; the
  coefficient would only matter at a hop count that does not fit a 1M-row read
  at these batches.

## The ceiling, plainly

- Wall: ≤0.021% of the shm read and ≤0.0017% of the regipc read; the price
  difference between engines — where a cheaper doorbell could act at all —
  0.019%.
- Client CPU: ≤0.035% of the measured 0.2345 µs/row (both sides of the hop
  credited to the client; ≤0.017% client-side only).
- The run's own spread is larger than the ceiling by 1-2 orders of magnitude:
  the connected records report per-cell rep spreads of ±1.0% to ±5.8% for the
  fastpostgres cells (`arrow-transport.log:32`, `:51`), cell-to-cell ratios of
  0.94x-1.04x (`arrow-transport.log:106-110`), and the scan's own rep spans are
  1.13% and 1.16% of the median (`transport-e2e.log:6193-6195`, values at
  `:6240`). The scan's measured engine gap, 1.3-1.4% (`:6197-6198`), is ~65x
  larger than the whole doorbell difference the arithmetic above allows — it is
  noise, not the doorbell.

**There is nothing measurable to recover in bulk.** Delete the polling and a 1M
row read does not move: the doorbell is 0.02% of it, and the proposal's subject
— the poll window — is 0.6% of the client CPU in the worst case, of which the
record shows none. The doorbell is also not decoration in bulk: a batch takes
~40 ms to deliver and the worker's poll window is 100 µs, so by the next request
the worker is asleep and the `sem_post` hint (`engine_regipc.c:289-295`) is what
wakes it; suppressing the wake trades latency for a shorter spin, it does not
delete the handoff.

(`docs/TRANSPORTS.md:120` reports the same product as 0.002%; that figure is the
regipc price's — 13 × 0.521 µs — not the shm price it is stated with.)

## What would have to change

No design, just the conditions under which the doorbell becomes a lever in bulk:

- the ring must hold more than one request (a queue): today one request at a
  time is the protocol (`ring.h:111-130`, `ring.cpp:390-392`), so a policy that
  waits for N requests to accumulate has one batch to wait for;
- hops × price must exceed spread × wall: at the shm price, 1% of a 404 ms read
  is ~635 hops and 6% is ~3,800 hops — a ~1.6k-row or ~260-row batch, against
  today's 100,000 (`transport-e2e.log:6185`);
- or the per-hop price must rise: a cheaper doorbell returns only the *difference*
  between prices — 5.84 µs/hop here — so the doorbell is a lever only where the
  price is already large against the batch (contention, a scheduler round trip
  back on the critical path);
- pipelining alone does not change the ceiling: the read would still be 13 hops
  per 1M rows at 6.362 µs, 0.021% of the read. The client's wait is already
  overlapped with decode (`dbapi.py:602-610`); the lever is the hop count, not
  the wait.

## Not answerable from these records

- The worker's CPU, and its spin, in the scan: not measured
  (`transport-e2e.log:27-28`). The only bound is the budget, ≤1.3 ms of worker
  poll per read. As of this writing the ring is gaining a per-side tally of
  handoffs, posts, semaphore fallbacks and polling microseconds
  (`FastpostgresRingTally`, `extension/include/fastpostgres/ring.h:105-109`);
  no record built on it exists yet, so nothing in this note is measured from it.
- Per-hop time *inside* the end-to-end cells: not measured. The hop prices above
  are the isolated doorbell record's; the end-to-end cells do not decompose
  their wall time.
- The scan cell is one timed read per rep (`transport-e2e.log:6185`), so its
  spread is across 3 reps, not across reads.
