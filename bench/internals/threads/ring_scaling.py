"""Aggregate throughput of N concurrent reads, one ring each, in N processes.

The unit is one *object-free* read: `Cursor.aggregate` pulls columnar batches
over the ring and reduces them in C++, so no Python object is built per row and
the client decode does not mask what the transport and the executor are doing.
Each worker process does `--reads` of `--rows` rows; the aggregate rate is
(served x reads x rows) / the slowest successful worker. A worker that cannot
get a ring at all is not counted as a reader: `served` says how many actually
ran, which is the point of an N-scaling run.

This is the A/B driver for decoupling the ring count from
`max_worker_processes`: on the baseline one ring is one background worker, so
past the server's worker budget the remaining processes have no ring; with the
eventfd pool one worker serves several rings.

Usage (inside the image, after the entrypoint started PostgreSQL):

    python3 bench/internals/threads/ring_scaling.py --rows 100000 --reads 3
"""
import argparse
import multiprocessing
import os
import queue
import statistics
import time

from fastpostgres import dbapi

CONNINFO = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")
TABLE = "fp_ring_scaling_t"
BENCH_ROW = (
    "g::int4 AS id, ('SKU-' || g)::text AS sku, ('Product ' || g)::text AS name, "
    "(g * 0.5)::float8 AS price, (g % 500)::int4 AS stock, (g % 2 = 0)::bool AS active"
)
SQL = f"SELECT id, sku, name, price, stock, active FROM {TABLE}"
PLAN = {"n": ("count_all", None), "s": ("sum", "price")}


def build_table(rows):
    """The benchmark's shape, built over libpq: the parent must not hold a ring."""
    conn = dbapi.connect(CONNINFO, shm=False)
    try:
        cur = conn.cursor()
        cur.execute(f"DROP TABLE IF EXISTS {TABLE}")
        cur.execute(f"CREATE TABLE {TABLE} AS SELECT {BENCH_ROW} FROM generate_series(1, {rows}) g")
        cur.execute(f"ANALYZE {TABLE}")
    finally:
        conn.close()


def drop_table():
    conn = dbapi.connect(CONNINFO, shm=False)
    try:
        conn.cursor().execute(f"DROP TABLE IF EXISTS {TABLE}")
    finally:
        conn.close()


def ring_workers():
    conn = dbapi.connect(CONNINFO, shm=False)
    try:
        return int(
            conn._conn.exec(
                "SELECT count(*) FROM pg_stat_activity "
                "WHERE backend_type = 'fastpostgres ring worker'"
            ).rows[0][0]
        )
    finally:
        conn.close()


def child(sql, reads, size, ready, go, done, out):
    """One reader: warm its ring, say it is ready, wait for the gun, read.

    Events rather than a Barrier: on the baseline one of N readers may have no
    ring at all, and that must not break the others' start (the point of the
    measurement is what N-1 readers can do). A failed reader reports and exits.
    """
    conn = None
    try:
        conn = dbapi.connect(CONNINFO)
        cur = conn.cursor()
        cur.aggregate(sql, PLAN, size=size)  # warm: creates this process's ring
        before = conn.shm_reads
        ready.put(1)
        if not go.wait(timeout=120):
            raise TimeoutError("the start gun never fired")
        t0 = time.perf_counter()
        total = 0
        last = None
        for _ in range(reads):
            got = cur.aggregate(sql, PLAN, size=size)
            total += got["n"]
            last = got["s"]
        elapsed = time.perf_counter() - t0
        out.put(
            {
                "ok": True,
                "total": total,
                "sum": last,
                "elapsed": elapsed,
                "served": conn.shm_reads - before,
                "pool": dbapi.pool_size(),
            }
        )
        done.wait(timeout=120)  # hold the ring so the worker count is meaningful
    except BaseException as exc:
        out.put({"ok": False, "error": f"{type(exc).__name__}: {exc}", "elapsed": None, "served": 0})
    finally:
        if conn is not None:
            try:
                conn.close()
            except Exception:
                pass


