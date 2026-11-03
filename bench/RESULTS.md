# Records: every committed measurement

The measurements live in [`bench/results/`](results/), next to
the harnesses that produce them, not beside the prose. Each file is one run end to
end: the harness's environment block (`cpu`, `postgres`, `python`, the versions,
`git:`), every player's own report, and the checks its class makes (the digest, the
`served_by_shm` evidence, the cache counters).

| record | what it is | the argument it backs |
|---|---|---|
| `fastpostgres-vs-psycopg.log` | the workload suite: seven scenarios over the commerce schema, against psycopg 3 | [`README.md`](README.md), the README's workload table |
| `fastpostgres-vs-arrow.log`, `-mixed`, `-small` | the read against ADBC + Arrow, at three sizes and two shapes | [`WHY-IT-WINS.md`](WHY-IT-WINS.md), [`LIMITATIONS.md`](../docs/LIMITATIONS.md) |
| `arrow-transport.log` | the same read with the ring's engine as a second factor: fastpostgres over shm and over regipc, ADBC measured once, one process per (engine, size) | [`TRANSPORTS.md`](../docs/TRANSPORTS.md#the-arrow-read-both-engines), [`README.md`](README.md) |
| `fastpostgres-vs-arrow-memory.log` | the same reads with the ring's buffer fixed: what each side held | [`ARROW-MEMORY.md`](ARROW-MEMORY.md) |
| `fastpostgres-vs-pgcache.log` | against a CDC-maintained result cache, each side at its defaults | [`PG-CACHE.md`](PG-CACHE.md) |
| `fastpostgres-vs-pgcache-memory.log` | the same comparison under one memory budget on both sides | [`PG-CACHE.md#the-same-memory-budget-on-both-sides`](PG-CACHE.md#the-same-memory-budget-on-both-sides) |
| `codec-transport.log` | the codec x transport matrix: format cost and transport cost, separated | [`CODEC-TRANSPORT.md`](CODEC-TRANSPORT.md) |
| `cache-stat.log` | the host PMU's counters: decodes per row, transports per KiB | [`NOTES.md`](NOTES.md) |
| `transport-shm-vs-regipc.log` | the ring's two doorbells, one hop at a time, on the host | [`TRANSPORTS.md`](../docs/TRANSPORTS.md) |
| `ab/*.log` | the before/after pairs the `improve-*` charts are drawn from | [`NOTES.md`](NOTES.md), [`CODEC-TRANSPORT.md`](CODEC-TRANSPORT.md) |

## Housing rules

- A record is produced by redirecting its harness to a file *outside* the tree and
  copying it in, so the `git:` stamp is taken while the record file is still
  untouched and the stamp is clean:

  ```
  just bench > /tmp/workloads.log && cp /tmp/workloads.log bench/results/fastpostgres-vs-psycopg.log
  ```

- One file per run family; a re-run replaces it, and the `git:` stamp says which
  commit produced the numbers.
- The charts in `docs/assets/` are rendered from these files by
  `just charts <target>`; no chart is drawn from a number typed by hand.
- No record is edited by hand. A number that is not in one of these files is not
  quoted.
