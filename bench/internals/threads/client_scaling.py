"""The client's own scaling: threads, asyncio and processes, one partition per worker.

Runs inside the image, against the entrypoint's PostgreSQL. It measures only
this client — the same table read, driven five ways:

  serial    one connection, the reads back to back: the baseline
  threads   one connection and one thread per worker, the dbapi path; the pool
            hands each thread its own ring (its own partition of shared memory)
  asyncio   one AsyncConnection per task on one event loop; each connection's
            tier thread drives its own ring, so the loop is never blocked
  columnar  the same threads over the zero-copy path: stream() batches reduced
            with numpy, whose loops release the GIL — this is where N threads on
            N partitions run in parallel instead of queueing on the decode
  processes one process per worker, each with its own rings and its own GIL

The unit is a read: execute + fetchall of `--rows` rows for the row paths, a
streamed reduction for the columnar one. Each worker runs `--reads` of them, and
the timed region is the workers' own loops — the connections are made, warmed and
closed outside it. Every worker's row count, and every columnar worker's sum,
must match what the table promises, and the row paths' `shm` column must account
for every read: a silent libpq fallback would show up there first.

Usage: python3 bench/internals/threads/client_scaling.py [--rows 100000]
           [--reads 5] [--max-workers 6] [--mode threads|asyncio|columnar|processes|serial|all]
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


def build_table(rows):
    """The benchmark's six-column row, in a table: the reads must cost the
    transport and the decode, not the executor's per-row work — the same table
    shape the rival benchmark reads, so these rates sit next to NOTES.md's."""
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


def read_all(cur, sql, reads):
    """`reads` sequential reads on one cursor: (rows, first id, last id)."""
    total = 0
    first = last = None
    for _ in range(reads):
        cur.execute(sql)
        rows = cur.fetchall()
        total += len(rows)
        if rows:
            first = rows[0][0]
            last = rows[-1][0]
    return total, first, last


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


def check(what, total, first, last, served, reads, rows):
    """Every worker must have read the whole table, on the ring."""
    if total != reads * rows:
        raise SystemExit(f"{what}: read {total} rows, expected {reads * rows}")
    if (first, last) != (1, rows):
        raise SystemExit(f"{what}: read id {first}..{last}, expected 1..{rows}")
    if served is not None and served != reads:
        raise SystemExit(f"{what}: {served}/{reads} reads served by the ring (libpq fallback?)")


def measure_serial(sql, reads, rows):
    conn = dbapi.connect(CONNINFO)
    try:
        cur = conn.cursor()
        before = conn.shm_reads
        start = time.perf_counter()
        total, first, last = read_all(cur, sql, reads)
        elapsed = time.perf_counter() - start
        served = conn.shm_reads - before
    finally:
        conn.close()
    check("serial", total, first, last, served, reads, rows)
    return elapsed


def measure_threads(sql, workers, reads, rows):
    barrier = threading.Barrier(workers + 1)
    results = [None] * workers
    errors = []

    def work(i):
        try:
            conn = dbapi.connect(CONNINFO)
            try:
                cur = conn.cursor()
                barrier.wait()
                # timed inside the worker: the barrier's release races a start
                # stamp the launcher takes, and one read slipping out of the
                # window is 20% of a five-read setting
                before = conn.shm_reads
                start = time.perf_counter()
                total, first, last = read_all(cur, sql, reads)
                elapsed = time.perf_counter() - start
                results[i] = (total, first, last, conn.shm_reads - before, elapsed)
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
    barrier.wait()  # all workers up: one start gun for the timed region
    for t in threads:
        t.join()

    if errors:
        raise SystemExit("threads: " + "; ".join(errors))
    for i, (total, first, last, served, _elapsed) in enumerate(results):
        check(f"threads n={workers} worker {i}", total, first, last, served, reads, rows)
    return max(r[4] for r in results)  # the slowest worker: the run's wall time


async def measure_asyncio(sql, workers, reads, rows):
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
            first = last = None
            for _ in range(reads):
                await cur.execute(sql)
                got = await cur.fetchall()
                total += len(got)
                if got:
                    first = got[0][0]
                    last = got[-1][0]
            elapsed = time.perf_counter() - start
            results[i] = (total, first, last, conns[i].stats()["shm_reads"] - before, elapsed)
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

    for i, (total, first, last, served, _elapsed) in enumerate(results):
        check(f"asyncio n={workers} worker {i}", total, first, last, served, reads, rows)
    return max(r[4] for r in results)


