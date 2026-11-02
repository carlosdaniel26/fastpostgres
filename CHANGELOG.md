# Changelog

All notable changes to this project. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[semantic versioning](https://semver.org/): the extension and the Python client
are released together.

## [Unreleased]

## [1.0.0] - 2026-11-01

### Added

- The transport: a background worker publishes each read's batches into
  `/dev/shm`, the client maps the segment and reads it in place. Two frames -
  `FPROWS` (the executor's own tuples, one `memcpy` per row) and `FPCOLUMN` (the
  zero-copy columnar frame) - defined once in `extension/include/fastpostgres/`.
- The Python client: a PEP 249 DBAPI, `stream()` for columnar batches, the
  SQLAlchemy dialect (`postgresql+fastpostgres`), and a native module with type
  stubs.
- The benchmark taxonomy: `just bench` (realistic workloads over a commerce
  schema, against psycopg 3), `just rivals` (ADBC + Arrow, and PgCache at its
  defaults and under a memory budget), `just internals` (the decode meters, the
  codec × transport matrix, PMU counters), and `just charts` for every log.
- `docs/BENCHMARKS.md` (the catalog) and `docs/RESULTS.md` (the index of every
  committed record under `benchmarks/results/`).

### Changed

- The transfer segment's capacity is the client's ask
  (`FASTPOSTGRES_SEGMENT_BYTES`, 64 KiB to 1 GiB), carried in the ring, instead
  of a compile-time 1 GiB; the row path carries a row that does not fit into the
  next batch rather than dropping it.
- The tree is one directory per artifact: `extension/`, `python/`,
  `benchmarks/`, `docker/`, `docs/`, `examples/`, with generated records and
  charts out of the prose.

### Fixed

- A capacity-limited row batch no longer truncates the stream: the row the
  segment could not take is carried into the next batch, and the portal's
  "short fetch means end-of-data" inference is undone when the shortfall was the
  segment's.
- The logo's cut-out has no light pixels along the silhouette (closed-form alpha
  matting over the original artwork).

[Unreleased]: https://github.com/carlosdaniel26/fastpostgres/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/carlosdaniel26/fastpostgres/releases/tag/v1.0.0
