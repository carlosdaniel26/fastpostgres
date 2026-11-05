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
