# Adoption

The fourth measurement class, and the only one whose protocol is not this
repository's: a published benchmark suite, vendored verbatim at a pinned commit,
run on its own terms, with fastpostgres added to its roster as one more cell.
Every other class here asks the questions the project chose; this one asks
someone else's, which is the point - it can only report what their queries find.

The first (and so far only) suite is **MagicStack/pgbench**, the PostgreSQL
client-driver toolbench whose numbers stand behind asyncpg's documentation: MIT,
seven query files, and a protocol of its own (per-cell process, warmup, a timed
window at a fixed concurrency, a latency histogram in hundredths of a
millisecond, JSON on stdout).

- The suite: [`magicstack-pgbench/`](magicstack-pgbench/), byte for byte as its
  authors wrote it. `PIN` holds the commit it came from; `LICENSE` is theirs.
- Our cell: [`players/pgbench_fastpostgres`](players/pgbench_fastpostgres) - the
  fastpostgres DBAPI in the suite's Python-runner contract, handing their own
  `runner()` the rows so the warmup, the timed loop and the JSON stay theirs.
- The roster: [`run.py`](run.py) appends `fastpostgres-dbapi` to the suite's
  module-level list of implementations and then calls its own `main()`, because
  the suite is vendored rather than edited.
- The record: `just adoption magicstack-pgbench`, committed as
  [`../results/adoption-magicstack-pgbench.log`](../results/adoption-magicstack-pgbench.log);
  the write-up, with what the cells found and where the ring loses on their
  terms, is [`../ADOPTION.md`](../ADOPTION.md).

## How to run it

```bash
just adoption magicstack-pgbench                 # the Python roster + our cell
just adoption magicstack-pgbench --quick         # warmup 1s, duration 3s
just adoption magicstack-pgbench --duration=30 python-asyncpg fastpostgres-dbapi
```

Cells are named by the suite: `python-aiopg`, `python-asyncpg`,
`python-psycopg3-async`, and `fastpostgres-dbapi` for ours. The suite's Go and
Node cells are in the vendored tree too; they need toolchains the dev image does
not carry, so no class here names them.

## What the adapter does, and does not do

- **The roster is extended, not the suite.** One symlink beside the vendored
  runner (`pgbench_fastpostgres`, in the suite's own idiom for its runners) and
  one appended list entry. No vendored file is edited.
- **The suite's own pins are not installed.** Its 2023 `requirements.txt` names
  driver versions that do not build on the image's Python 3.13; the image
  installs the roster at versions that do, and the venv its Python wrapper wants
  is created with `--system-site-packages` so that is one install. The record's
  own block prints what ran, and `_python/requirements.txt` stays in the tree for
  reference.
- **One compatibility shim.**
  [`players/sitecustomize.py`](players/sitecustomize.py) restores
  `asyncio.get_event_loop()`'s loop creation for the suite's 2023 runner (uvloop
  0.21+ raises instead), so its async cells still run on uvloop. Scoped to this
  class by the job script, and it changes nothing the suite measures.
- **COPY is not supported, and says so.** The client's writes go through libpq
  and there is no DBAPI copy call there, so the cell exits 3 - the suite's own
  "not supported by the runner" contract - and the record shows the cell missing
  rather than a number that hides it.
- **The check this class can make is the row count.** The suite's protocol does
  not digest values; the record's `rows` column is what the classes here call
  "the same output".
