#!/usr/bin/env python3
"""The worker's turn at a small batch, where its per-batch mapping cost shows.

`decode_driver.py` times a whole pull (worker + client decode). This tool times
the two halves apart, because the mapping cache under test is worker-side only:

    worker  ring.next_rows(batch)   the executor, the segment map, the row write
    client  _client.read_rows(SEG)  the Python decode out of the same segment

At 100k rows per fetch the fixed worker cost is ~0.25 ns/row and invisible; at
3k rows it is ~8 ns/row of the worker's 44, which is the whole point. The same
command therefore runs both batch sizes: 3k for the change, 100k as a control.

One stream, one segment, fully synchronous (no pipelining): the worker's turn is
on the critical path, which is what a small interactive read sees. The rows are
a table, not a generate_series: the executor must cost what it costs in the
benchmark, or its own per-row work hides the fixed mapping cost again.

Usage: python3 bench/dispatch/worker-map-batch.py [rows] [batches]
       (defaults: 3000 100)

The image entrypoint prints the `### ENV` block; this prints one `map-batch:`
result line per size that a results log can quote verbatim.
"""
import argparse
import os
import statistics
import sys
import time

import fastpostgres as fp
import psycopg
from fastpostgres import _client

CONNINFO = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")

# One ring per process: a fixed name would collide when runs share a container.
STEM = f"fp_workermap_{os.getpid()}"
SEG = f"{STEM}_stream"
TABLE = "fp_workermap_rows"
QUERY = f"SELECT id, sku, name, price, stock, active FROM {TABLE}"


def positive(text):
    n = int(text)
    if n <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return n


def setup(n):
    """The benchmark's row shape in a table, so the read pays the executor's
    real per-row cost and the worker's fixed per-batch cost is what is left."""
    with psycopg.connect(CONNINFO, autocommit=True) as conn:
        conn.execute(f"DROP TABLE IF EXISTS {TABLE}")
        conn.execute(
            f"CREATE TABLE {TABLE} AS "
            "SELECT g::int4 AS id, ('SKU-' || g)::text AS sku, "
            "('Product ' || g)::text AS name, (g * 0.5)::float8 AS price, "
            "(g % 500)::int4 AS stock, (g % 2 = 0)::bool AS active "
            f"FROM generate_series(1, {n}) g"
        )


def worker_pid(conn, before):
    """The ring worker this run just started (the process the ring names)."""
    with psycopg.connect(CONNINFO, autocommit=True) as admin:
        after = {
            row[0]
            for row in admin.execute(
                "SELECT pid FROM pg_stat_activity WHERE backend_type = 'fastpostgres ring worker'"
            ).fetchall()
        }
    new = sorted(after - before)
    return new[0] if len(new) == 1 else None


def cpu_ns(pid):
    """On-CPU nanoseconds for a process, from /proc/<pid>/schedstat."""
    if pid is None:
        return None
    with open(f"/proc/{pid}/schedstat") as f:
        return int(f.read().split()[0])


def measure(ring, rows, batches, pid=None):
    """Worker and client time per batch, the first pull a warmup."""
    warmup = min(rows, 1000)
    ring.next_rows(warmup)
    _client.read_rows(SEG)

    worker = []
    client = []
    cpu_before = cpu_ns(pid)
    for _ in range(batches):
        t0 = time.perf_counter_ns()
        n = ring.next_rows(rows)
        t1 = time.perf_counter_ns()
        chunk = _client.read_rows(SEG)
        t2 = time.perf_counter_ns()
        if n != rows or len(chunk.rows) != rows:
            raise SystemExit(f"worker-map-batch: asked {rows}, worker sent {n}, decoded {len(chunk.rows)}")
        worker.append((t1 - t0) / rows)
        client.append((t2 - t1) / rows)
    cpu_after = cpu_ns(pid)

    w = statistics.median(worker)
    c = statistics.median(client)
    cpu = None if cpu_before is None else (cpu_after - cpu_before) / (rows * batches)
    return w, c, w + c, cpu


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("rows", nargs="?", type=positive, default=3000,
                    help="rows per batch (default: %(default)s)")
    ap.add_argument("batches", nargs="?", type=positive, default=100,
                    help="measured batches (default: %(default)s)")
    ap.add_argument("--rows2", type=positive, default=100000,
                    help="control batch size, larger so the fixed cost is tiny (default: %(default)s)")
    ap.add_argument("--batches2", type=positive, default=5,
                    help="control batches (default: %(default)s)")
    args = ap.parse_args()

    # enough rows for the larger of the two streams, warmup included
    need = max(min(args.rows, 1000) + args.rows * args.batches,
               min(args.rows2, 1000) + args.rows2 * args.batches2)
    setup(need + 1000)

    conn = fp.Connection(CONNINFO)
    with psycopg.connect(CONNINFO, autocommit=True) as admin:
        before = {
            row[0]
            for row in admin.execute(
                "SELECT pid FROM pg_stat_activity WHERE backend_type = 'fastpostgres ring worker'"
            ).fetchall()
        }
    ring = fp.Ring(STEM)
    try:
        ring.start(conn)
        pid = worker_pid(conn, before)
        for rows, batches, tag in ((args.rows, args.batches, "small"),
                                   (args.rows2, args.batches2, "control")):
            ring.open(SEG, QUERY)
            try:
                w, c, t, cpu = measure(ring, rows, batches, pid)
            finally:
                ring.close()
            print(f"### MAP-BATCH {tag}")
            print(f"rows: {rows} batches: {batches}")
            print(f"worker: {w:.1f} ns/row  ({1e9 / w / 1e6:.1f}M rows/s)")
            if cpu is not None:
                print(f"worker_cpu: {cpu:.1f} ns/row  ({1e9 / cpu / 1e6:.1f}M rows/s)")
            print(f"client: {c:.1f} ns/row  ({1e9 / c / 1e6:.1f}M rows/s)")
            print(f"total:  {t:.1f} ns/row  ({1e9 / t / 1e6:.2f}M rows/s)")
            print("### END MAP-BATCH")
            print(f"map-batch: size={tag} rows={rows} batches={batches} "
                  f"worker_ns_per_row={w:.2f} worker_rows_per_s={1e9 / w / 1e6:.2f}M "
                  f"worker_cpu_ns_per_row={'n/a' if cpu is None else f'{cpu:.2f}'} "
                  f"client_ns_per_row={c:.2f} total_ns_per_row={t:.2f} "
                  f"total_rows_per_s={1e9 / t / 1e6:.2f}M")
    finally:
        ring.stop()
        conn.close()


if __name__ == "__main__":
    sys.exit(main())