def wait_for_workers(target=0, timeout=20.0):
    """A finished setting's workers take a tick to notice their clients are gone
    (the eventfd pool keeps an empty worker for two seconds). Starting the next
    N on top of them would count a previous setting's worker, so wait."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if ring_workers() <= target:
            return
        time.sleep(0.05)


def one_run(n, rows, reads, size):
    """Spawn n readers, release them together, return (rate, served, workers, errors)."""
    wait_for_workers(0)
    ready = multiprocessing.Queue()
    go = multiprocessing.Event()
    done = multiprocessing.Event()
    out = multiprocessing.Queue()
    procs = [
        multiprocessing.Process(target=child, args=(SQL, reads, size, ready, go, done, out))
        for _ in range(n)
    ]
    for p in procs:
        p.start()

    # Let every reader that can create its ring say so; a reader with no ring on
    # the baseline never does, and the others still start after the deadline.
    for _ in range(n):
        try:
            ready.get(timeout=5.0)
        except queue.Empty:
            break
    go.set()

    results = [out.get(timeout=180) for _ in procs]
    workers = ring_workers()  # while the readers (and their rings) are still alive
    done.set()
    for p in procs:
        p.join(timeout=60)

    good = [r for r in results if r["ok"]]
    bad = [r for r in results if not r["ok"]]
    if not good:
        raise SystemExit(f"n={n}: every reader failed: {[r['error'] for r in bad]}")

    elapsed = max(r["elapsed"] for r in good)
    served = sum(1 for r in good if r["served"] >= reads)
    rate = served * reads * rows / elapsed
    sums = {round(r["sum"], 3) for r in good}
    if len(sums) != 1:
        raise SystemExit(f"n={n}: the readers' sums disagree: {sorted(sums)}")
    return rate, served, workers, [r["error"] for r in bad]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rows", type=int, default=100000)
    ap.add_argument("--reads", type=int, default=3)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--batch", type=int, default=25000)
    ap.add_argument("--max-workers", type=int, default=8)
    args = ap.parse_args()

    print("### RING-SCALING")
    print(f"cpu: {os.cpu_count()}")
    print(f"rows: {args.rows}")
    print(f"reads: {args.reads} per process")
    print(f"reps: {args.reps} (median reported)")
    print(f"batch: {args.batch}")
    print(f"shape: {TABLE} (the benchmark's six-column row)")
    print(f"transport: {os.environ.get('FASTPOSTGRES_TRANSPORT', 'shm')}")
    print(f"rings_per_worker: {os.environ.get('FASTPOSTGRES_RINGS_PER_WORKER', '<default>')}")
    print(f"shm_pool: {os.environ.get('FASTPOSTGRES_SHM_POOL', '<default>')}")
    print(f"git: {os.environ.get('BENCH_GIT_REV', 'unknown')}")
    print("### END RING-SCALING")
    print()

    build_table(args.rows)
    try:
        print(f"{'n':>2} {'served':>6} {'ring workers':>12} {'aggregate rows/s':>18} {'per reader':>12}")
        base = None
        last_rate = None
        ns = [n for n in (1, 2, 4, 8) if n <= args.max_workers]
        for n in ns:
            rates, served, workers, errors = [], 0, 0, []
            for _ in range(args.reps):
                rate, served, workers, errors = one_run(n, args.rows, args.reads, args.batch)
                rates.append(rate)
            rate = statistics.median(rates)
            last_rate = rate
            if base is None:
                base = rate
            note = f" ({len(errors)} reader(s) had no ring)" if errors else ""
            print(f"{n:>2} {served:>6} {workers:>12} {rate:>18,.0f} {rate / max(1, served):>12,.0f}{note}")
        if base and last_rate:
            print(f"# n={ns[-1]} vs n=1: {last_rate / base - 1:+.0%}")
    finally:
        drop_table()


if __name__ == "__main__":
    main()
