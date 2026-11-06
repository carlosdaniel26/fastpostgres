# integration/backlog — the nine backlog workstreams in one tree

Base: `merge/read-path` (`3e3665a`), which already contains the 7 transport
teams plus `team/os` and `team/unix`. Branch: `integration/backlog`.

Each workstream was applied with `git cherry-pick <its base>..ws/<name>`, so the
`merge/transport` reverts did not run and the OS/unix code stayed. Order below
is the order applied; **db-encode went first** because it changes the wire.

| order | branch | commits added | result |
|---|---|---|---|
| 1 | `ws/db-encode` | `3ccf585`, `de97a90`, `16f04ea`, `66d9607`, `0c361aa` | adapted: `extension/Makefile` union |
| 2 | `ws/arch-readpath` | `6b8427b` | clean code; `DESIGN.md` union |
| 3 | `ws/object-reuse` | `199c8bb` | clean code; `DESIGN.md` union |
| 4 | `ws/reduce` | `025d813` | `aggregate.cpp` + `DESIGN.md` unions |
| 5 | `ws/memfd-default` | `d94ed7a` | clean code; `DESIGN.md` union |
| 6 | `ws/sd-3k` | `c54bada` | clean code; `DESIGN.md` union |
| 7 | `ws/net-n8` | `a9cebc8` | row carry reconciled into `carry.c`; `DESIGN.md` union |
| 8 | `ws/os-scm` | `a9145c2` | `connection.cpp` method-table union |
| 9 | `ws/orm-semantics` | `37746db` | clean code; `DESIGN.md` union |

**None dropped.** Every branch merged and the three required suites are green
(see Tests).

## Conflicts and how they were resolved

### `DESIGN.md` — one section per workstream

Every branch appended its own design section at the end of `DESIGN.md`, so each
cherry-pick conflicted there and only there (except the two code conflicts
below). Resolution was mechanical and content-preserving: keep the integration
tree's existing sections, then append that branch's own section from
`git show <commit>:DESIGN.md`. No branch's prose was discarded. `net-n8`
contributed the whole eventfd design document (it was absent from the base), and
`orm-semantics`' section was promoted from `##` to `#` so it reads as its own
top-level section after the docs it originally followed.

### `extension/Makefile` — union of the two object lists

`db-encode` adds `src/carry.o`; `merge/read-path` adds `src/columnar_store.o`.
Both are kept, so the receiver's carry queue and the OS bulk-load store build.

### `python/src/fastpostgres/native/aggregate.cpp` — two comments + `walk_text`

`merge/read-path` (memfd store) reworded the `reduce_payload` comment; `reduce`
adds `walk_text()` (text/bytea MIN/MAX) between the float walker and
`reduce_payload`. Resolution keeps both comments merged and `walk_text` intact.
The payload is decoded through `fastpostgres_reader_init/next`, so the reduce
reads v3 frames without a second parser.

### `python/src/fastpostgres/native/connection.cpp` — method table

`arch-readpath` adds `exec_lazy`; `os-scm` adds `backend_pid` (for the
`SO_PEERCRED` check). The table keeps both entries and the eager `exec` doc
string.

### `net-n8`'s row carry × `db-encode`'s `carry.c`

This is the one semantic overlap. `net-n8` kept a per-portal row carry inside
`rows.c`, keyed by the cursor **name** so a savepoint abort (which destroys the
portal) does not lose the carried row. `db-encode` had already moved the carry
to `carry.c` as a per-portal queue (it is used by both the row frame and the
streaming columnar receiver).

Resolution: **`carry.c` is the single implementation**, extended with the
`net-n8` property — each queue stores the stream name, `fastpostgres_carry_for`
reattaches by name when a recreated portal has the same name, and
`fastpostgres_carry_drop_name()` lets `fastpostgres_stream_forget()` drop the
queue after a savepoint rolled the portal away. The duplicate `RowCarry`
implementation in `rows.c`/`rows.h` was removed, and `stream.c` now calls
`fastpostgres_carry_drop_name()`. The per-stream savepoints stay in `worker.c`.

### What was adapted for wire v3

The v3 frame (`columns_off` table) is defined once in
`extension/include/fastpostgres/fastpostgres_shm.h`; every reader goes through
`fastpostgres_reader_init()` / `fastpostgres_reader_next()`, which follow the
table and no longer walk `data_len`. That covers, and was verified by compiling
and running the combined tree:

