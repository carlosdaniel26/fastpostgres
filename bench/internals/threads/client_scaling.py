"""The client's own scaling, on the paths a result is actually consumed by.

Runs inside the image, against the entrypoint's PostgreSQL. One table, five
settings, each with N workers and one ring (one partition of the shared memory)
per worker:

  serial     one connection, back to back: the baseline
  columnar   N threads, stream() batches reduced with numpy — the array path
  threads    N threads, Cursor.aggregate: stream + sum/count inside the
             extension, no Python object per row (the C++ reduce)
  asyncio    N AsyncConnections on one event loop, await aggregate — the loop
             stays free while each connection's tier thread drives its own ring
  processes  one process per worker, each with its own rings and its own GIL

Python-tuple materialization (`execute` + `fetchall`) is deliberately not
measured: it is the DB-API compatibility path, not how a result is consumed in a
hot path, and it is GIL-bound, so it competes on ergonomics rather than
throughput. The historical numbers are in `bench/NOTES.md`, where they explain
why the free-threaded build exists.

The unit is one read of `--rows` rows, reduced either by numpy or by the C++
aggregate; each worker runs `--reads` of them and the timed region is the
workers' own loops. Every worker must have counted the whole table and every
worker's sum must agree, and every aggregate read must have been served by the
ring (`libpq_reads=0`): a silent fallback would show up in the `shm` column.

Usage: python3 bench/internals/threads/client_scaling.py [--rows 100000]
           [--reads 5] [--max-workers 6]
           [--mode threads|asyncio|columnar|processes|serial|all]
"""
import argparse
import asyncio
import json
import multiprocessing
import os
import threading
import time
from pathlib import Path

import numpy as np

from fastpostgres import aio, dbapi

CONNINFO = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")

TABLE = "fp_scaling_t"
BENCH_ROW = (
    "g::int4 AS id, ('SKU-' || g)::text AS sku, ('Product ' || g)::text AS name, "
    "(g * 0.5)::float8 AS price, (g % 500)::int4 AS stock, (g % 2 = 0)::bool AS active"
)
# The reduction every aggregate-mode run applies: whole-table count and sum.
PLAN = {"n": ("count_all", None), "s": ("sum", "price")}


def build_table(rows):
    """The benchmark's six-column row, in a table: the reads must cost the
    transport and the consumption, not the executor's per-row work — the same
    table shape the rival benchmark reads, so these rates sit next to NOTES.md's."""
    conn = dbapi.connect(CONNINFO)
    try:
        cur = conn.cursor()
        cur.execute(f"DROP TABLE IF EXISTS {TABLE}")
        cur.execute(f"CREATE TABLE {TABLE} AS SELECT {BENCH_ROW} FROM generate_series(1, {rows}) g")
        cur.execute(f"ANALYZE {TABLE}")
    finally:
        conn.close()


def drop_table():
    conn = dbapi.connect(CONNINFO)
    try:
        conn.cursor().execute(f"DROP TABLE IF EXISTS {TABLE}")
    finally:
        conn.close()


def reduce_reads(cur, sql, reads, size):
    """`reads` C++ reductions on one cursor: (rows counted, last sum)."""
    total = 0
    last = None
    for _ in range(reads):
        got = cur.aggregate(sql, PLAN, size=size)
        total += got["n"]
        last = got["s"]
    return total, last


def check(what, total, last, served, reads, rows):
    """Every worker must have counted the whole table, on the ring."""
    if total != reads * rows:
        raise SystemExit(f"{what}: counted {total} rows, expected {reads * rows}")
    if served is not None and served != reads:
        raise SystemExit(f"{what}: {served}/{reads} reads served by the ring (libpq fallback?)")
    if last is None:
        raise SystemExit(f"{what}: no batch arrived")


def measure_serial(sql, reads, rows, size):
    conn = dbapi.connect(CONNINFO)
    try:
        cur = conn.cursor()
        before = conn.shm_reads
        start = time.perf_counter()
        total, last = reduce_reads(cur, sql, reads, size)
        elapsed = time.perf_counter() - start
        served = conn.shm_reads - before
    finally:
        conn.close()
    check("serial", total, last, served, reads, rows)
    return elapsed, last


