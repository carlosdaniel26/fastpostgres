#!/usr/bin/env python3
"""Arrow IPC cell of the codec/transport isolation experiment (format factor).

Contract: ``bench/internals/codec_transport/SPEC.md`` (dataset, checksum, timed
operation, CLI + JSON schema, copy accounting).  One process, no transport:
build the dataset as Arrow arrays, serialise it as an Arrow **IPC stream**, read
it back, run the six aggregates with ``pyarrow.compute``, and prove the payload
carried the SPEC values by computing the full logical checksum once, outside the
timed region.

Everything that touches a row value runs in Arrow/numpy C code: no per-value
Python object is created anywhere (1e6 Python strings would measure CPython, not
Arrow, and would be incomparable to the C++ cell).

Decisions the matrix depends on (all deliberate controls)
--------------------------------------------------------
* **IPC stream, not IPC file** (``pa.ipc.new_stream``).  A stream is the natural
  "bytes as transmitted" shape for the transport experiment: self-delimiting,
  no footer, no magic string, no random-access sink needed.  The file format
  would append a footer + magic that only a seekable consumer can use, and the
  UDS/SHM transport does not require one.
* **No compression** (``IpcWriteOptions(compression=None)``).  The FPG cell
  moves its raw frame; the comparison is format cost, not entropy coding, so
  both cells move the format's natural bytes.
* **Single thread** (``pa.set_cpu_count(1)``, ``pa.set_io_thread_count(1)``).
  The FPG cell is a single-threaded C loop, so thread count is part of the
  control, held at 1.
* **encode_ms = array_build_ms + ipc_write_ms** and **decode_ms = ipc_read_ms +
  aggregate_ms**: encode covers building the Arrow arrays from the source
  vectors plus serialisation, decode covers deserialisation plus the six
  aggregates -- i.e. exactly the timed operation in SPEC.md.  ``breakdown``
  reports the four pieces separately, so either split can be recomputed.
* ``--emit-payload`` is a *side artifact*: it writes the very bytes the timed
  decode consumed, but the file write is not timed and the timed decode never
  reads the file, so ``encode_ms``/``decode_ms`` do not depend on whether
  ``--emit-payload`` was passed.  The emitted file is then re-decoded through
  ``pa.memory_map`` (untimed) to verify it and to exercise the mmap path.

Copy accounting is measured, not asserted: see ``alias_probe`` and
``zero_copy_evidence`` in the JSON.
"""

from __future__ import annotations

import argparse
import json
import resource
import sys
import time
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc

CELL = "arrow-ipc"

FNV_OFFSET_BASIS = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = 0xFFFFFFFFFFFFFFFF

# Declared schema == the IPC stream's schema, in the order the SPEC lists.
SCHEMA = pa.schema(
    [
        pa.field("id", pa.int32(), nullable=False),
        pa.field("sku", pa.string(), nullable=False),
        pa.field("name", pa.string(), nullable=False),
        pa.field("price", pa.float64(), nullable=False),
        pa.field("stock", pa.int32(), nullable=False),
        pa.field("active", pa.bool_(), nullable=False),
    ]
)

# Uncompressed, V5 metadata, default 8-byte alignment: the plainest stream
# Arrow can write, so payload_bytes is format cost, not an encoding choice.
WRITE_OPTIONS = pa.ipc.IpcWriteOptions(
    compression=None, metadata_version=pa.ipc.MetadataVersion.V5
)


# --------------------------------------------------------------------------- #
# dataset: numpy vectors only -- no Python per-value objects
# --------------------------------------------------------------------------- #
def generate_dataset(n: int) -> dict[str, np.ndarray]:
    """SPEC.md dataset as source column vectors (g = 1..n)."""
    g = np.arange(1, n + 1, dtype=np.int64)
    return {
        "id": g.astype(np.int32),
        "price": g.astype(np.float64) * 0.5,  # exact: 0.5 is a power of two
        "stock": (g % 500).astype(np.int32),
        "active": (g % 2 == 0),
    }


