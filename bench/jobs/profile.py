#!/usr/bin/env python3
"""Where a full read spends its time, for the profile log.

`bench/jobs/profile.sh` runs this and attaches `perf` to the two processes a
read actually uses: this one (the client) and the ring worker inside the
server. The workload is the DB-API read an application makes, with the rows
consumed - `execute()` publishes and copies every batch, then `to_tuples()`
decodes them - so both halves are paid and timed apart:

    execute()      the worker's turn, the ring handoff, the segment copy
    to_tuples()    the C++ decode of the copied columns into Python tuples

The measured loop starts only after setup and a warm read, and the shell
attaches `perf` at that boundary: no DDL, no table build, no page faults from
the first touch are in the samples. The worker pid is the one the read just
started (pg_stat_activity, the same way `worker-map-batch.py` finds it).

Usage: python3 bench/jobs/profile.py [rows] [--batch N] [--reps N] [--seconds N]
       (defaults: 1000000 100000 20 30)

The shell passes the pid file and stop file in FP_PROFILE_DIR; this prints the
`### PROFILE` block the log quotes.
"""
import argparse
import json
import os
import statistics
import sys
import time

import fastpostgres as fp
import psycopg
from fastpostgres import dbapi

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from lib.dataset import CORE_SQL, setup  # noqa: E402

CONNINFO = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")
DIR = os.environ.get("FP_PROFILE_DIR", "/tmp/fp-profile")


def positive(text):
    n = int(text)
    if n <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return n


def worker_pids():
    with psycopg.connect(CONNINFO, autocommit=True) as admin:
        return {
            row[0]
            for row in admin.execute(
                "SELECT pid FROM pg_stat_activity "
                "WHERE backend_type = 'fastpostgres ring worker'"
            ).fetchall()
        }


def proc_cpu_ns(pid):
    """On-CPU nanoseconds for a process (schedstat's first field)."""
    if pid is None:
        return None
    try:
        with open(f"/proc/{pid}/schedstat") as f:
            return int(f.read().split()[0])
    except OSError:
        return None


def self_cpu_ns():
    with open("/proc/self/schedstat") as f:
        return int(f.read().split()[0])


def wait_for(path, timeout=60.0):
    end = time.monotonic() + timeout
    while not os.path.exists(path):
        if time.monotonic() >= end:
            raise SystemExit(f"profile: timed out waiting for {path}")
        time.sleep(0.005)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("rows", nargs="?", type=positive, default=1_000_000,
                    help="table rows, and rows per read (default: %(default)s)")
    ap.add_argument("--batch", type=positive, default=100_000,
                    help="rows per ring batch (default: %(default)s)")
    ap.add_argument("--reps", type=positive, default=20,
                    help="measured reads (default: %(default)s)")
    ap.add_argument("--seconds", type=positive, default=30,
                    help="wall-clock cap on the measured loop (default: %(default)s)")
    args = ap.parse_args()

    os.makedirs(DIR, exist_ok=True)
    setup(args.rows)

    before = worker_pids()
    conn = dbapi.connect(CONNINFO)
    cur = conn.cursor()

    # Warm: compile, open the ring, touch the segment, decode once. The worker
    # the read started is the one perf has to follow.
    cur.execute(CORE_SQL, {"batch": args.batch} if False else None)
    cur.fetchall().to_tuples()
    new = sorted(worker_pids() - before)
    if len(new) != 1:
        raise SystemExit(f"profile: expected one new ring worker, saw {new}")
    worker = new[0]

    pids = {"client": os.getpid(), "worker": worker}
    with open(os.path.join(DIR, "pids.json"), "w") as f:
        json.dump(pids, f)
    wait_for(os.path.join(DIR, "go"))

    worker_cpu0 = proc_cpu_ns(worker)
    client_cpu0 = self_cpu_ns()
    phases = []
    wall0 = time.perf_counter()
    for rep in range(args.reps):
        if time.perf_counter() - wall0 > args.seconds:
            break
        t0 = time.perf_counter_ns()
        cur.execute(CORE_SQL)
        t1 = time.perf_counter_ns()
        rows = cur.fetchall().to_tuples()
        t2 = time.perf_counter_ns()
        if len(rows) != args.rows:
            raise SystemExit(f"profile: read {len(rows)} rows, expected {args.rows}")
        phases.append((t1 - t0, t2 - t1))
    wall = time.perf_counter() - wall0
    worker_cpu = proc_cpu_ns(worker)
    client_cpu = self_cpu_ns()

    n = len(phases)
    if n == 0:
        raise SystemExit("profile: the measured loop never ran")
    total_rows = n * args.rows
    execute = statistics.median(p[0] for p in phases)
    decode = statistics.median(p[1] for p in phases)
    total = execute + decode
    cpu_worker = None if worker_cpu is None else (worker_cpu - worker_cpu0) / total_rows
    cpu_client = (client_cpu - client_cpu0) / total_rows

    print("### PROFILE")
    print("path: fastpostgres DB-API, rows materialised with ResultSet.to_tuples()")
    print(f"rows: {args.rows} batch: {args.batch} reps: {n} shape: {os.environ.get('SHAPE') or 'bench'}")
    print(f"client pid: {pids['client']} worker pid: {worker}")
    print()
    print(f"{'phase':<34}{'ms/read':>12}{'ns/row':>12}{'share':>9}")
    print(f"{'execute (worker + copy)':<34}{execute / 1e6:>12.2f}{execute / args.rows:>12.1f}{execute / total * 100:>8.1f}%")
    print(f"{'to_tuples (C++ decode)':<34}{decode / 1e6:>12.2f}{decode / args.rows:>12.1f}{decode / total * 100:>8.1f}%")
    print(f"{'total':<34}{total / 1e6:>12.2f}{total / args.rows:>12.1f}{'100.0%':>9}")
    print()
    print(f"throughput: {total_rows / wall / 1e6:.2f}M rows/s ({n} reads over {wall:.2f}s)")
    print(f"client CPU: {cpu_client:.1f} ns/row   worker CPU: "
          f"{'n/a' if cpu_worker is None else f'{cpu_worker:.1f}'} ns/row")
    print(f"spread: execute {min(p[0] for p in phases) / 1e6:.2f}-{max(p[0] for p in phases) / 1e6:.2f} ms, "
          f"decode {min(p[1] for p in phases) / 1e6:.2f}-{max(p[1] for p in phases) / 1e6:.2f} ms")
    print("### END PROFILE")

    cur.close()
    conn.close()
    with open(os.path.join(DIR, "done"), "w") as f:
        f.write(str(n))


if __name__ == "__main__":
    sys.exit(main())