def _threaded(what, workers, reads, rows, reduce_one):
    """`workers` threads, one connection each; `reduce_one(conn)` does the reads."""
    barrier = threading.Barrier(workers + 1)
    results = [None] * workers
    errors = []

    def work(i):
        try:
            conn = dbapi.connect(CONNINFO)
            try:
                barrier.wait()  # one start gun; the timing happens inside the worker
                results[i] = reduce_one(conn)
            finally:
                conn.close()
        except BaseException as exc:
            errors.append(f"{type(exc).__name__}: {exc}")
            try:
                barrier.abort()
            except Exception:
                pass

    threads = [threading.Thread(target=work, args=(i,)) for i in range(workers)]
    for t in threads:
        t.start()
    barrier.wait()
    for t in threads:
        t.join()

    if errors:
        raise SystemExit(f"{what}: " + "; ".join(errors))
    for i, (total, last, served, _elapsed) in enumerate(results):
        check(f"{what} worker {i}", total, last, served, reads, rows)
    sums = {round(r[1], 3) for r in results}
    if len(sums) != 1:
        raise SystemExit(f"{what}: the workers' sums disagree: {sorted(sums)}")
    return max(r[3] for r in results)


def aggregate_worker(sql, reads, size):
    """The timed body of a worker: (rows, sum, shm reads, seconds)."""

    def run(conn):
        cur = conn.cursor()
        before = conn.shm_reads
        start = time.perf_counter()
        total, last = reduce_reads(cur, sql, reads, size)
        return total, last, conn.shm_reads - before, time.perf_counter() - start

    return run


def measure_threads(sql, workers, reads, rows, size):
    return _threaded("threads", workers, reads, rows, aggregate_worker(sql, reads, size))


def measure_columnar(sql, workers, reads, rows, size):
    """The array path: stream() + a numpy reduction, whose loops release the GIL."""

    def run(conn):
        cur = conn.cursor()
        start = time.perf_counter()
        total = 0
        last = None
        for _ in range(reads):
            for batch in cur.stream(sql, size=size):
                prices = np.asarray(batch.columns[3])  # the float8 column, no NULLs
                total += prices.size
                last = float(prices.sum())
        return total, last, None, time.perf_counter() - start

    return _threaded("columnar", workers, reads, rows, run)


async def measure_asyncio(sql, workers, reads, rows, size):
    conns = [await aio.connect(CONNINFO) for _ in range(workers)]
    ready = asyncio.Barrier(workers + 1)
    results = [None] * workers

    async def work(i):
        try:
            cur = conns[i].cursor()
            await ready.wait()
            before = conns[i].stats()["shm_reads"]
            start = time.perf_counter()
            total = 0
            last = None
            for _ in range(reads):
                got = await cur.aggregate(sql, PLAN, size=size)
                total += got["n"]
                last = got["s"]
            results[i] = (total, last, conns[i].stats()["shm_reads"] - before,
                          time.perf_counter() - start)
        except BaseException:
            ready.abort()
            raise

    tasks = [asyncio.create_task(work(i)) for i in range(workers)]
    try:
        await ready.wait()
        await asyncio.gather(*tasks)
    finally:
        for conn in conns:
            await conn.close()

    for i, (total, last, served, _elapsed) in enumerate(results):
        check(f"asyncio worker {i}", total, last, served, reads, rows)
    sums = {round(r[1], 3) for r in results}
    if len(sums) != 1:
        raise SystemExit(f"asyncio: the workers' sums disagree: {sorted(sums)}")
    return max(r[3] for r in results)


def _process_worker(sql, reads, rows, size, out, barrier):
    """One worker process: its own connection, its own rings, its own GIL."""
    conn = None
    try:
        conn = dbapi.connect(CONNINFO)
        cur = conn.cursor()
        barrier.wait()
        before = conn.shm_reads
        start = time.perf_counter()
        total, last = reduce_reads(cur, sql, reads, size)
        out.put((total, last, conn.shm_reads - before, time.perf_counter() - start, None))
    except BaseException as exc:
        out.put((0, None, 0, None, f"{type(exc).__name__}: {exc}"))
        try:
            barrier.abort()
        except Exception:
            pass
    finally:
        if conn is not None:
            try:
                conn.close()
            except Exception:
                pass


