# FastPostgres documentation

The [README](README.md) covers what the project is, the measured results, the
quick start, the API usage and the configuration. This file holds the rest: how
the repository is laid out, where to read more, what the project does not do,
and the formal bits.

## Project layout

| path | what it is |
|---|---|
| `extension/transport/` | the doorbell engine (`engine_shm.cpp`) |
| `extension/` | the PostgreSQL extension: the ring worker, the request/response ring, the two writers, and `fastpostgres_shm.h` - the wire format |
| `python/` | the client: a C++ extension module (the row/columnar decode and the aggregate reduce), the PEP 249 DBAPI, the asyncio layer (`aio.py`), the SQLAlchemy dialect, and the tests |
| `bench/` | the claim's tree: the measurement classes (`rivals/` - one directory per rival - `internals/`), the arguments (`WHY-IT-WINS.md`, `ARROW-MEMORY.md`, `CODEC-TRANSPORT.md`), the notebook, the dispatchers, the chart scripts, the image jobs and the committed records in `results/` |
| `docker/` | the Dockerfile and entrypoint every harness runs in |
| `examples/` | the driver-swap demo application |
| `docs/` | the manual: the limitations and the figures every page embeds |
| `bench/NOTES.md` | the decode's lab notebook: hypotheses, meters and their results |

## Documentation index

| I want to... | go here |
|---|---|
| run or read about the benchmarks | [`bench/README.md`](bench/README.md) |
| see every committed measurement | [`bench/RESULTS.md`](bench/RESULTS.md) |
| understand the wire format and the two frames | [`extension/include/fastpostgres/fastpostgres_shm.h`](extension/include/fastpostgres/fastpostgres_shm.h) |
| reproduce a measurement, or read the statistics | [`bench/METHODOLOGY.md`](bench/METHODOLOGY.md) |
| know what is not covered, and what happens there | [`docs/LIMITATIONS.md`](docs/LIMITATIONS.md) |
| understand why it is faster than Arrow, mechanism by mechanism | [`bench/WHY-IT-WINS.md`](bench/WHY-IT-WINS.md) |
| separate format cost from transport cost | [`bench/CODEC-TRANSPORT.md`](bench/CODEC-TRANSPORT.md) |
| see what a published third-party suite found | [`bench/ADOPTION.md`](bench/ADOPTION.md) |
| contribute, or report a vulnerability | [`CONTRIBUTING.md`](CONTRIBUTING.md), [`SECURITY.md`](SECURITY.md) |

## Known limitations

The full list, with what the benchmark does not cover and the objections to
expect, is in [`docs/LIMITATIONS.md`](docs/LIMITATIONS.md). The ones that shape
how this can be used:

- **Research prototype.** Not for production, multi-user, or untrusted networks.
- **The segment is the trust boundary.** `/dev/shm/<seg>` is an SQL-execution endpoint: the client creates it `0600` and the worker widens it to `0666` (`extension/src/shm.cpp`) so a client running as another user can map it. Treat the segment itself as the permission.
- **Ring reads run in the worker's session.** They miss the calling connection's uncommitted writes; the driver is autocommit.
- **`numeric` widens to `float8` on the wire.** Types whose storage is not the wire bytes (numeric, jsonb, arrays, enums) take the columnar frame, which is correct but not the zero-copy row path.
- **One stream per ring.** The pool is sized on first use - one stream per ~1.2 hardware threads, divided by the SMT width (6 on this machine), clamped by the server's free `max_worker_processes` - and when it is exhausted the read falls back to libpq.
- **The asyncio layer is a thread-per-connection facade.** Each `AsyncConnection` owns a tier thread that runs the blocking ring waits, so the loop keeps running; concurrency is bounded by the ring pool, and there is no `PQcancel` yet, so cancelling a task lets its current call finish before the cancellation lands. A batch's views are valid until *that stream's* next pull.
- **`aggregate()` walks numbers, not rows.** `Cursor.aggregate` reduces sum/min/max/avg/count in C++ with the GIL released — parallel on any interpreter, no numpy — and the worker can do it on the tuples themselves (`FASTPOSTGRES_OP_REDUCE`, the default). Integer sums accumulate in 128 bits and come back as Python ints, so they never wrap: `sum(bigint)` matches the server's exact numeric. `avg` expands to sum + count and divides with PostgreSQL's numeric scale. `min`/`max` also cover text (the worker compares with the column's collation), date/time and numeric; `count` works on every kind. The client-side fallback is still there (`FASTPOSTGRES_AGG_PUSHDOWN=0`); it gets exact integer sums too, but numeric is float8 there because that is what the columnar wire carries.
- **What parallelizes is the consumption, not the row decode.** `Cursor.aggregate` (a C++ reduce) and numpy reductions over `stream()` scale with one ring per thread; building Python tuples is the DB-API compatibility path, holds the GIL, and is not benchmarked — it scales on a free-threaded interpreter, where `just nogil` builds the client against CPython 3.13t and runs the suite and the scaling cell (`bench/NOTES.md`).

## Contributing

Issues and pull requests are welcome. `just test` runs the client test suite, and
any measurement offered as evidence should follow
[`bench/METHODOLOGY.md`](bench/METHODOLOGY.md) and be committed as a log next to
the others in `bench/results/`, with its `git:` stamp.

## License

MIT - see [`LICENSE`](LICENSE).
