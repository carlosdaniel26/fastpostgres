# Worker segment-mapping cache

The ring worker publishes every batch into the same one or two data segments
(`<stream>`, or `<stream>_a` / `<stream>_b` when the client pipelines). Before
this change each batch ran `shm_open` + `fchmod` + `fstat` (+ `ftruncate`) +
`mmap` + `madvise(MADV_HUGEPAGE)` and then `munmap` at the end of the publish:
a fixed ~25–31 µs per fetch, visible as the worker's 35 → 44 ns/row at 3k-row
batches. The client's read-only mapping was already kept; this is the writer's
half.

## What is cached

`extension/src/shm.c` keeps up to eight `(path, capacity) → mapping` entries in
a process-static array, and `fastpostgres_shm_map_cached()` is now what
`rows.c` and `columnar.c` publish through. A hit is a `strcmp` + a size check;
a miss creates the mapping once, exactly as before. Eight slots cover the two
pipelined segments plus stale names, so a capacity change does not evict the
mapping the next batch is about to use; eviction is least-recently-used.
The worker is single-threaded, so the array needs no lock.

## Lifetime and teardown

- **Stream boundary.** `FASTPOSTGRES_OP_OPEN` flushes the cache before the new
  stream starts: a new stream may name new segments, or the same names over
  fresh files, and a mapping must not outlive the segment it describes.
- **Worker exit.** The normal exit path and both early failure paths after the
  ring is mapped call `fastpostgres_shm_cache_fini()`, so the data mappings are
  unmapped with the worker.
- **Capacity change.** `capacity` is part of the key; the same name at a
  different size gets a fresh mapping rather than an undersized one.
- **LRU.** A ninth distinct segment unmaps the least recently used one.

No mapping is left holding a deleted inode: the flush at OPEN covers segment
replacement between streams, and the worker owns the segments it publishes into
for the life of a stream.

## Measured

`bench/results/team-system-design-ab.log`, three interleaved runs per side, a
real table in the benchmark's row shape, one synchronous stream. At 3k rows per
fetch the worker's own CPU drops **97.92 → 87.62 ns/row (-10.5%)**, i.e.
**-30.9 µs per fetch**; its wall turn drops 106.61 → 97.97 ns/row (-8.1%), the
rest being the wakeup that wall time includes. At 100k rows per fetch the same
cost is 0.25 ns/row and the measurement is flat (44.60 → 43.01, inside the run
spread), as is the official 100k benchmark within its ±10% run-to-run noise.

## What remains

- The mapping is no longer per-batch, but the worker's *other* per-fetch fixed
  cost is unchanged: cursor/SPI setup and the doorbell wakeup dominate the
  remaining ~70 ns/row at 3k (`worker_cpu` 87.6 against ~43 at 100k). A smaller
  batch than 3k pays that even more; the map cache is one constant removed, not
  the per-fetch floor.
- `memfd` would let the worker own the segment and avoid `/dev/shm` naming, but
  the client maps the segment by name and an fd cannot cross the ring's
  `arg[]`; it needs a protocol change and is not worth it for this cost.
- `MADV_HUGEPAGE` is now paid once per segment instead of once per batch, which
  is the right place for it. Pre-faulting (`MAP_POPULATE` or a first touch) was
  not attempted: the worker writes every page it maps anyway, so it would move
  the fault, not remove it.
- The client's RO map cache is unchanged; the win here is entirely worker-side.