def measure_processes(sql, workers, reads, rows, size):
    """One process per worker: separate GILs, separate pools, separate rings.

    Each child registers its own background workers, so this setting spends
    `workers` of the server's `max_worker_processes` budget on top of whatever
    this process already holds — the wait below lets the previous setting's
    workers exit first, and the cell runs this mode before its own pool fills.
    """
    wait_for_slots(workers)
    barrier = multiprocessing.Barrier(workers)
    out = multiprocessing.Queue()
    procs = [
        multiprocessing.Process(target=_process_worker, args=(sql, reads, rows, size, out, barrier))
        for _ in range(workers)
    ]
    for p in procs:
        p.start()
    for p in procs:
        p.join(timeout=180)
        if p.exitcode != 0:
            raise SystemExit(f"processes: a worker exited with {p.exitcode}")

    results = [out.get(timeout=10) for _ in procs]
    for i, (total, last, served, _elapsed, error) in enumerate(results):
        if error:
            raise SystemExit(f"processes worker {i}: {error}")
        check(f"processes worker {i}", total, last, served, reads, rows)
    sums = {round(r[1], 3) for r in results}
    if len(sums) != 1:
        raise SystemExit(f"processes: the workers' sums disagree: {sorted(sums)}")
    return max(r[3] for r in results)


def ring_workers():
    """How many ring workers the server has registered, or None if it cannot say."""
    conn = dbapi.connect(CONNINFO)
    try:
        cur = conn.cursor()
        cur.execute(f"SELECT count(*) FROM pg_stat_activity WHERE backend_type = '{dbapi._RING_BACKEND}'")
        return int(cur.fetchall()[0][0])
    except Exception:
        return None
    finally:
        conn.close()


def worker_budget():
    """What the server can register at once, less the one it holds back."""
    conn = dbapi.connect(CONNINFO)
    try:
        cur = conn.cursor()
        cur.execute("SHOW max_worker_processes")
        return int(cur.fetchall()[0][0]) - 1
    except Exception:
        return None
    finally:
        conn.close()


def wait_for_slots(need, timeout=20.0):
    """Wait until `need` more ring workers can register.

    A finished setting's workers take a poll (100 ms) to notice their client is
    gone, and the server budgets them (`max_worker_processes`): forking a new
    setting before the last one's workers exited finds no slots, falls back to
    libpq, and the run fails — correctly, but for the wrong reason.
    """
    budget = worker_budget()
    if budget is None:
        return
    deadline = time.monotonic() + timeout
    while True:
        taken = ring_workers()
        if taken is None or taken + need <= budget or time.monotonic() >= deadline:
            return
        time.sleep(0.05)


def cpu_model():
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return "unknown"