def measure_columnar(sql, workers, reads, rows, size):
    """The zero-copy path: stream() batches reduced with numpy, which releases the GIL.

    Same N-partitions shape as the thread mode; only the consumption changes.
    Python-object decode queues on the GIL, a numpy loop does not, so this is the
    row that shows the partitions themselves scale.
    """
    barrier = threading.Barrier(workers + 1)
    results = [None] * workers
    errors = []

    def work(i):
        try:
            conn = dbapi.connect(CONNINFO)
            try:
                cur = conn.cursor()
                barrier.wait()
                start = time.perf_counter()
                total = 0
                last_sum = None
                for _ in range(reads):
                    for batch in cur.stream(sql, size=size):
                        prices = np.asarray(batch.columns[3])  # the float8 column, no NULLs
                        total += prices.size
                        last_sum = float(prices.sum())
                results[i] = (total, last_sum, time.perf_counter() - start)
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
    barrier.wait()  # all workers up: one start gun for the timed region
    for t in threads:
        t.join()

    if errors:
        raise SystemExit("columnar: " + "; ".join(errors))
    for i, (total, last_sum, _elapsed) in enumerate(results):
        if total != reads * rows:
            raise SystemExit(f"columnar worker {i}: reduced {total} rows, expected {reads * rows}")
        if last_sum is None:
            raise SystemExit(f"columnar worker {i}: no batch arrived")
    sums = {round(r[1], 3) for r in results}
    if len(sums) != 1:
        raise SystemExit(f"columnar: the workers' sums disagree: {sorted(sums)}")
    return max(r[2] for r in results)


def _process_worker(sql, reads, rows, out, barrier):
    """One worker process: its own connection, its own rings, its own GIL."""
    conn = None
    try:
        conn = dbapi.connect(CONNINFO)
        cur = conn.cursor()
        barrier.wait()
        before = conn.shm_reads
        start = time.perf_counter()
        total, first, last = read_all(cur, sql, reads)
        elapsed = time.perf_counter() - start
        out.put((total, first, last, conn.shm_reads - before, elapsed, None))
    except BaseException as exc:
        out.put((0, None, None, 0, None, f"{type(exc).__name__}: {exc}"))
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


def measure_processes(sql, workers, reads, rows):
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
        multiprocessing.Process(target=_process_worker, args=(sql, reads, rows, out, barrier))
        for _ in range(workers)
    ]
    for p in procs:
        p.start()
    for p in procs:
        p.join(timeout=180)
        if p.exitcode != 0:
            raise SystemExit(f"processes: a worker exited with {p.exitcode}")

    results = [out.get(timeout=10) for _ in procs]
    for i, (total, first, last, served, _elapsed, error) in enumerate(results):
        if error:
            raise SystemExit(f"processes worker {i}: {error}")
        check(f"processes n={workers} worker {i}", total, first, last, served, reads, rows)
    return max(r[4] for r in results)


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
        measure_serial(sql, 1, args.rows)  # warm: the first read creates the ring
        base = median_elapsed(lambda: measure_serial(sql, args.reads, args.rows))
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
                measure_processes(sql, workers, 1, args.rows)  # warm
                rate = (
                    workers * args.reads * args.rows
                    / median_elapsed(lambda workers=workers: measure_processes(sql, workers, args.reads, args.rows))
                )
                report("processes", workers, rate, f"{workers * args.reads}/{workers * args.reads}")

        def runners(mode, workers):
            """The warm-up and the timed run for one setting."""
            if mode == "threads":
                return (lambda: measure_threads(sql, workers, 1, args.rows),
                        lambda: measure_threads(sql, workers, args.reads, args.rows))
            if mode == "asyncio":
                return (lambda: asyncio.run(measure_asyncio(sql, workers, 1, args.rows)),
                        lambda: asyncio.run(measure_asyncio(sql, workers, args.reads, args.rows)))
            return (lambda: measure_columnar(sql, workers, 1, args.rows, batch),
                    lambda: measure_columnar(sql, workers, args.reads, args.rows, batch))

        modes = [m for m in ("threads", "asyncio", "columnar") if args.mode in (m, "all")]
        # The modes alternate at each worker count, not block after block: a
        # setting measured twenty runs later sees a different machine (clock,
        # page state), and that drift is bigger than what threads buy.
        for workers in range(1, args.max_workers + 1):
            for mode in modes:
                warm, timed = runners(mode, workers)
                warm()  # the rings this setting will use
                rate = workers * args.reads * args.rows / median_elapsed(timed)
                # a streamed read is not counted by shm_reads; its correctness
                # check is the sum every worker must agree on
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
