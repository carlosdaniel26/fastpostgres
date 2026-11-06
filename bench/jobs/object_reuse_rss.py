"""Materialise a read through the row decoder and report time and peak RSS.

The default DB-API read is lazy (a ResultSet over copied columnar frames), so
it never builds a Python object per value. Object reuse lives in `read_rows` /
the row-frame deformer, so this job forces the eager path (`force_tuples=True`)
and repeats the read in one process: the point is not only ns/row, but that a
second read of the same shape reuses the first read's storage instead of adding
resident pages.

Runs inside the dev image (the entrypoint starts PostgreSQL and installs the
extension):

    docker run --rm --shm-size=3g -e BENCH_GIT_REV=<rev> \
        -e FASTPOSTGRES_OBJ_ARENA=<bytes|0> fpq-<name> \
        python3 /src/bench/jobs/object_reuse_rss.py [nrows] [reps]
"""
import gc
import os
import resource
import sys
import time

sys.path.insert(0, "/src/bench")  # lib/

from lib.dataset import CORE_SQL, setup  # noqa: E402
from fastpostgres import dbapi  # noqa: E402

DATABASE_URL = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")
N = int(sys.argv[1]) if len(sys.argv) > 1 else 1_000_000
REPS = int(sys.argv[2]) if len(sys.argv) > 2 else 5


def rss_mib():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024


def one_read(conn):
    cur = conn.cursor()
    try:
        t0 = time.perf_counter()
        cur.execute(CORE_SQL)
        rows = cur.fetchall()
        ms = (time.perf_counter() - t0) * 1000
        count = len(rows)
    finally:
        cur.close()
    del rows
    gc.collect()
    return ms, count


def main():
    setup(N)
    print(f"# object reuse: n={N} reps={REPS} rss_after_setup={rss_mib():.1f} MiB")

    # force_tuples=True is the eager DB-API path: cursor.execute() calls
    # read_rows(), which builds one typed tuple per row in C.
    conn = dbapi.connect(DATABASE_URL, force_tuples=True)
    try:
        ms, count = one_read(conn)  # warm: rings, plans, page faults
        print(f"warm {ms:.1f} ms {count / (ms / 1000) / 1e6:.2f}M rows/s rss={rss_mib():.1f} MiB")
        times = []
        counts = []
        for i in range(REPS):
            ms, count = one_read(conn)
            times.append(ms)
            counts.append(count)
            print(f"rep {i + 1} {ms:.1f} ms {count / (ms / 1000) / 1e6:.2f}M rows/s "
                  f"ns/row={ms * 1e6 / count:.1f} rss={rss_mib():.1f} MiB")
    finally:
        conn.close()

    best = min(times)
    print(f"### dbapi_eager n={N} best_ms={best:.1f} "
          f"ns_per_row={best * 1e6 / N:.1f} rows_per_s={N / (best / 1000):.0f} "
          f"peak_rss_mib={rss_mib():.1f} reps={REPS}")
    if any(c != N for c in counts):
        raise SystemExit(f"read returned {counts}, expected {N}")


if __name__ == "__main__":
    main()