def cpu_ghz():
    try:
        with open("/proc/cpuinfo") as f:
            values = [float(line.split(":", 1)[1]) for line in f if line.startswith("cpu MHz")]
        return round(max(values) / 1000, 1) if values else "unknown"
    except (OSError, ValueError):
        return "unknown"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rows", type=int, default=100000, help="rows per read (default: %(default)s)")
    ap.add_argument("--reads", type=int, default=5, help="reads per worker (default: %(default)s)")
    ap.add_argument("--reps", type=int, default=3, help="runs per setting; the median is reported")
    ap.add_argument("--max-workers", type=int, default=6, help="highest worker count to try")
    ap.add_argument("--mode", choices=("threads", "asyncio", "columnar", "processes", "serial", "all"),
                    default="all")
    args = ap.parse_args()

    sql = f"SELECT id, sku, name, price, stock, active FROM {TABLE}"
    batch = max(1000, args.rows // 4)

    print("### CLIENT-SCALING")
    print(f"cpu: {cpu_model()}")
    print(f"ghz: {cpu_ghz()}")
    print(f"shape: bench ({TABLE}: the benchmark's six-column row, {args.rows} rows)")
    print(f"rows: {args.rows}")
    print(f"reads: {args.reads} per worker")
    print(f"reps: {args.reps} (median reported)")
    print(f"mode: {args.mode}")
    print(f"pool: {os.environ.get('FASTPOSTGRES_SHM_POOL', '<machine default>')}")
    print(f"git: {os.environ.get('BENCH_GIT_REV', 'unknown')}")
    print("### END CLIENT-SCALING")
    print()

    def median_elapsed(run, reps=args.reps):
        """The median of `reps` timed runs — one setting at a time, never mixed."""
        return sorted(run() for _ in range(reps))[reps // 2]

    build_table(args.rows)
    try:
        measure_serial(sql, 1, args.rows, batch)  # warm: the first read creates the ring
        base, _sum = median_elapsed(lambda: measure_serial(sql, args.reads, args.rows, batch))
        serial_rate = args.reads * args.rows / base

        print(f"{'mode':<9} {'n':>2} {'reads':>6} {'rows/s':>12} {'vs n=1':>8} {'per worker':>12} {'shm':>9}")
        print(f"{'serial':<9} {1:>2} {args.reads:>6} {serial_rate:>12,.0f} {'—':>8} "
              f"{serial_rate:>12,.0f} {args.reads}/{args.reads:<7}")

        rates = {}

        def report(mode, workers, rate, shm):
            baseline = rates.setdefault((mode, 1), rate)
            shown = f"{rate / baseline - 1:+.0%}" if workers > 1 else "—"
            print(f"{mode:<9} {workers:>2} {workers * args.reads:>6} {rate:>12,.0f} {shown:>8} "
                  f"{rate / workers:>12,.0f} {shm:>9}")

        # Processes first: every child registers its own background workers, and
        # this process's rings would otherwise be holding the server's slots.
        if args.mode in ("processes", "all"):
            for workers in range(1, args.max_workers + 1):
                measure_processes(sql, workers, 1, args.rows, batch)  # warm
                rate = (
                    workers * args.reads * args.rows
                    / median_elapsed(
                        lambda workers=workers: measure_processes(sql, workers, args.reads, args.rows, batch)
                    )
                )
                report("processes", workers, rate, f"{workers * args.reads}/{workers * args.reads}")

        def runners(mode, workers):
            """The warm-up and the timed run for one setting."""
            if mode == "threads":
                return (lambda: measure_threads(sql, workers, 1, args.rows, batch),
                        lambda: measure_threads(sql, workers, args.reads, args.rows, batch))
            if mode == "asyncio":
                return (lambda: asyncio.run(measure_asyncio(sql, workers, 1, args.rows, batch)),
                        lambda: asyncio.run(measure_asyncio(sql, workers, args.reads, args.rows, batch)))
            return (lambda: measure_columnar(sql, workers, 1, args.rows, batch),
                    lambda: measure_columnar(sql, workers, args.reads, args.rows, batch))

        modes = [m for m in ("columnar", "threads", "asyncio") if args.mode in (m, "all")]
        # The modes alternate at each worker count, not block after block: a
        # setting measured twenty runs later sees a different machine (clock,
        # page state), and that drift is bigger than what the change buys.
        for workers in range(1, args.max_workers + 1):
            for mode in modes:
                warm, timed = runners(mode, workers)
                warm()  # the rings this setting will use
                rate = workers * args.reads * args.rows / median_elapsed(timed)
                # a streamed read is not counted by shm_reads; the columnar mode's
                # correctness check is the sum every worker must agree on
                shm = "—" if mode == "columnar" else f"{workers * args.reads}/{workers * args.reads}"
                report(mode, workers, rate, shm)
    finally:
        drop_table()

    print()
    stats = dbapi.pool_stats()
    print(f"pool: created={stats['created']} idle={stats['idle']} leases={len(stats['leases'])} "
          f"broken={stats['broken']} libpq_reads={stats['libpq_reads']} causes={json.dumps(stats['causes'])}")
    names = sorted(p.name for p in Path("/dev/shm").glob(f"fp_dbapi_{os.getpid()}_*"))
    print(f"partitions: {len([n for n in names if not n.endswith(('_seg', '_seg_a', '_seg_b'))])} rings, "
          f"{len([n for n in names if n.endswith(('_seg', '_seg_a', '_seg_b'))])} data segments")


if __name__ == "__main__":
    main()
