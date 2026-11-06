# merge/read-path — nine teams, one measured tree

Base: `main@7e3d2e6` ("bench: re-run the ADBC records for the read-only timed
region"). Branch: `merge/read-path`. Every branch below was merged with
`git merge --no-ff`, in the requested order. None was dropped.

| order | branch | commit | result |
|---|---|---|---|
| 1 | `team/databases` | `ffb8f35` | clean |
| 2 | `team/architecture` | `5162737` | `DESIGN.md` add/add resolved (union) |
| 3 | `team/compilers` | `bdb2a2e` | `DESIGN.md` only |
| 4 | `team/programming` | `91af94b` | `DESIGN.md` only; `dbapi.py` auto-merged |
| 5 | `team/system-design` | `c434150` | `DESIGN.md` only; `shm`/`worker` auto-merged |
| 6 | `team/formats` | `445db35` | real conflicts: `shm.c`, `shm.h`, `columnar.c`, `rows.c`, `worker.c` |
| 7 | `team/networking` | `b1d9897` | real conflicts: `Makefile`, `ring.h`, `worker.c`, `dbapi.py` |
| 8 | `team/unix` | `d086e1b` | clean (`DESIGN.md` union) |
| 9 | `team/os` | `5273982` | real conflicts: `Makefile`, native `aggregate.cpp`, `segment.cpp/.hpp` |

The merge commits are `ac7a938`, `332d0b1`, `6d3ee6e`, `56044dd`, `f227be0`,
`8dc2927`, `1cdee52`, `b6dddff`, `9ae0166`; `4902866` closes the one brace the
`aggregate.cpp` union left behind. The records commit follows.

## Overlap resolutions

### formats × system-design — one segment cache, not two

Formats' `shm.c` replaces the data-segment backend with a per-worker,
path-keyed mapping table over a sealed memfd (and a `/dev/shm` fallback). That
is a superset of system-design's `map_cached()`: the mapping is still created
once per segment and reused for the worker's lifetime, but the memfd descriptor
can be handed to the client once instead of widening a named file.

Resolution: **formats' backend is the one data-segment implementation**.
`fastpostgres_shm_map_rw(path, size, &mapped)` and
`fastpostgres_shm_worker_begin/end()` replace `map_cached()` / `cache_fini()`.
The system-design cache survives as the cached `worker_segs[]` entries in
`shm.c` (the `/dev/shm` fallback path), so the default path still does no
per-batch `shm_open`/`mmap`. There is one mapping per segment, not two.
`reduce.c` (from programming) was adapted to the three-argument map call and
no longer `munmap`s the shared mapping after each batch.

The stream-open cache flush from system-design was removed: a memfd is sealed
and handed over once, so tearing its mapping down at a stream boundary would
invalidate the descriptor the client already holds. The table is flushed when
the worker exits.

### programming × networking — default shm, opt-in eventfd

Networking's eventfd engine stays opt-in behind
`FASTPOSTGRES_TRANSPORT=eventfd`; the client default is `"shm"`. The default
one-ring-one-worker path is the original semaphore engine and is what
`benchmarks.sh` and `tests.sh` exercise by default.

`worker.c` kept both: the programming `REDUCE` op on the ring worker, and the
networking pool worker. The pool's `pool_handle()` did not know `REDUCE`; it now
does, with the same stream-name-is-segment convention, so the aggregate
pushdown also works when the eventfd engine is selected. The pool still uses
the `/dev/shm` segment fallback (the memfd handoff is per ring worker).

### architecture × programming — lazy ResultSet + aggregate pushdown

`dbapi.py` auto-merged. The lazy `ResultSet` is the default `execute()` /
`fetchall()` result; `Cursor.aggregate()` still drives the worker reduce and
returns the same scalars. `read_reduce()` (the worker's `FPREDUCE` frame) and
`reduce()` (the client-side walk) both remain registered.

### databases × os — separate columnar layers

No shared file: `columnar.c` is the H8-A in-place frame builder; the OS
`columnar_store.c` is the separate bulk-load/memfd store. Both are built.

## Drops

None. All nine branches are in the tree and the full test suite passes on the
default path.

## Known limits in the combined tree

- `FASTPOSTGRES_MEMFD=1` is opt-in. Two tests inspect `/dev/shm/<seg>` directly
  (`test_the_data_segment_capacity_is_the_clients_ask`,
  `test_parallel.test_threads_each_get_their_own_partition`); under memfd those
  files do not exist by design. That run is 68 passing, 2 failing, 1 skipped,
  not a regression: formats' own record documents the same two assertions. The
  default and eventfd runs are fully green.
- `eventfd` + `memfd` together fall back to named `/dev/shm` segments for pool
  workers. Each is opt-in and tested separately; the combination is not a
  supported fast path yet.
- The OS columnar store and the unix ORM materializer are separate consumption
  paths, not alternatives inside `dbapi.Cursor.aggregate()`.

## Test evidence

    flock /tmp/fpq-bench.lock -c 'docker run --rm --shm-size=3g fpq-merged tests.sh'
    -> Ran 71 tests ... OK (skipped=1; eventfd opt-in test)

    ... -e FASTPOSTGRES_TRANSPORT=eventfd ... tests.sh
    -> Ran 71 tests ... OK

    ... -e FASTPOSTGRES_MEMFD=1 ... tests.sh
    -> Ran 71 tests ... FAILED (failures=1, errors=1, skipped=1)
       The two /dev/shm assertions fail; all functional tests pass.

`make installcheck` passed in every run. The measured build is image
`fpq-merged` — `sha256:b359704e50f452aa336fdccdade4a2a3c9d5d41086a0b7df84ed055fa730a88e`
— built from the tree at `4902866`.

See `bench/results/team-merge-ab.log` for the commands, raw ENV blocks,
baseline/merged rows/s, the AB deltas, and the two fast-path harnesses.
