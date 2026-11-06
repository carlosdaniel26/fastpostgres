#!/usr/bin/env python3
"""The storage-level columnar path, measured against the heap + frame path.

One command, two images:

  * the baseline image has the heap, the ring worker and the FPCOLUMN frame:
    every read is a server scan, a deform, a columnar frame built into /dev/shm
    and a client decode;
  * the change image adds ``Connection.columnar_load()``: the same rows are
    written once into a sealed memfd in the FPCOLUMN layout, mapped read-only by
    the client, and scanned where they lie.

So the script always runs the baseline players (they exist on both images) and,
when the bulk-load API exists, the columnar players.  Run it as

    python3 bench/jobs/columnar_scan.py [nrows]

and mount it into both containers; the baseline image predates the file, the
feature itself is what the A/B is about.  One process per run, one player at a
time.  Page faults are the client process's own ``getrusage`` deltas.
"""
import os
import resource
import statistics
import sys
import time

CONNINFO = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")
TABLE = "fp_bench_rows"
CORE_SQL = f"SELECT id, sku, name, price, stock, active FROM {TABLE}"
OPS = {
    "rows": ("count_all", None),
    "price_sum": ("sum", "price"),
    "stock_sum": ("sum", "stock"),
    "id_max": ("max", "id"),
}


def now():
    return time.perf_counter()


def faults():
    r = resource.getrusage(resource.RUSAGE_SELF)
    return r.ru_minflt, r.ru_majflt


def setup(n):
    """The benchmark's 100K shape, built by the harness's own psycopg path."""
    import psycopg

    with psycopg.connect(CONNINFO) as conn:
        with conn.cursor() as cur:
            cur.execute(f"DROP TABLE IF EXISTS {TABLE}")
            cur.execute(
                f"CREATE TABLE {TABLE} (id int4 PRIMARY KEY, sku text, name text, "
                f"price float8, stock int4, active bool)"
            )
            cur.execute(
                f"INSERT INTO {TABLE} (id, sku, name, price, stock, active) "
                f"SELECT g, 'SKU-' || g, 'Product ' || g, g * 0.5, g % 500, g % 2 = 0 "
                f"FROM generate_series(1, {int(n)}) g"
            )
            cur.execute(f"ANALYZE {TABLE}")
            cur.execute(f"SELECT pg_relation_size('{TABLE}')")
            heap_bytes = cur.fetchone()[0]
        conn.commit()
    return heap_bytes


def timed(fn, reps):
    """(seconds, minor faults, major faults) per run."""
    out = []
    for _ in range(reps):
        f0 = faults()
        t0 = now()
        value = fn()
        t1 = now()
        f1 = faults()
        out.append((t1 - t0, f1[0] - f0[0], f1[1] - f0[1], value))
    return out


def med(rows, i):
    return statistics.median(r[i] for r in rows)


def rows_per_s(rows, n):
    ms = med(rows, 0) * 1000.0
    return (n / med(rows, 0), ms, int(med(rows, 1)), int(med(rows, 2)))


def main(argv):
    n = int(argv[1]) if len(argv) > 1 else 100_000
    reps = 5

    heap_bytes = setup(n)
    print(f"### COLUMNAR SCAN rows={n} reps={reps}")
    print(f"heap.relation.bytes: {heap_bytes}")

    from fastpostgres import dbapi

    conn = dbapi.connect(CONNINFO)
    cur = conn.cursor()

    # Warm every player once, so the ring pair startup and the Python import
    # does not land inside the timed region.
    cur.aggregate(CORE_SQL, OPS, size=n)
    for _ in cur.stream(CORE_SQL, size=n):
        pass

    heap_agg = timed(lambda: cur.aggregate(CORE_SQL, OPS, size=n), reps)
    rate, ms, mn, mj = rows_per_s(heap_agg, n)
    print(f"heap.aggregate.rows_s: {rate:.0f}")
    print(f"heap.aggregate.ms: {ms:.3f}")
    print(f"heap.aggregate.faults: {mn} {mj}")
    want = heap_agg[0][3]

    def stream_scan():
        return sum(batch.row_count for batch in cur.stream(CORE_SQL, size=n))

    heap_stream = timed(stream_scan, reps)
    rate, ms, mn, mj = rows_per_s(heap_stream, n)
    print(f"heap.stream.rows_s: {rate:.0f}")
    print(f"heap.stream.ms: {ms:.3f}")
    print(f"heap.stream.faults: {mn} {mj}")

    if not hasattr(conn, "columnar_load"):
        print("columnar.path: unavailable (baseline image has no bulk-load API)")
        conn.close()
        return 0

    # The load is the one-time server pass: execute the SELECT, deform the
    # tuples, write every value into its column run, seal the memfd, map it.
    loads = timed(lambda: conn.columnar_load(CORE_SQL), reps)
    load_ms = med(loads, 0) * 1000.0
    table = loads[0][3]
    # Column bytes from the views' lengths: no data page is touched yet.
    data_bytes = sum(len(c.bytes) if c.bytes is not None else len(c.data) for c in table.columns)
    print(f"columnar.load.ms: {load_ms:.3f}")
    print(f"columnar.load.faults: {int(med(loads, 1))} {int(med(loads, 2))}")
    print(f"columnar.bytes: {data_bytes}")
    print(f"columnar.heap.x: {data_bytes / heap_bytes if heap_bytes else 0:.3f}")

    # The cold scan is the first touch of every data page; the warm ones reuse
    # the mapping's PTE entries and the in-page cache.
    coldf = timed(table.scan_all, 1)[0]
    cold = coldf[3]
    print(f"columnar.scan.cold.ms: {coldf[0] * 1000.0:.3f}")
    print(f"columnar.scan.cold.faults: {coldf[1]} {coldf[2]}")

    got = table.reduce(OPS)
    same = (
        got["rows"] == want["rows"]
        and got["stock_sum"] == want["stock_sum"]
        and got["id_max"] == want["id_max"]
        and abs(got["price_sum"] - want["price_sum"]) < 1e-9
    )
    print(f"verify.reduce: {'ok' if same else 'MISMATCH ' + repr((got, want))}")

    scans = timed(table.scan_all, reps)
    rate, ms, mn, mj = rows_per_s(scans, n)
    print(f"columnar.scan.rows_s: {rate:.0f}")
    print(f"columnar.scan.ms: {ms:.3f}")
    print(f"columnar.scan.faults: {mn} {mj}")
    print(f"columnar.scan.bytes_s: {data_bytes / med(scans, 0):.0f}")

    reduces = timed(lambda: table.reduce(OPS), reps)
    rate, ms, mn, mj = rows_per_s(reduces, n)
    print(f"columnar.reduce.rows_s: {rate:.0f}")
    print(f"columnar.reduce.ms: {ms:.3f}")
    print(f"columnar.reduce.faults: {mn} {mj}")

    print(f"columnar.scan.checksum: {cold[2]}")
    conn.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
