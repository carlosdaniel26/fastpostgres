# nativepostgress

A native PostgreSQL path that moves query results as **columnar binary over
POSIX shared memory** instead of text over a socket — for both reads and writes.

The point is to skip text serialization/parsing and per-row object
construction on both ends, so results land in JS as typed arrays and rows are
loaded back with the COPY fast path.

```
READ:   SQL → SPI → columnar binary → /dev/shm → typed arrays (JS)   ~2x faster than libpq
WRITE:  typed arrays (JS) → columnar binary → /dev/shm → multi_insert ~2x faster than COPY
```

## Layout

```
pg-extension/         the PostgreSQL server extension (C)
  nativepostgress.c   all the SQL-callable functions
  np_shm.h            the shared-memory wire format (also in v8-extension/)
  nativepostgress.control / nativepostgress--1.0.sql / Makefile   build + install
  test.sql            correctness tests
  bench.c             read + write benchmark (libpq client)

v8-extension/         the client runtime (embeds V8)
  include/v8ext/
    runtime.h         Runtime: decodes a columnar segment into JS typed arrays
    np_shm.h          the wire format
  src/runtime.cpp
  tools/
    shm_read.cpp      standalone decoder (no V8): reads a segment, prints columns
    shm_demo.cpp      loads a segment into JS typed arrays
  CMakeLists.txt
  scripts/get-v8.sh   downloads a PREBUILT V8 (no compilation)

scripts/
  test-pg-docker.sh   build + run the correctness tests in postgres:18 (Docker)
  bench-pg-docker.sh  run the read/write benchmark in postgres:18 (Docker)
```

## The wire format (`np_shm.h`)

A `/dev/shm` segment is:

```
NpShmHeader (64 bytes)   magic "NPQSHMEM", payload_size, capacity, seq
NPCOLUMN payload          magic "NPCOLUMN", ncols, nrows, then per column:
                          name, kind (1=bool 2=int32 3=int64 4=float8 5=text),
                          pgtype, null bitmap, then data:
                            numeric -> flat little-endian values
                            text    -> int32 offsets + raw bytes
```

`seq` is bumped after each write so a reader can detect a concurrent write. The
two copies of `np_shm.h` (server + client) must stay in sync.

## Extension functions

| function | what it does |
|---|---|
| `native_query(text) → bytea` | run a query, return the columnar result |
| `native_query_shm(text, text) → text` | run a query, write the columnar result to `/dev/shm/<seg>` |
| `native_write_shm(text, text) → bigint` | read a columnar segment, bulk-insert into a table |
| `native_query_header(text) → text` | column names/types (debug) |
| `native_shm_read(text) → bytea` / `native_shm_info(text) → text` | inspect a segment |

## Build & test

Everything needs only **Docker** (no local Postgres or V8 build):

```sh
# correctness tests (query + shared memory round-trip + write round-trip)
scripts/test-pg-docker.sh

# benchmark: normal libpq/COPY vs the native columnar path
scripts/bench-pg-docker.sh 1000000      # rows
```

To run the client side (`shm-read` / `shm-demo`) on the host, build it once:

```sh
v8-extension/scripts/get-v8.sh                                   # fetch prebuilt V8
cmake -S v8-extension -B v8-extension/build \
      -DV8_ROOT="$PWD/v8-extension/third_party/v8-install"
cmake --build v8-extension/build
```

The test script writes `/dev/shm/np_test` (via `--ipc=host`) and then runs
`v8-extension/build/shm-demo np_test` to decode it into JS typed arrays.

## Supported types

`int2`/`int4`/`int8`, `float4`/`float8`, `numeric`, `bool`, `text`/`varchar`/
`char`, and anything else falls back to its text output. Nulls are tracked per
column via a bitmap.
