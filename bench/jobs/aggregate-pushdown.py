"""The aggregate pushdown, measured: SUM/COUNT through the driver's reduce path.

The aggregate API reduces the *output* of a read, so the measured statement is

    SELECT price FROM fp_agg_bench

with the plan ``sum(price) + count_all``, checked against the server's own
``SELECT sum(price), count(*) FROM fp_agg_bench``. Run it through
``Cursor.aggregate`` and report what the worker actually shipped. On a
client-side reduce the segment holds the whole columnar batch, so
``payload_size`` is the batch; with FASTPOSTGRES_OP_REDUCE the worker
accumulates and publishes one scalar per op, so it is a few dozen bytes.
Same query, same API, same rows: only the payload size moves.

Two batch sizes, because they answer different questions:

  batch=100000   the streaming shape (the driver's default): rows/s, 20 requests
  batch=rows     one request: the payload measured is the whole result

Usage: python3 bench/jobs/aggregate-pushdown.py [rows] [reps]

Run inside the dev image under the shared lock, on the baseline and the change:

  flock /tmp/fpq-bench.lock -c 'docker run --rm --shm-size=3g \
    -e BENCH_GIT_REV=baseline -v "$PWD/bench/jobs/aggregate-pushdown.py:/src/bench/jobs/aggregate-pushdown.py:ro" \
    fpq-baseline python3 /src/bench/jobs/aggregate-pushdown.py'
"""
import os
import statistics
import struct
import sys
import time

import fastpostgres as fp
from fastpostgres import dbapi

DATABASE_URL = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")
TABLE = "fp_agg_bench"

ROWS = int(sys.argv[1]) if len(sys.argv) > 1 else 2_000_000
REPS = int(sys.argv[2]) if len(sys.argv) > 2 else 5
BATCHES = (100_000, ROWS)

SQL = f"SELECT price FROM {TABLE}"
OPS = {"price_sum": ("sum", "price"), "rows": ("count_all", None)}
VERIFY = f"SELECT sum(price), count(*) FROM {TABLE}"


def now():
    return time.perf_counter()


def payload_size(seg):
    """One segment's published payload bytes (fastpostgres_shm.h: offset 16)."""
    with open(os.path.join("/dev/shm", seg), "rb") as f:
        head = f.read(24)
    if len(head) < 24 or head[:8] != b"FPQSHMEM":
        return 0
    return struct.unpack("<Q", head[16:24])[0]


def shipped_bytes(cur, batch):
    """One untimed aggregate with the frame reader wrapped: total bytes shipped.

    ``Cursor.aggregate`` consumes one frame per request (NEXT + client reduce on
    the baseline, REDUCE + read_reduce on the change), so wrapping whichever
    reader this build has counts every frame without touching the timed region.
    """
    from fastpostgres import _client

    sizes = []

    if hasattr(_client, "read_reduce"):
        original = _client.read_reduce

        def wrapped(seg):
            value = original(seg)
            sizes.append(payload_size(seg))
            return value

        _client.read_reduce = wrapped
    else:
        original = _client.reduce

        def wrapped(seg, plan):
            value = original(seg, plan)
            sizes.append(payload_size(seg))
            return value

        _client.reduce = wrapped

    try:
        got = cur.aggregate(SQL, OPS, size=batch)
    finally:
        if hasattr(_client, "read_reduce"):
            _client.read_reduce = original
        else:
            _client.reduce = original
    return got, sum(sizes), max(sizes) if sizes else 0, len(sizes)


def setup():
    conn = fp.Connection(DATABASE_URL)
    try:
        conn.exec(f"DROP TABLE IF EXISTS {TABLE}")
        conn.exec(
            f"CREATE TABLE {TABLE} AS SELECT g::int8 AS id, (g % 1000)::int8 AS price "
            f"FROM generate_series(1, {ROWS}) g"
        )
        conn.exec(f"ANALYZE {TABLE}")
        want = conn.exec(VERIFY).rows[0]
    finally:
        conn.close()
    return want


def measure(batch):
    """One aggregate shape: a warm call, an untimed byte count, REPS timed."""
    conn = dbapi.connect(DATABASE_URL)
    try:
        cur = conn.cursor()
        cur.aggregate(SQL, OPS, size=batch)  # warm: ring bootstrap, plan cache
        got, total, biggest, frames = shipped_bytes(cur, batch)
        times = []
        for _ in range(REPS):
            t0 = now()
            cur.aggregate(SQL, OPS, size=batch)
            times.append((now() - t0) * 1000)
        cur.close()
        return statistics.median(times), min(times), max(times), got, total, biggest, frames
    finally:
        conn.close()


def main():
    want = setup()
    print(f"\nAGGREGATE (sum(int8) + count_all): {ROWS} rows through Cursor.aggregate")

    ok = True
    for batch in BATCHES:
        med, lo, hi, got, total, biggest, frames = measure(batch)
        rate = ROWS / (med / 1000) / 1e6
        same = got["rows"] == want[1] and got["price_sum"] == want[0]
        ok = ok and same
        spread = 0.0 if lo == hi else (hi - lo) / 2 / med * 100
        print(
            f"  {ROWS:>9} {batch:>9}  {med:8.2f} ms  {rate:7.2f}M rows/s  "
            f"{total:>10} B shipped in {frames:<3} frames (max {biggest:>8} B)  "
            f"±{spread:.1f}%  {'ok' if same else 'MISMATCH'}"
        )

    print(f"  result: price_sum={got['price_sum']} rows={got['rows']} "
          f"(server {want[0]}, {want[1]})")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