- the worker writer (`columnar.c`, both the streaming receiver and the build
  path used by the OS store);
- the native client's full-frame, block (`decode_rows`), lazy `ResultSet`, lease
  and tuple paths (`segment.cpp`, `codec.cpp`);
- the OS columnar store attach and its mapped reduce (`segment.cpp`,
  `aggregate.cpp`) — the OS store is still a separate, opt-in path, while the
  ring owns the v3 frame;
- the compilers deformer (`bench/internals/codec_transport/fpg_codec.cpp`);
- the client-side reduce (`aggregate.cpp` / `fp_segment_payload`).

No reader was left on a v2 `data_len` walk, so no branch had to be dropped for
v3. The `FPROWS` frame is unchanged, and the `FPREDUCE` v2 frame and the worker
`REDUCE` op (including in the eventfd pool) are untouched by v3.

## Validation

Build (under the benchmark lock):

    flock /tmp/fpq-bench.lock -c \
      'docker build -q --target dev -t fpq-integration -f docker/Dockerfile .'

Tests, each in a fresh container, all green:

| suite | result |
|---|---|
| default (`shm`, sealed memfd store) | `make installcheck`: All 1 passed; `Ran 106 tests ... OK (skipped=2)` |
| `FASTPOSTGRES_TRANSPORT=eventfd` | `make installcheck`: All 1 passed; `Ran 106 tests ... OK` |
| `FASTPOSTGRES_MEMFD=1` | `make installcheck`: All 1 passed; `Ran 106 tests ... OK (skipped=2)` |

The two skips are the opt-in eventfd tests (skipped unless the transport is
selected), not failures. The memfd run is fully green: the two `/dev/shm`
assertions that failed in the earlier `merge/read-path` record now assert the
sealed-memfd behavior, and the v3 frame works over both the memfd default and
the `/dev/shm` fallback.

The eventfd run also covers `REDUCE` in the pool (the whole aggregate suite runs
under `FASTPOSTGRES_TRANSPORT=eventfd`), so the `db-encode` × `net-n8` overlap is
exercised, not assumed.

## Benchmarks

Full commands, raw ENV blocks and both images' output are in
`bench/results/integration-backlog-ab.log`.

| cell (rows/s) | baseline | integration | delta |
|---|---:|---:|---:|
| 100K DBAPI | 3.67M | **11.58M** | +215.5% |
| 100K stream() | 9.20M | **14.29M** | +55.3% |
| 100K core | 3.66M | **11.23M** | +206.8% |
| 100K 4 processes | 8.86M | **33.30M** | +275.8% |
| 1M DBAPI | 4.00M | **13.87M** | +246.8% |
| 1M stream() | 9.23M | **13.30M** | +44.1% |
| 1M core | 3.98M | **13.84M** | +247.7% |
| 1M 4 processes | 9.57M | **49.78M** | +420.2% |

`stream()` vs Arrow: **630%** at 100K (14.29M / 2.27M) and **575%** at 1M
(13.30M / 2.31M), up from 397%/409% on the baseline.

**One honest combined read multiple:** default DBAPI at 1M, `4.00M -> 13.87M
rows/s` = **3.47x** (100K: `3.67M -> 11.58M`, 3.16x). `stream()` itself is
1.44x/1.55x over baseline and 5.75x/6.30x Arrow. These are separate cells, not
factors multiplied together. The baseline is the protocol baseline (`main`), so
the 3.47x is cumulative over both merge phases; `merge/read-path` alone measured
2.29x at 1M in its own earlier run (`team-merge-ab.log`), which is where this
branch started.

## Known limits in the combined tree

- `FASTPOSTGRES_TRANSPORT=eventfd` and `FASTPOSTGRES_MEMFD=1` together still use
  the named `/dev/shm` fallback for pool workers; each is opt-in and tested
  separately (inherited from `merge/read-path`).
- A live leased result still holds a ring, and lease eviction above two batches
  is serial — the `ws/arch-readpath` trade, unchanged.
- The OS columnar store and the unix ORM materializer remain separate
  consumption paths, not alternatives inside `dbapi.Cursor.aggregate()`.
- The wire is now v3 only. A client built from a v2 header refuses v3 frames and
  vice versa; there is no negotiation or v2 emitter yet (documented by
  `ws/db-encode`).
