# Codec + transport isolation experiment — shared contract

Two experiments, four cells, one runner. Nothing here talks to PostgreSQL: the
question is what the **format** costs and what the **transport** costs, measured
outside the server so the two factors do not hide behind each other.

## Factors

| factor | levels |
|---|---|
| format | `fpcolumn` (this repo's columnar frame, `extension/fastpostgres_shm.h`) vs `arrow-ipc` (Apache Arrow IPC stream via pyarrow) |
| transport | `shm` (POSIX shared memory + a sequence counter — the mechanism the ring uses) vs `uds` (`AF_UNIX` `SOCK_SEQPACKET`, one message per chunk) |

- **Control (held constant):** dataset, checksum, chunking, one producer and one
  consumer process, one measurement at a time.
- **Experiment 1 (format):** encode → bytes → decode → checksum, entirely in one
  process, no transport. Isolates serialization/deserialization cost.
- **Experiment 2 (transport):** the *same payload bytes* (a file emitted by
  Experiment 1's codecs) move producer → consumer over each transport. The
  codec is not in the loop, so only the transport varies.
- The row frame (`fprows`) is an optional extra row: it is not the same output
  shape as Arrow, so it cannot be the headline format comparison. FPCOLUMN vs
  Arrow IPC is columnar-vs-columnar and is.

## Dataset (identical for every cell)

`--rows N` (default 1,000,000), generated in memory, deterministic, `g = 1..N`:

| column | type | value |
|---|---|---|
| `id` | int32 | `g` |
| `sku` | utf8 | `"SKU-" + g` |
| `name` | utf8 | `"Product " + g` |
| `price` | float64 | `g * 0.5` |
| `stock` | int32 | `g % 500` |
| `active` | bool | `g % 2 == 0` |

## The timed operation (identical in every cell)

Within the timed region each cell does exactly the same thing: **decode the
payload into its natural in-memory form and compute six aggregates** over it —
`rows`, `Σ id`, `Σ stock`, `Σ price`, `count(active)`, `Σ len(text bytes)`.
No per-value Python objects are built, in any cell: the aggregates are native on
both sides (a C loop over the frame; `pyarrow.compute` over the buffers).

The full logical checksum below is computed **once, outside the timed region**,
from the decoded values, and reported as `checksum`. It is the proof that every
cell carried the same data; it is not part of the measurement.

## Checksum (proves every cell carries the same values)

All 64-bit unsigned with wrapping arithmetic, over the logical values in row
order:

- `sum_id` = Σ id
- `sum_stock` = Σ stock
- `sum_price_bits` = Σ bit_cast<uint64>(price)
- `trues` = Σ active
- `text_hash` = FNV-1a 64 over, per row: `len(sku)` as u32 little-endian, sku
  bytes, `len(name)` as u32 little-endian, name bytes

Every cell must report the same five numbers.

## CLI contract

Each tool prints **one line of JSON on stdout** and exits 0:

```json
{"cell": "fpcolumn", "rows": 1000000, "payload_bytes": 0, "encode_ms": 0.0,
 "decode_ms": 0.0, "rows_per_s": 0.0, "gb_per_s": 0.0, "p50_ms": 0.0,
 "p99_ms": 0.0, "copies": 0, "copy_stages": {"": 0},
 "cpu_user_ms": 0.0, "cpu_sys_ms": 0.0, "rss_kb": 0,
 "checksum": {"sum_id": 0, "sum_stock": 0, "sum_price_bits": 0, "trues": 0,
              "text_hash": 0}}
```

- Format cells (in-process, no transport):
  - `fpg_bench --rows N --format fpcolumn|fprows --reps R [--emit-payload PATH]`
  - `arrow_bench.py --rows N --reps R [--emit-payload PATH]`
  - `--reps R` repeats the encode+decode R times; report the median of the R
    runs (and p50/p99 over chunks if chunked).
- Transport cells:
  - `transport_bench --which shm|uds --payload FILE [--chunk BYTES]`
  - The binary forks its own consumer, so producer and consumer are the same
    code and each side's CPU is reported (`cpu_user_ms`/`cpu_sys_ms` are the
    sum of both sides; add `producer_cpu_ms`/`consumer_cpu_ms`).
  - p50/p99 latency = per chunk, consumer's "received" timestamp minus
    producer's "published" timestamp, using `CLOCK_MONOTONIC` (system-wide, so
    timestamps from both processes are comparable).
  - Throughput is over the payload bytes actually moved.

## Copy accounting is part of the result

Count every copy and name its stage, e.g. `copy_stages: {"values->buffer": 1,
"buffer->shm": 1, "shm->consumer": 0, "bytes->values": 0}`. Zero-copy steps must
be *demonstrated* (by not copying), not asserted: the codec readers alias the
payload, the shm path mmaps, the consumer decodes in place. Say what each count
means in a comment next to the counter.

## Rules for the agents building this

1. **One measurement at a time.** Timing under load measures the neighbours.
   Agents compile and smoke-test with tiny sizes (`--rows 1000`) in parallel;
   the real matrix is run sequentially afterwards, from the runner.
2. Compile and run inside the dev image, repository mounted read-write but with
   **all build artifacts written under `benchmarks/internals/codec_transport/build/`**
   (git-ignored), e.g.
   `docker run --rm -v "$PWD":/src -w /src/benchmarks/rivals/codec_transport fastpostgres-dev bash build_<unit>.sh`
   Build at **-O3**: pyarrow ships optimized (SIMD) kernels and the PostgreSQL
   extension itself builds -O3, so the C++ units are compiled at the same level.
   Comparing an -O2 scalar loop against a vectorized kernel measures the flags,
   not the format.
3. **File ownership.** Each unit owns only its own files under
   `benchmarks/internals/codec_transport/` (`fpg_*` for the FPG codec, `arrow_*` for the
   Arrow codec, `transport_*` for the transports, `run_matrix.py` +
   `docs/CODEC-TRANSPORT.md` for the runner and the write-up). Do not edit
   `justfile`, `README.md`, `benchmarks/charts/`, `docs/METHODOLOGY.md`,
   `docs/LIMITATIONS.md`, `docs/CODEC-TRANSPORT.md` is the runner's, or
   `benchmarks/rivals/sqlalchemy_bench.py`.
4. A unit is done when it compiles/runs in the image, produces the JSON above
   for `--rows 1000`, and its checksum matches the others (when they exist).
