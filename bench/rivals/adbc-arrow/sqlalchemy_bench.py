"""fast-postgres vs ADBC + Arrow: the same reads from both, across load sizes.

Two players, because the fair rival for a typed shared-memory transport is the
Arrow ecosystem's own answer to the same read - ADBC's PostgreSQL driver, whose
results arrive as Arrow record batches - and the socket's text protocol is no
longer in the comparison:

  TUPLE READ      the same output from both: Python tuples. fast-postgres's own
                  DBAPI returns them directly; ADBC always converts the wire
                  data into Arrow buffers, so its column pays that conversion
                  plus a per-column to_pylist
  ZERO-COPY READ  the same output from both: the columnar form each stack lands,
                  with one column summed in place - stream()'s numpy view over
                  the shm bytes, ADBC's pyarrow.compute over its Arrow table

Both sections compare like with like: the first against object rows, the second
against columnar rows. Comparing fast-postgres's tuples against ADBC's Arrow
table would price the two different outputs, not the two transports.

Then a PATHS table puts everything else fast-postgres can do with those rows
next to its own driver read - through SQLAlchemy, the lean core interface, four
processes - so what a wrapper costs is visible instead of implied.

Every measurement is one player per process, one at a time. Interleaving them
measures the neighbours' cache footprint (with the Arrow rival present, the
fast-postgres row read came out ~20% below the same read measured alone), and a
subprocess - not a forked worker, which exits through os._exit - lets each
player print its own `read_rows` report. A warm read precedes the timed
reps; past 1M rows a single rep.

Usage: python3 bench/rivals/sqlalchemy_bench.py [nrows ...]
       (default 100000 1000000 10000000)
"""
import json
import os
import statistics
import subprocess
import sys
import time
from multiprocessing import Pool
from pathlib import Path

import adbc_driver_postgresql.dbapi as adbc
import fastpostgres.dialect  # noqa: F401  registers postgresql+fastpostgres
import numpy as np
import pyarrow.compute as pc
from fastpostgres import dbapi
from fastpostgres.core import create_engine as lean_engine
from sqlalchemy import select

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))  # bench/: lib

from lib.dataset import BASE, CORE_SQL, SHAPE, SHAPES, Row, setup  # noqa: E402

DATABASE_URL = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")
FPG = "fastpostgres"
ARROW = "adbc+arrow"
FPG_URL = f"postgresql+fastpostgres://{BASE}"
ARROW_URI = f"postgresql://{BASE}"

REPS = 3
# Past this, one read is slow and the memory a full materialisation needs has to
# be bounded, so one rep.
REPS_MAX_ROWS = 1_000_000


def now():
    return time.perf_counter()


def fmt(n):
    if n >= 1_000_000:
        return f"{n // 1_000_000}M"
    if n >= 1_000:
        return f"{n // 1_000}K"
    return str(n)


def col(value, width, right):
    s = str(value)
    return s.rjust(width) if right else s.ljust(width)


# ---- what each player does ------------------------------------------------


def open_dbapi():
    return dbapi.connect(DATABASE_URL)


def open_arrow():
    return adbc.connect(ARROW_URI)


def open_lean():
    return lean_engine(FPG_URL)


def read_dbapi(conn):
    """fast-postgres's own DBAPI: the naked read, typed tuples."""
    cur = conn.cursor()
    t0 = now()
    cur.execute(CORE_SQL)
    rows = cur.fetchall()
    ms = (now() - t0) * 1000
    cur.close()
    return ms, len(rows)


def fetch_arrow(conn):
    """ADBC fetch, untimed: an Arrow table with the transaction ended.

    The PostgreSQL ADBC driver does not autocommit, so the read's transaction is
    ended here: left open it holds ACCESS SHARE on the table, and the harness's
    DDL between sizes would wait on it.
    """
    cur = conn.cursor()
    cur.execute(CORE_SQL)
    table = cur.fetch_arrow_table()
    cur.close()
    conn.commit()
    return table


def read_arrow_tuples(conn):
    """ADBC's Arrow table turned into Python tuples, column by column.

    The like-for-like rival for the DBAPI's typed tuples: ADBC always converts
    the wire data into Arrow buffers first, so asking it for rows means paying
    that conversion and then the per-column `to_pylist`.
    """
    t0 = now()
    table = fetch_arrow(conn)
    list(zip(*(col.to_pylist() for col in table.columns)))
    ms = (now() - t0) * 1000
    return ms, table.num_rows


def read_arrow_fetchall(conn):
    """ADBC's own DBAPI rows: `fetchall()`.

    The plainest way any DBAPI hands back Python tuples, and the comparison the
    TUPLE READ section's rival was missing: ADBC does its own row conversion
    instead of this harness making a column-wise `to_pylist` in Python. Same
    output as the to_pylist route and as this repo's DBAPI read.
    """
    cur = conn.cursor()
    t0 = now()
    cur.execute(CORE_SQL)
    rows = cur.fetchall()
    ms = (now() - t0) * 1000
    cur.close()
    conn.commit()
    return ms, len(rows)


