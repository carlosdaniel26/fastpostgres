# Records: every committed measurement

The measurements live in [`bench/results/`](results/), next to
the harnesses that produce them, not beside the prose. Each file is one run end to
end: the harness's environment block (`cpu`, `postgres`, `python`, the versions,
`git:`), every player's own report, and the checks its class makes (the digest, the
`served_by_shm` evidence, the cache counters).

| record | what it is | the argument it backs |
|---|---|---|
| `fastpostgres-vs-arrow.log`, `-mixed`, `-small` | the read against ADBC + Arrow, at three sizes and two shapes | [`WHY-IT-WINS.md`](WHY-IT-WINS.md), [`LIMITATIONS.md`](../docs/LIMITATIONS.md) |
| `fastpostgres-vs-arrow-memory.log` | the same reads with the ring's buffer fixed: what each side held | [`ARROW-MEMORY.md`](ARROW-MEMORY.md) |
| `adoption-magicstack-pgbench.log` | a published suite's own seven queries and protocol, with our cell in its roster: MagicStack/pgbench, vendored at a pin | [`ADOPTION.md`](ADOPTION.md) |

## Housing rules

- A record is produced by redirecting its harness to a file *outside* the tree and
  copying it in, so the `git:` stamp is taken while the record file is still
  untouched and the stamp is clean:

  ```
  ```

- One file per run family; a re-run replaces it, and the `git:` stamp says which
  commit produced the numbers.
- The charts in `docs/assets/` are rendered from these files by
  `just charts <target>`; no chart is drawn from a number typed by hand.
- No record is edited by hand. A number that is not in one of these files is not
  quoted.