def text_buffers(prefix: bytes, values: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """(int32 offsets[n+1], uint8 data) for ``prefix + str(values[i])``.

    Fully vectorised: the loops are over the *digit positions* of the widest
    value (<= 19; 7 at 1e6 rows) and over the bytes of the ascii prefix -- never
    over rows.  Values are written straight into the final Arrow buffers, so the
    utf8 arrays are built without any intermediate Python str.
    """
    n = int(values.shape[0])
    plen = len(prefix)
    if n == 0:
        return np.zeros(1, np.int32), np.zeros(plen, np.uint8)
    if values.dtype.kind not in "iu":
        raise TypeError(f"text_buffers needs an integer array, got {values.dtype}")
    v = np.ascontiguousarray(values).astype(np.int64, copy=False)
    if int(v.min()) < 0:
        raise ValueError("text_buffers only implements non-negative integers")

    # decimal digit count (>= 1); loop over powers of ten, not over rows
    digits = np.ones(n, np.int64)
    t = 10
    vmax = int(v.max())
    while t <= vmax:
        digits += v >= t
        t *= 10

    offsets64 = np.empty(n + 1, np.int64)
    offsets64[0] = 0
    np.cumsum(digits + plen, out=offsets64[1:])
    total = int(offsets64[-1])
    if total >= 2**31:
        raise ValueError("utf8 offsets would overflow int32")

    data = np.empty(total, np.uint8)
    starts = offsets64[:-1]
    for j in range(plen):  # ascii prefix
        data[starts + j] = prefix[j]
    num_start = starts + plen
    for k in range(int(digits.max())):  # digit positions, most significant first
        m = digits > k
        if not m.any():
            continue
        place = np.where(m, digits - 1 - k, 0)
        for p in np.unique(place[m]):
            sel = m & (place == p)
            data[num_start[sel] + k] = (48 + (v[sel] // 10 ** int(p)) % 10).astype(np.uint8)

    return offsets64.astype(np.int32), data


# --------------------------------------------------------------------------- #
# encode
# --------------------------------------------------------------------------- #
def _wraps_numpy(array: pa.Array, np_values: np.ndarray) -> bool:
    """True when the Arrow array's value buffer *is* the numpy buffer."""
    buffers = array.buffers()
    if len(buffers) < 2 or buffers[1] is None:
        return False
    return buffers[1].address == np_values.ctypes.data and buffers[1].size >= np_values.nbytes


def _wraps_utf8(array: pa.Array, offsets: np.ndarray, data: np.ndarray) -> bool:
    """True when both utf8 buffers of the Arrow array are the numpy buffers."""
    buffers = array.buffers()
    return (
        buffers[1].address == offsets.ctypes.data
        and buffers[2].address == data.ctypes.data
    )


def build_batch(
    src: dict[str, np.ndarray], n: int, text: dict[str, tuple[np.ndarray, np.ndarray]]
) -> tuple[pa.RecordBatch, dict[str, bool]]:
    """Arrow arrays + RecordBatch from the numpy source vectors.

    Zero-copy where Arrow's layout allows it: int32/float64 wrap the numpy
    buffer, and the utf8 arrays wrap the offsets+data buffers built with the
    dataset (``text``, untimed -- the C++ cell's dataset holds its text bytes
    before its timer starts too).  ``active`` is the one exception -- numpy bool
    is 1 byte/value, Arrow bool is 1 bit/value, so the bit-packing copy is
    mandatory; it is counted in copy_stages.
    """
    sku_off, sku_data = text["sku"]
    name_off, name_data = text["name"]
    sku = pa.Array.from_buffers(
        pa.string(), n, [None, pa.py_buffer(sku_off), pa.py_buffer(sku_data)]
    )
    name = pa.Array.from_buffers(
        pa.string(), n, [None, pa.py_buffer(name_off), pa.py_buffer(name_data)]
    )
    id_arr = pa.array(src["id"])
    price_arr = pa.array(src["price"])
    stock_arr = pa.array(src["stock"])
    active_arr = pa.array(src["active"])  # bit-packs -> one mandatory copy
    batch = pa.RecordBatch.from_arrays(
        [id_arr, sku, name, price_arr, stock_arr, active_arr], schema=SCHEMA
    )
    zero_copy = {
        "id": _wraps_numpy(id_arr, src["id"]),
        "sku": _wraps_utf8(sku, sku_off, sku_data),
        "name": _wraps_utf8(name, name_off, name_data),
        "price": _wraps_numpy(price_arr, src["price"]),
        "stock": _wraps_numpy(stock_arr, src["stock"]),
        "active": _wraps_numpy(active_arr, src["active"]),  # False by design
    }
    return batch, zero_copy


def ipc_encode(batch: pa.RecordBatch) -> pa.Buffer:
    """RecordBatch -> Arrow IPC *stream* bytes (schema msg + batch msg + EOS).

    ``BufferOutputStream.getvalue()`` is zero-copy (the returned Buffer aliases
    the stream's internal buffer and is invalidated by further writes -- we never
    write again), so the payload costs no extra copy here.
    """
    sink = pa.BufferOutputStream()
    writer = pa.ipc.new_stream(sink, batch.schema, options=WRITE_OPTIONS)
    writer.write_batch(batch)
    writer.close()  # writes the 8-byte end-of-stream marker
    return sink.getvalue()


# --------------------------------------------------------------------------- #
# decode
# --------------------------------------------------------------------------- #
def ipc_decode(source) -> pa.RecordBatch:
    """IPC bytes -> the single RecordBatch they carry (buffers alias the source)."""
    batches = []
    reader = pa.ipc.open_stream(source)
    try:
        for batch in reader:
            batches.append(batch)
    finally:
        reader.close()
    if len(batches) != 1:
        raise RuntimeError(f"payload carries {len(batches)} record batches, expected 1")
    return batches[0]


def aggregate(batch: pa.RecordBatch) -> dict:
    """The six SPEC aggregates, ``pyarrow.compute`` only, no per-value Python.

    Only scalars come out; the allocations are the two ``binary_length`` arrays
    and the ``cast(active, int32)`` temporary (counted as aggregate temporaries
    in copy_stages).
    """
    col = batch.column
    return {
        "rows": batch.num_rows,
        "sum_id": int(pc.sum(col("id")).as_py()),
        "sum_stock": int(pc.sum(col("stock")).as_py()),
        "sum_price": float(pc.sum(col("price")).as_py()),
        "trues": int(pc.sum(pc.cast(col("active"), pa.int32())).as_py()),
        "text_bytes": int(pc.sum(pc.binary_length(col("sku"))).as_py())
        + int(pc.sum(pc.binary_length(col("name"))).as_py()),
    }


# --------------------------------------------------------------------------- #
# validation checksum: once, outside the timed region, on the decoded buffers
# --------------------------------------------------------------------------- #
def fnv1a64_text(
    sku_off, sku_data, name_off, name_data, n: int
) -> int:
    """FNV-1a 64 over, per row: len(sku) u32 LE, sku bytes, len(name) u32 LE, name bytes.

    One chain over the interleaved sequence (SPEC.md).  Deliberately a plain
    Python loop over the utf8 memoryviews: it runs once, untimed, and is the
    cross-cell proof rather than a measurement.
    """
    h = FNV_OFFSET_BASIS
    prime = FNV_PRIME
    mask = MASK64
    for i in range(n):
        # int() on the offsets: numpy scalars would propagate into `h` as
        # fixed-width ints and break the wrapping u64 arithmetic.
        s = int(sku_off[i])
        e = int(sku_off[i + 1])
        length = e - s
        h = ((h ^ (length & 0xFF)) * prime) & mask
        h = ((h ^ ((length >> 8) & 0xFF)) * prime) & mask
        h = ((h ^ ((length >> 16) & 0xFF)) * prime) & mask
        h = ((h ^ ((length >> 24) & 0xFF)) * prime) & mask
        for b in sku_data[s:e]:
            h = ((h ^ b) * prime) & mask
        s = int(name_off[i])
        e = int(name_off[i + 1])
        length = e - s
        h = ((h ^ (length & 0xFF)) * prime) & mask
        h = ((h ^ ((length >> 8) & 0xFF)) * prime) & mask
        h = ((h ^ ((length >> 16) & 0xFF)) * prime) & mask
        h = ((h ^ ((length >> 24) & 0xFF)) * prime) & mask
        for b in name_data[s:e]:
            h = ((h ^ b) * prime) & mask
    return h


def checksum_from_batch(batch: pa.RecordBatch, n: int) -> dict:
    """The SPEC logical checksum from the decoded values' own buffers.

    All arithmetic wraps modulo 2**64 (numpy unsigned sums wrap silently).
    """
    for i in range(batch.num_columns):
        if batch.column(i).null_count:
            raise RuntimeError(f"column {batch.schema.names[i]} carries nulls")

    ids = np.frombuffer(memoryview(batch.column("id").buffers()[1]), np.int32, n)
    stock = np.frombuffer(memoryview(batch.column("stock").buffers()[1]), np.int32, n)
    price = np.frombuffer(memoryview(batch.column("price").buffers()[1]), np.float64, n)
    active_bits = np.frombuffer(memoryview(batch.column("active").buffers()[1]), np.uint8)
    sku_off = np.frombuffer(memoryview(batch.column("sku").buffers()[1]), np.int32, n + 1)
    name_off = np.frombuffer(memoryview(batch.column("name").buffers()[1]), np.int32, n + 1)
    sku_data = memoryview(batch.column("sku").buffers()[2])
    name_data = memoryview(batch.column("name").buffers()[2])

    return {
        "sum_id": int(np.sum(ids, dtype=np.uint64)),
        "sum_stock": int(np.sum(stock, dtype=np.uint64)),
        "sum_price_bits": int(np.sum(price.view(np.uint64), dtype=np.uint64)),
        # bit order inside the bitmap is irrelevant for a popcount
        "trues": int(np.unpackbits(active_bits, count=n).sum(dtype=np.uint64)),
        "text_hash": fnv1a64_text(sku_off, sku_data, name_off, name_data, n),
    }


def source_checksum(src: dict[str, np.ndarray]) -> dict:
    """The same numeric checksum computed from the *source* vectors (cheap)."""
    return {
        "sum_id": int(np.sum(src["id"], dtype=np.uint64)),
        "sum_stock": int(np.sum(src["stock"], dtype=np.uint64)),
        "sum_price_bits": int(np.sum(src["price"].view(np.uint64), dtype=np.uint64)),
        "trues": int(np.count_nonzero(src["active"])),
    }


# --------------------------------------------------------------------------- #
# zero-copy evidence (measured, never assumed)
# --------------------------------------------------------------------------- #
def _value_buffers(col: pa.Array) -> list[tuple[str, pa.Buffer | None]]:
    """The buffers holding a column's *values* (never the validity bitmap)."""
    buffers = col.buffers()
    if pa.types.is_string(col.type) or pa.types.is_binary(col.type):
        return [("offsets", buffers[1]), ("data", buffers[2])]
    return [("data", buffers[1])]


def alias_probe(
    batch: pa.RecordBatch, region: bytes, span: tuple[int, int] | None = None
) -> dict:
    """Show whether the decoded buffers alias ``region`` (payload / mapped file).

    Two independent measured signals -- nothing about pyarrow internals is
    assumed:

    * ``alias_delta_by_buffer``: for every value buffer whose exact bytes are
      found *uniquely* in the region, compute ``buffer.address -
      byte_offset_in_region``.  If the buffers are slices of one contiguous
      region, every delta is the identical number (and that number is the
      region's base address).  One consistent delta across >= 2 buffers proves
      aliasing; differing deltas prove the reader copied.
    * ``addresses_inside_payload_span``: when the region's own address range is
      known (the in-memory payload Buffer), range-check every value buffer.
    """
    deltas: dict[str, int] = {}
    probed = 0
    for i in range(batch.num_columns):
        for tag, buf in _value_buffers(batch.column(i)):
            if buf is None or buf.size == 0:
                continue
            probed += 1
            raw = buf.to_pybytes()
            off = region.find(raw)
            if off == -1 or region.find(raw, off + 1) != -1:
                continue  # not uniquely locatable -> unusable as evidence
            deltas[f"{batch.schema.names[i]}/{tag}"] = buf.address - off

    distinct = sorted(set(deltas.values()))
    aliased = len(deltas) >= 2 and len(distinct) == 1
    result = {
        "aliased": aliased,
        "region_base_address": distinct[0] if aliased else None,
        "buffers_probed": probed,
        "buffers_uniquely_located": len(deltas),
        "alias_delta_by_buffer": deltas,
        "addresses_inside_payload_span": None,
    }
    if span is not None:
        base, size = span
        inside = [
            base <= buf.address < base + size
            for i in range(batch.num_columns)
            for _, buf in _value_buffers(batch.column(i))
            if buf is not None and buf.size
        ]
        result["addresses_inside_payload_span"] = bool(inside) and all(inside)
    return result


def message_framing(payload: pa.Buffer) -> dict | None:
    """Split the stream into metadata / body / framing(+EOS) bytes."""
    try:
        messages = list(pa.ipc.MessageReader.open_stream(payload))
    except Exception as exc:  # diagnostics only, never fatal
        return {"error": f"{type(exc).__name__}: {exc}"}
    metadata = sum(len(m.metadata) for m in messages)
    body = sum(len(m.body) for m in messages)
    return {
        "messages": len(messages),
        "metadata_bytes": metadata,
        "body_bytes": body,
        "framing_and_eos_bytes": payload.size - metadata - body,
    }


# --------------------------------------------------------------------------- #
# one timed repetition
# --------------------------------------------------------------------------- #
def run_rep(
    src: dict[str, np.ndarray], n: int, text: dict[str, tuple[np.ndarray, np.ndarray]], keep: bool = False
) -> dict:
    """One timed encode+decode: assemble arrays -> IPC write -> IPC read -> aggregate.

    The dataset (numeric vectors and utf8 buffers) exists before this function is
    called; ``array_build_ms`` is assembly only, mirroring the C++ cell, whose
    timer also starts with the frame build and not with the values.
    """
    t0 = time.perf_counter_ns()
    batch, zero_copy = build_batch(src, n, text)
    t1 = time.perf_counter_ns()
    payload = ipc_encode(batch)
    t2 = time.perf_counter_ns()
    decoded = ipc_decode(pa.BufferReader(payload))
    t3 = time.perf_counter_ns()
    aggs = aggregate(decoded)
    t4 = time.perf_counter_ns()

    def ms(a: int, b: int) -> float:
        return (b - a) / 1e6

    run = {
        "array_build_ms": ms(t0, t1),
        "ipc_write_ms": ms(t1, t2),
        "ipc_read_ms": ms(t2, t3),
        "aggregate_ms": ms(t3, t4),
        "encode_ms": ms(t0, t2),  # array build + IPC write
        "decode_ms": ms(t2, t4),  # IPC read + aggregates
        "total_ms": ms(t0, t4),
        "payload_bytes": int(payload.size),
        "aggregates": aggs,
        "zero_copy": zero_copy,
    }
    if keep:  # heavy objects are only retained where the evidence needs them
        run["batch"] = batch
        run["decoded"] = decoded
        run["payload"] = payload
    return run


# --------------------------------------------------------------------------- #
# helpers
# --------------------------------------------------------------------------- #
def median(values: list[float]) -> float:
    s = sorted(values)
    mid = len(s) // 2
    return float(s[mid]) if len(s) % 2 else (s[mid - 1] + s[mid]) / 2.0


def percentile(values: list[float], q: float) -> float:
    """Linear interpolation (numpy's default), so R=1 yields the single value."""
    s = sorted(values)
    if len(s) == 1:
        return float(s[0])
    pos = (len(s) - 1) * q
    lo = int(pos)
    hi = min(lo + 1, len(s) - 1)
    return float(s[lo] * (1.0 - pos + lo) + s[hi] * (pos - lo))


def r(x: float, nd: int = 4) -> float:
    return round(float(x), nd)


def parse_args(argv=None):
    p = argparse.ArgumentParser(
        description="Arrow IPC cell: encode/decode + six aggregates (SPEC.md)"
    )
    p.add_argument("--rows", type=int, default=1_000_000, help="rows to generate (default 1e6)")
    p.add_argument("--reps", type=int, default=1, help="timed encode+decode repetitions")
    p.add_argument(
        "--emit-payload",
        metavar="PATH",
        default=None,
        help="also write the IPC bytes exactly as transmitted (untimed side artifact)",
    )
    p.add_argument(
        "--no-alias-probe",
        action="store_true",
        help="skip the (untimed) zero-copy evidence probe on very large runs",
    )
    args = p.parse_args(argv)
    if args.rows < 1:
        p.error("--rows must be >= 1")
    if args.reps < 1:
        p.error("--reps must be >= 1")
    return args


# --------------------------------------------------------------------------- #
# main
# --------------------------------------------------------------------------- #
def main(argv=None) -> int:
    args = parse_args(argv)
    n = args.rows
    reps = args.reps

    # Held constant for every rep: one worker thread, one I/O thread.
    pa.set_cpu_count(1)
    pa.set_io_thread_count(1)

    ru0 = resource.getrusage(resource.RUSAGE_SELF)

    t = time.perf_counter()
    src = generate_dataset(n)
    # The utf8 buffers are part of the *dataset*, not of encoding. The C++ cell's
    # dataset holds its text bytes before its timer starts, so this one must too:
    # building them per rep would charge Arrow for materialising the text.
    text = {
        "sku": text_buffers(b"SKU-", src["id"]),
        "name": text_buffers(b"Product ", src["id"]),
    }
    dataset_gen_ms = (time.perf_counter() - t) * 1e3
    ru1 = resource.getrusage(resource.RUSAGE_SELF)

    runs = [run_rep(src, n, text, keep=(i == 0)) for i in range(reps)]
    first = runs[0]
    ru2 = resource.getrusage(resource.RUSAGE_SELF)

    payload = first["payload"]
    payload_bytes = first["payload_bytes"]
    decoded = first["decoded"]

    # --- validation checksum: once, outside every timed region ---------------
    t = time.perf_counter()
    checksum = checksum_from_batch(decoded, n)
    checksum_ms = (time.perf_counter() - t) * 1e3

    # --- cross-checks against the source vectors (untimed) -------------------
    src_checksum = source_checksum(src)
    sku_off_src, sku_data_src = text_buffers(b"SKU-", src["id"])
    name_off_src, name_data_src = text_buffers(b"Product ", src["id"])
    source_match = {
        **{k: src_checksum[k] == checksum[k] for k in src_checksum},
        "sku_text": bool(
            np.array_equal(
                np.frombuffer(memoryview(decoded.column("sku").buffers()[1]), np.int32, n + 1),
                sku_off_src,
            )
            and np.array_equal(
                np.frombuffer(memoryview(decoded.column("sku").buffers()[2]), np.uint8),
                sku_data_src,
            )
        ),
        "name_text": bool(
            np.array_equal(
                np.frombuffer(memoryview(decoded.column("name").buffers()[1]), np.int32, n + 1),
                name_off_src,
            )
            and np.array_equal(
                np.frombuffer(memoryview(decoded.column("name").buffers()[2]), np.uint8),
                name_data_src,
            )
        ),
    }

    # --- zero-copy evidence (untimed) ---------------------------------------
    evidence: dict = {"decode_source": "buffer_reader", "python_objects_per_value": 0}
    if not args.no_alias_probe:
        evidence["buffer_reader"] = alias_probe(
            decoded, payload.to_pybytes(), span=(payload.address, payload.size)
        )

    # --- optional artifact for the transport cell ---------------------------
    emit_info = None
    if args.emit_payload:
        path = Path(args.emit_payload)
        if not path.parent.exists():  # the runner usually creates this itself
            path.parent.mkdir(parents=True, exist_ok=True)
        raw = payload.to_pybytes()  # the exact bytes the timed decode consumed
        t = time.perf_counter()
        path.write_bytes(raw)  # write(2), untimed: not part of encode_ms
        write_ms = (time.perf_counter() - t) * 1e3

        # Re-decode through pa.memory_map -- the file path the SPEC names as a
        # zero-copy read -- and check the artifact equals the timed payload.
        t = time.perf_counter()
        with pa.memory_map(str(path), "r") as source:
            mmap_batch = ipc_decode(source)
            mmap_aggs = aggregate(mmap_batch)
            file_bytes = path.read_bytes()
            mmap_probe = None if args.no_alias_probe else alias_probe(mmap_batch, file_bytes)
        mmap_ms = (time.perf_counter() - t) * 1e3
        emit_info = {
            "path": str(path),
            "bytes": len(file_bytes),
            "write_ms": r(write_ms),
            "identical_to_timed_payload": file_bytes == raw,
            "mmap_decode_ms": r(mmap_ms),
            "mmap_aggregates_match": mmap_aggs == first["aggregates"],
            "mmap_evidence": mmap_probe,
        }
        evidence["memory_map"] = mmap_probe

    ru3 = resource.getrusage(resource.RUSAGE_SELF)

    # --- medians / percentiles over the R reps ------------------------------
    def med(key: str) -> float:
        return median([run[key] for run in runs])

    encode_ms, decode_ms = med("encode_ms"), med("decode_ms")
    total_ms = encode_ms + decode_ms
    per_run_totals = [run["total_ms"] for run in runs]

    # Extra, not a substitute: the first rep pays Arrow's one-time lazy kernel /
    # IPC initialisation, so the tail medians show the format's steady-state
    # cost.  encode_ms/decode_ms above stay the literal median over all R reps.
    steady_state = None
    if len(runs) > 1:
        tail = runs[1:]
        steady_state = {
            "reps": len(tail),
            "array_build_ms": r(median([x["array_build_ms"] for x in tail])),
            "ipc_write_ms": r(median([x["ipc_write_ms"] for x in tail])),
            "ipc_read_ms": r(median([x["ipc_read_ms"] for x in tail])),
            "aggregate_ms": r(median([x["aggregate_ms"] for x in tail])),
            "encode_ms": r(median([x["encode_ms"] for x in tail])),
            "decode_ms": r(median([x["decode_ms"] for x in tail])),
            "total_ms": r(median([x["total_ms"] for x in tail])),
        }

    # --- copy accounting (measured where possible) --------------------------
    numeric_copies = sum(
        1 for name in ("id", "price", "stock") if not first["zero_copy"][name]
    )
    reader_aliases = bool(evidence.get("buffer_reader", {}).get("aliased"))
    copy_stages = {
        # numpy source vectors -> Arrow arrays: 0 measured for id/price/stock
        # (pa.array wraps the numpy buffer; utf8 buffers are filled in place);
        # `active` MUST be repacked because Arrow bool is bit-packed.
        "values->arrow_arrays": numeric_copies + 1,
        # the IPC writer serialises the column buffers into the output stream
        "arrow_arrays->ipc_payload": 1,
        # decode: 0 when the reader aliases the payload (verified by address),
        # else the whole batch was copied out of it.
        "ipc_payload->record_batch": 0 if reader_aliases else 1,
        # temporaries allocated inside the timed aggregates (binary_length x2,
        # cast(active,int32)); they do not move the payload
        "aggregate_temporaries": 3,
        # optional artifact for the transport cell (write(2), untimed)
        "ipc_payload->emit_file": 1 if emit_info else 0,
    }
    copy_stage_notes = {
        "values->arrow_arrays": "numpy->Arrow wrap (0 copies, see zero_copy_by_column) plus the mandatory numpy-bool -> bit-packed-bool repack",
        "arrow_arrays->ipc_payload": "IPC serialisation writes the buffers into the stream; not aliased",
        "ipc_payload->record_batch": (
            "0 = the decoded batch aliases the payload bytes; see zero_copy_evidence"
            if "buffer_reader" in evidence
            else "unmeasured (--no-alias-probe): counted as 1 copy, conservatively"
        ),
        "aggregate_temporaries": "allocations of the two binary_length arrays + the cast(active) array inside the timed region",
        "ipc_payload->emit_file": "write(2) of the emitted artifact; not part of encode_ms",
    }

    out = {
        # ---- SPEC.md top level ----
        "cell": CELL,
        "rows": n,
        "payload_bytes": payload_bytes,
        "encode_ms": r(encode_ms),
        "decode_ms": r(decode_ms),
        "rows_per_s": r(n / (total_ms / 1e3), 1),
        "gb_per_s": r(payload_bytes / 1e9 / (total_ms / 1e3), 6),
        "p50_ms": r(percentile(per_run_totals, 0.50)),
        "p99_ms": r(percentile(per_run_totals, 0.99)),
        "copies": sum(copy_stages.values()),
        "copy_stages": copy_stages,
        "cpu_user_ms": r((ru2.ru_utime - ru0.ru_utime) * 1e3),
        "cpu_sys_ms": r((ru2.ru_stime - ru0.ru_stime) * 1e3),
        "rss_kb": int(ru3.ru_maxrss),
        "checksum": checksum,
        # ---- extras (honest split + evidence; extra keys are allowed) ----
        "engine": {
            "pyarrow": pa.__version__,
            "numpy": np.__version__,
            "python": sys.version.split()[0],
            "cpu_threads": 1,
            "io_threads": 1,
        },
        "ipc": {
            "kind": "stream",
            "writer": "pa.ipc.new_stream",
            "compression": "none",
            "metadata_version": "V5",
            "payload_framing": None if args.no_alias_probe else message_framing(payload),
        },
        "reps": reps,
        "breakdown": {
            "array_build_ms": r(med("array_build_ms")),
            "ipc_write_ms": r(med("ipc_write_ms")),
            "ipc_read_ms": r(med("ipc_read_ms")),
            "aggregate_ms": r(med("aggregate_ms")),
            "encode_ms": r(encode_ms),
            "decode_ms": r(decode_ms),
            "total_ms": r(total_ms),
        },
        "runs": [
            {
                "array_build_ms": r(run["array_build_ms"]),
                "ipc_write_ms": r(run["ipc_write_ms"]),
                "ipc_read_ms": r(run["ipc_read_ms"]),
                "aggregate_ms": r(run["aggregate_ms"]),
                "encode_ms": r(run["encode_ms"]),
                "decode_ms": r(run["decode_ms"]),
                "total_ms": r(run["total_ms"]),
                "payload_bytes": run["payload_bytes"],
            }
            for run in runs
        ],
        "encode_p50_ms": r(percentile([run["encode_ms"] for run in runs], 0.50)),
        "encode_p99_ms": r(percentile([run["encode_ms"] for run in runs], 0.99)),
        "decode_p50_ms": r(percentile([run["decode_ms"] for run in runs], 0.50)),
        "decode_p99_ms": r(percentile([run["decode_ms"] for run in runs], 0.99)),
        "encode_ms_basis": "median(array_build_ms + ipc_write_ms) over --reps",
        "decode_ms_basis": "median(ipc_read_ms + aggregate_ms) over --reps",
        "steady_state": steady_state,
        "steady_state_note": (
            "medians over reps 2..R only (null when R=1): the first rep carries "
            "Arrow's one-time lazy initialisation, which is process start-up rather "
            "than format cost; the headline encode_ms/decode_ms stay the literal "
            "median over all R reps"
        ),
        "percentile_basis": "per-rep (encode+decode) round trip; this cell does not chunk",
        "throughput_basis": "rows and payload_bytes over the median round trip",
        "dataset_gen_ms": r(dataset_gen_ms),
        "checksum_ms": r(checksum_ms, 3),
        "checksum_source": "decoded RecordBatch value buffers, once, outside the timed region",
        "aggregates": first["aggregates"],
        "aggregates_match_checksum": bool(
            first["aggregates"]["sum_id"] == checksum["sum_id"]
            and first["aggregates"]["sum_stock"] == checksum["sum_stock"]
            and first["aggregates"]["trues"] == checksum["trues"]
        ),
        "aggregates_match_checksum_note": (
            "sum_price is a float64 sum, sum_price_bits is a wrap-around u64 sum of "
            "the same bit patterns -- different quantities by construction"
        ),
        "source_matches_decoded": source_match,
        "all_reps_identical": all(run["aggregates"] == first["aggregates"] for run in runs),
        "payload_bytes_stable": all(run["payload_bytes"] == payload_bytes for run in runs),
        "zero_copy_by_column": first["zero_copy"],
        "zero_copy_evidence": evidence,
        "copy_stage_notes": copy_stage_notes,
        "emit_payload": emit_info,
        "cpu_user_ms_timed": r((ru2.ru_utime - ru1.ru_utime) * 1e3),
        "cpu_sys_ms_timed": r((ru2.ru_stime - ru1.ru_stime) * 1e3),
        "cpu_user_ms_total": r((ru3.ru_utime - ru0.ru_utime) * 1e3),
        "cpu_sys_ms_total": r((ru3.ru_stime - ru0.ru_stime) * 1e3),
    }
    print(json.dumps(out, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
