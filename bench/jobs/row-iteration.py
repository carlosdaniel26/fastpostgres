"""The lazy result's row costs, on the benchmark's own shape.

Three ways to pay for the same read, in one process, with the same connection:

  fetchall     execute() + fetchall(), nothing consumed. On the lazy result this
               is the ring read plus the column copy; on the older list result it
               is the eager per-row decode.
  iterate      execute() + fetchall() + consume every row, the per-row path.
  materialize  execute() + fetchall() + to_tuples() where the result has it; on
               the older list result this is the list already in hand.

Rows-per-second is the benchmark's row count over the median wall time. It is a
probe, not a published rival comparison: it exists to show the block-wise decode
against the per-row one and the materialising path.

Runs inside the image:

  python3 /src/bench/jobs/row-iteration.py [nrows ...]
"""
import json
import os
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # bench/: lib

from lib.dataset import CORE_SQL, setup  # noqa: E402

import fastpostgres.dbapi as dbapi  # noqa: E402

DATABASE_URL = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")
REPS = 3


def now():
    return time.perf_counter()


def consume(rows):
    for _ in rows:
        pass


def run_cell(conn, cell):
    cur = conn.cursor()
    try:
        t0 = now()
        cur.execute(CORE_SQL)
        rows = cur.fetchall()
        if cell == "iterate":
            consume(rows)
        elif cell == "materialize":
            if hasattr(rows, "to_tuples"):
                rows.to_tuples()
            else:
                list(rows)
        return (now() - t0) * 1000, len(rows)
    finally:
        cur.close()


def measure(conn, cell, n, reps=REPS):
    run_cell(conn, cell)  # warm: rings, mappings, plans
    times = []
    count = 0
    for _ in range(reps):
        ms, count = run_cell(conn, cell)
        times.append(ms)
    if count != n:
        raise SystemExit(f"{cell}: read {count} rows, expected {n}")
    return statistics.median(times)


def main():
    sizes = [int(a) for a in sys.argv[1:] if a.isdigit() and int(a) > 0] or [100000, 1000000]
    conn = dbapi.connect(DATABASE_URL)
    try:
        for n in sizes:
            setup(n)
            for cell in ("fetchall", "iterate", "materialize"):
                ms = measure(conn, cell, n)
                rate = n / (ms / 1000) / 1e6
                print(json.dumps({
                    "rows": n,
                    "cell": cell,
                    "ms": ms,
                    "rows_per_s": rate,
                }))
    finally:
        conn.close()


if __name__ == "__main__":
    main()
