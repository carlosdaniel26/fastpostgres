"""One player, one scenario, one process.

Run by runner.py, never by hand:

    player.py --scenario lookup --player fastpostgres --scale 1 [--reps 3]
              [--duration 10] [--client-index 0] [--clients 1] [--seed 7]

It opens the player's connection, warms it with the scenario's statement, runs the
timed work, and prints one JSON object: the raw timings (read and write samples,
the wall time of the timed region), the scenario's digest, and the evidence that
the work took the path it claims - read statements versus reads the ring served,
the ring pool size, the segment bytes this process mapped, and peak RSS.

`--client-index`/`--clients` partition the seeded parameter stream, so several
processes of one player can run at once (the concurrency and mixed scenarios) and
still issue a deterministic, disjoint set of statements.
"""
import argparse
import json
import random
import sys
import time
from pathlib import Path

import psycopg

from fastpostgres import dbapi

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # benchmarks/

from lib.harness import mapped_segment_bytes, rss_bytes  # noqa: E402
from scenarios import BY_NAME, write_param  # noqa: E402
from schema import DATABASE_URL, counts  # noqa: E402

PLAYERS = ("fastpostgres", "psycopg 3")


def now():
    return time.perf_counter()


def connect(player):
    if player == "fastpostgres":
        return dbapi.connect(DATABASE_URL)
    conn = psycopg.connect(DATABASE_URL)
    conn.autocommit = True
    return conn


def measure(conn, scenario, n, args):
    """Run the scenario's work; returns the timings, the wall time and the digest."""
    cur = conn.cursor()
    rng = random.Random(f"{scenario.name}:{args.seed}:{args.client_index}")
    samples, write_samples = [], []
    reads = writes = 0
    rows_per_read = 0
    ok, count, acc_a, acc_b = True, 0, 0, 0

    def read(i):
        nonlocal reads
        param = scenario.param(i, n, rng)
        t0 = now()
        cur.execute(scenario.sql, param)
        rows = cur.fetchall()
        ms = (now() - t0) * 1000
        reads += 1
        good, c, x, y = scenario.digest(rows, param)
        return ms, good, c, x, y

    def write(i):
        nonlocal writes
        param = write_param(i, n, rng)
        t0 = now()
        cur.execute(scenario.write_sql, param)
        ms = (now() - t0) * 1000
        writes += 1
        return ms, cur.rowcount == 1

    try:
        warm = 1 if scenario.kind == "bulk" else 20
        for i in range(warm):
            read(i)  # warm: connections, rings, plans
        reads = writes = 0  # the evidence counts the timed work, not the warm-up
        shm_before = getattr(conn, "shm_reads", 0)
        if scenario.kind == "statements":
            started = now()
            for _ in range(args.reps):
                for i in range(scenario.statements):
                    ms, good, c, x, y = read(i)
                    samples.append(ms)
                    ok &= good
                    count += c
                    acc_a += x
                    acc_b += y
        elif scenario.kind == "bulk":
            param = scenario.param(0, n, rng)
            started = now()
            for _ in range(args.reps):
                t0 = now()
                cur.execute(scenario.sql, param)
                rows = cur.fetchall()
                samples.append((now() - t0) * 1000)
                reads += 1
                rows_per_read = len(rows)
                good, c, x, y = scenario.digest(rows, param)
                ok &= good
                count += c
                acc_a += x
                acc_b += y
        else:  # duration
            started = now()
            deadline = started + args.duration
            i = 0
            while now() < deadline:
                if scenario.write_sql and i % 10 == 9:
                    ms, good = write(i)
                    write_samples.append(ms)
                    ok &= good
                else:
                    ms, good, c, x, y = read(i)
                    samples.append(ms)
                    ok &= good
                    count += c
                    acc_a += x
                    acc_b += y
                i += 1
        work_ms = (now() - started) * 1000
    finally:
        cur.close()

    return {
        "samples": samples,
        "write_samples": write_samples,
        "work_ms": work_ms,
        "reads": reads,
        "writes": writes,
        "rows_per_read": rows_per_read,
        "valid": ok,
        "digest": [count, acc_a, round(acc_b, 3)],
        "shm_reads": getattr(conn, "shm_reads", 0) - shm_before,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scenario", required=True, choices=sorted(BY_NAME))
    ap.add_argument("--player", required=True, choices=PLAYERS)
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--duration", type=float, default=10.0)
    ap.add_argument("--client-index", type=int, default=0)
    ap.add_argument("--clients", type=int, default=1)
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    scenario = BY_NAME[args.scenario]
    n = counts(args.scale)
    conn = connect(args.player)
    try:
        result = measure(conn, scenario, n, args)
        result["pool"] = dbapi.pool_size() if args.player == "fastpostgres" else 0
    finally:
        conn.close()

    result.update(
        {
            "scenario": scenario.name,
            "player": args.player,
            "kind": scenario.kind,
            "client_index": args.client_index,
            "mapped_bytes": mapped_segment_bytes(),
            "rss_bytes": rss_bytes(),
        }
    )
    print(json.dumps(result))


if __name__ == "__main__":
    main()