def read_stream(conn):
    """stream()'s zero-copy column views, summed in place with numpy."""
    cur = conn.cursor()
    t0 = now()
    total = 0
    for batch in cur.stream(CORE_SQL, size=100000):
        total += batch.row_count
        if batch.row_count:
            np.frombuffer(batch.columns[3].data, dtype="<f8").sum()
    ms = (now() - t0) * 1000
    cur.close()
    return ms, total


def read_arrow_sum(conn):
    """ADBC's Arrow table, summed over the same column with pyarrow.compute."""
    t0 = now()
    table = fetch_arrow(conn)
    pc.sum(table["price"])
    ms = (now() - t0) * 1000
    return ms, table.num_rows


# ---- the two headline sections -------------------------------------------
#
# key, column label, handle opener, reader. One entry per player, in column
# order; the row's comparison column is the second player against the first.

SECTIONS = {
    "tuples": {
        "title": "TUPLE READ (rows/s)",
        "note": "same output: ADBC's Arrow table converted column-wise with to_pylist",
        "players": (
            (FPG, "fastpostgres", open_dbapi, read_dbapi),
            (ARROW, "adbc+tuples", open_arrow, read_arrow_tuples),
        ),
        "ratio": "vs arrow",
    },
    "zerocopy": {
        "title": "ZERO-COPY READ (rows/s)",
        "note": "same output: the columnar form each stack lands, one column summed in place",
        "players": (
            (FPG, "stream()+sum", open_dbapi, read_stream),
            (ARROW, "arrow+sum", open_arrow, read_arrow_sum),
        ),
        "ratio": "vs arrow",
    },
}


def header(section):
    spec = SECTIONS[section]
    names = [label for _key, label, _open, _read in spec["players"]] + [spec["ratio"]]
    print(f"\n{spec['title']}: " + " ".join(col(s, 12, False) for s in names))
    print(f"  ({spec['note']})")


def pct(ratio):
    """Improvement over the reference, in percent (the ratio is ref time / this time)."""
    return f"{(ratio - 1) * 100:+.1f}%"


def row(load, medians, counts, section):
    spec = SECTIONS[section]
    keys = [key for key, *_ in spec["players"]]
    rates = {key: load / (medians[key] / 1000) / 1e6 for key in keys}
    ok = all(counts[key] == load for key in keys)
    rate_cells = " ".join(col(f"{rates[key]:.2f}", 11, True) + "M" for key in keys)
    ratio = pct(medians[keys[1]] / medians[keys[0]])
    print(
        "  "
        + col(fmt(load), 8, False)
        + " "
        + rate_cells
        + " "
        + col(ratio, 9, True)
        + "  "
        + ("ok" if ok else "MISMATCH")
    )


# ---- measuring ------------------------------------------------------------


def drive(section, key, n, reps):
    """One player's reads of one size: a warm read, then `reps` timed ones."""
    _key, _label, open_handle, read = next(p for p in SECTIONS[section]["players"] if p[0] == key)
    handle = open_handle()
    try:
        read(handle)  # warm: connections, rings, arrow pools, plans
        times = []
        count = 0
        for _ in range(reps):
            ms, count = read(handle)
            times.append(ms)
        return statistics.median(times), count, min(times), max(times)
    finally:
        handle.close()


def spawn(args):
    """Run one measurement in a process of its own and read its JSON answer."""
    proc = subprocess.run([sys.executable, __file__, *args], capture_output=True, text=True)
    sys.stderr.write(proc.stderr)
    if proc.returncode != 0:
        raise SystemExit(f"{' '.join(args)} failed:\n{proc.stdout[-2000:]}{proc.stderr[-2000:]}")
    return json.loads(proc.stdout.strip().splitlines()[-1])


def read_one_player(section, key, n, reps):
    """Run one player in a process of its own, one player at a time.

    Not a forked worker: those exit through os._exit, and the client prints its
    `read_rows` report from Py_AtExit, which belongs in the log per player.
    Returns (key, median ms, rows, fastest ms, slowest ms).
    """
    ms, count, lo, hi = spawn(["--player", section, key, str(n), str(reps)])
    return key, ms, count, lo, hi


def spread_line(section, spread, reps):
    """The spread over the reps, under the row it belongs to: ± half the range."""
    parts = []
    for key, label, *_ in SECTIONS[section]["players"]:
        lo, hi = spread[key]
        if reps == 1 or lo == hi:
            parts.append(f"{label} n=1")
            continue
        mid = (lo + hi) / 2
        parts.append(f"{label} ±{100 * (hi - lo) / 2 / mid:.1f}%")
    print("  spread: " + "  ".join(parts))


