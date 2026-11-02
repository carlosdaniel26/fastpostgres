# FastPostgres documentation

The [README](README.md) covers what the project is, the measured results, the
quick start, the API usage and the configuration. This file holds the rest: how
the repository is laid out, where to read more, what the project does not do,
and the formal bits.

## Project layout

| path | what it is |
|---|---|
| `extension/transport/` | the two doorbell engines (`engine_shm.c`, `engine_regipc.c`), the vendored regipc ABI and module |
| `extension/` | the PostgreSQL extension: the ring worker, the request/response ring, the two writers, and `fastpostgres_shm.h` - the wire format |
| `python/` | the client: a C++ extension module, the PEP 249 DBAPI, the SQLAlchemy dialect, and the tests |
| `benchmarks/` | the harnesses (`workloads/`, `rivals/`, `internals/`), the dispatchers, the chart scripts, the image jobs and the committed records in `results/` |
| `docker/` | the Dockerfile and entrypoint every harness runs in |
| `examples/` | the driver-swap demo application |
| `docs/` | the benchmark catalog, methodology, limitations, the comparisons, and every committed log |
| `docs/DECODE-NOTES.md` | the decode's lab notebook: hypotheses, meters and their results |

## Documentation index

| I want to... | go here |
|---|---|
| run or read about the benchmarks | [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) |
| see every committed measurement | [`docs/RESULTS.md`](docs/RESULTS.md) |
| choose or change the transport | [`docs/TRANSPORTS.md`](docs/TRANSPORTS.md) |
| understand the wire format and the two frames | [`extension/include/fastpostgres/fastpostgres_shm.h`](extension/include/fastpostgres/fastpostgres_shm.h) |
| reproduce a measurement, or read the statistics | [`docs/METHODOLOGY.md`](docs/METHODOLOGY.md) |
| know what is not covered, and what happens there | [`docs/LIMITATIONS.md`](docs/LIMITATIONS.md) |
| understand why it is faster than Arrow, mechanism by mechanism | [`docs/WHY-IT-WINS.md`](docs/WHY-IT-WINS.md) |
| compare against a result cache | [`docs/PG-CACHE.md`](docs/PG-CACHE.md) |
| separate format cost from transport cost | [`docs/CODEC-TRANSPORT.md`](docs/CODEC-TRANSPORT.md) |
| contribute, or report a vulnerability | [`CONTRIBUTING.md`](CONTRIBUTING.md), [`SECURITY.md`](SECURITY.md) |
| know what changed | [`CHANGELOG.md`](CHANGELOG.md) |

## Known limitations

The full list, with what the benchmark does not cover and the objections to
expect, is in [`docs/LIMITATIONS.md`](docs/LIMITATIONS.md). The ones that shape
how this can be used:

- **Research prototype.** Not for production, multi-user, or untrusted networks.
- **Linux, same host.** The transport is POSIX shared memory; there is no network path to benchmark or to use.
- **The segment is the trust boundary.** `/dev/shm/<seg>` is an SQL-execution endpoint: the client creates it `0600` and the worker widens it to `0666` (`extension/src/shm.c`) so a client running as another user can map it. Treat the segment itself as the permission.
- **Ring reads run in the worker's session.** They miss the calling connection's uncommitted writes; the driver is autocommit.
- **`numeric` widens to `float8` on the wire.** Types whose storage is not the wire bytes (numeric, jsonb, arrays, enums) take the columnar frame, which is correct but not the zero-copy row path.
- **One stream per ring.** The pool is sized on first use - one stream per ~1.2 hardware threads, divided by the SMT width (6 on this machine), clamped by the server's free `max_worker_processes` - and when it is exhausted the read falls back to libpq.
- **The object arena is never returned to the OS.** A tuple read builds Python objects into it; a 10M-row read is what the benchmark's one-rep rule is about.

## Contributing

Issues and pull requests are welcome. `just test` runs the client test suite, and
any measurement offered as evidence should follow
[`docs/METHODOLOGY.md`](docs/METHODOLOGY.md) and be committed as a log next to
the others in `docs/`, with its `git:` stamp.

## License

MIT - see [`LICENSE`](LICENSE).