def measure(section, sizes):
    """One section: the same rows, one player at a time, medians of the same reps."""
    keys = [key for key, *_ in SECTIONS[section]["players"]]
    header(section)
    for n in sizes:
        setup(n)  # a fresh table of exactly n rows, so each read returns n
        reps = REPS if n <= REPS_MAX_ROWS else 1
        results = [read_one_player(section, key, n, reps) for key in keys]
        medians = {key: median for key, median, _count, _lo, _hi in results}
        counts = {key: count for key, _median, count, _lo, _hi in results}
        spread = {key: (lo, hi) for key, _median, _count, lo, hi in results}
        row(n, medians, counts, section)
        spread_line(section, spread, reps)


# ---- everything else fast-postgres does with those rows -------------------
#
# key, label, opener, reader, closer, rows per read. Each path is measured in a
# process of its own, like the sections: in one shared process the first path
# pays the ring bootstrap and the segment's page faults for every path after it.


def fpg_worker(_):
    """One DBAPI read in a process of its own: work parallelism, no wrapper."""
    handle = open_dbapi()
    try:
        return read_dbapi(handle)[1]
    finally:
        handle.close()


def read_lean(engine):
    t0 = now()
    with engine.connect() as conn:
        rows = conn.execute(
            select(Row.id, Row.sku, Row.name, Row.price, Row.stock, Row.active)
        ).all()
    ms = (now() - t0) * 1000
    return ms, len(rows)


def open_pool():
    return Pool(4)


def read_parallel(pool):
    t0 = now()
    counts = pool.map(fpg_worker, range(4))
    ms = (now() - t0) * 1000
    if len(set(counts)) != 1:
        raise SystemExit("parallel reads returned different row counts")
    return ms, sum(counts)


def close_pool(pool):
    pool.close()
    pool.join()


def close_conn(handle):
    handle.close()


def close_engine(engine):
    engine.dispose()


PATHS = (
    ("dbapi", "fastpostgres, DBAPI", open_dbapi, read_dbapi, close_conn, 1),
    ("stream", "fastpostgres, stream()+sum", open_dbapi, read_stream, close_conn, 1),
    ("core", "fastpostgres, core interface", open_lean, read_lean, close_engine, 1),
    ("par", "fastpostgres, 4 processes", open_pool, read_parallel, close_pool, 4),
    ("arrow", "adbc+arrow, arrow+sum", open_arrow, read_arrow_sum, close_conn, 1),
    ("arrow_rows", "adbc+arrow, fetchall()", open_arrow, read_arrow_fetchall, close_conn, 1),
    ("arrow_pylist", "adbc+arrow, table+to_pylist", open_arrow, read_arrow_tuples, close_conn, 1),
)


def run_path(key, n, reps):
    """One path's reads of one size: a warm read, then `reps` timed ones."""
    _key, _label, open_handle, read, close, multiple = next(p for p in PATHS if p[0] == key)
    handle = open_handle()
    try:
        read(handle)  # warm
        times = []
        count = 0
        for _ in range(reps):
            ms, count = read(handle)
            times.append(ms)
        if count != multiple * n:
            raise SystemExit(f"{key}: read {count} rows, expected {multiple * n}")
        return statistics.median(times), count
    finally:
        close(handle)


def measure_paths(sizes):
    """fast-postgres's other paths, against its own driver read, on the same rows."""
    print("\nPATHS, the same rows (rows/s):" + " " * 22 + "vs driver read")
    for n in sizes:
        setup(n)
        reps = REPS if n <= REPS_MAX_ROWS else 1
        results = {}
        for key, *_ in PATHS:
            ms, count = spawn(["--path", key, str(n), str(reps)])
            results[key] = (ms, count)

        driver_rate = results["dbapi"][1] / (results["dbapi"][0] / 1000) / 1e6
        for key, label, *_ in PATHS:
            ms, count = results[key]
            rate = count / (ms / 1000) / 1e6
            delta = "" if key == "dbapi" else f"{(rate / driver_rate - 1) * 100:+.0f}%"
            print("  " + col(fmt(n), 8, False) + " " + col(label, 30, False)
                  + col(f"{rate:.2f}M", 11, True) + " " + col(delta, 9, True))


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--player":
        # the private mode read_one_player() spawns: one player, one size
        section, key = sys.argv[2], sys.argv[3]
        n, reps = int(sys.argv[4]), int(sys.argv[5])
        median, count, lo, hi = drive(section, key, n, reps)
        print(json.dumps([median, count, lo, hi]))
        return
    if len(sys.argv) > 1 and sys.argv[1] == "--path":
        # ... and the mode measure_paths() spawns: one path, one size
        key, n, reps = sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
        ms, count = run_path(key, n, reps)
        print(json.dumps([ms, count]))
        return

    sizes = [int(a) for a in sys.argv[1:] if a.isdigit() and int(a) > 0]
    if not sizes:
        sizes = [100000, 1000000, 10000000]
    if SHAPE not in SHAPES:
        raise SystemExit(f"SHAPE={SHAPE!r}: choose one of {', '.join(SHAPES)}")

    print(f"# shape: {SHAPE} — {SHAPES[SHAPE][0]}")
    measure("tuples", sizes)
    measure("zerocopy", sizes)
    measure_paths(sizes)


if __name__ == "__main__":
    main()
