"""bench.py — the same data across three transports, read and write:

  tcp    libpq over 127.0.0.1          (text rows -> Python objects)
  unix   libpq over the unix socket    (text rows -> Python objects)
  shm    native columnar over /dev/shm (zero-copy views, ring control)

Sizes default to 100000 1000000: the text paths materialise every row as a dict,
so larger loads need more memory (and the shm path streams in batches).

Usage: python3 benchmark/bench.py [nrows ...]
"""
import os
import statistics
import sys
import time

import fastpostgres as fp

QUERY = "SELECT id, account_id, balance, active, name, email, score, age FROM bench"
SEG = "fastpostgres_bench_seg"
RING = "fastpostgres_bench_ring"

CONN_TCP = os.environ.get("FASTPOSTGRES_TCP", "host=127.0.0.1 dbname=postgres user=postgres")
CONN_UNIX = os.environ.get("FASTPOSTGRES_UNIX", "host=/tmp dbname=postgres user=postgres")

READ_BATCH = 100000

# Each transport is measured REPS times, rotating the run order so neither the
# warm-up nor the write-ahead log of an earlier run systematically favours one
# transport. The reported figure is the median time (hence the median rate).
REPS = 3
# Repeats are cheap and useful for small loads; at 10M a single read takes
# minutes, so the relative variance is already tiny and one run suffices.
REPS_MAX_ROWS = 1_000_000
ORDERS = [("tcp", "unix", "shm"), ("shm", "unix", "tcp"), ("unix", "tcp", "shm")]


def now():
    return time.perf_counter()


def rate(nrows, ms):
    return nrows / (ms / 1000) / 1e6  # M rows/s


def fmt(n):
    if n >= 1_000_000:
        return f"{n // 1_000_000}M"
    if n >= 1_000:
        return f"{n // 1_000}K"
    return str(n)


def setup_table(conn, n):
    conn.exec("DROP TABLE IF EXISTS bench")
    conn.exec(
        f"""CREATE TABLE bench AS
            SELECT g::int4 AS id, (g::int8 * 1000) AS account_id,
                   (g*1.5)::float8 AS balance, (g % 2 = 0)::bool AS active,
                   ('user' || g)::text AS name, ('user' || g || '@example.com')::text AS email,
                   (g*0.25)::float8 AS score, (g % 100)::int4 AS age
            FROM generate_series(1, {n}) g"""
    )


# ---- READ ----------------------------------------------------------------


def read_text(conn):
    """Read rows over libpq; the cost is libpq's text decode plus the row dicts."""
    t0 = now()
    res = conn.exec(QUERY)
    nbytes = sum(len(r["name"] or "") for r in res.rows)
    ms = (now() - t0) * 1000
    if nbytes == 0:
        raise RuntimeError("read: empty result")
    return ms


def read_shm(ring):
    """Pull chunks through the ring (no socket) and read each from /dev/shm."""
    t0 = now()
    ring.open(SEG, QUERY)
    while True:
        n = ring.next(READ_BATCH)
        fp.read(SEG)  # zero-copy chunk, consumed before the next fetch
        if n == 0:
            break
    ring.close()
    return (now() - t0) * 1000


# ---- WRITE ---------------------------------------------------------------


def write_copy(conn, n):
    conn.exec("DROP TABLE IF EXISTS dst; CREATE TABLE dst (LIKE bench)")
    t0 = now()
    conn.copy_from("COPY dst FROM STDIN", fp.synth_copy(n))  # native build + send
    return (now() - t0) * 1000


def write_shm(conn, ring, n):
    conn.exec("DROP TABLE IF EXISTS dst; CREATE TABLE dst (LIKE bench)")
    t0 = now()
    fp.synth_segment(SEG, n)  # native: generate rows straight into /dev/shm
    ring.write(SEG, "dst")  # no socket
    return (now() - t0) * 1000


# ---- output --------------------------------------------------------------


def col(value, width, right):
    s = str(value)
    return s.rjust(width) if right else s.ljust(width)


def header(title):
    names = ["tcp", "unix", "shm", "shm/tcp", "shm/unix"]
    print(f"\n{title}: " + " ".join(col(s, 8 if i < 3 else 10, False) for i, s in enumerate(names)) + " ")


def row(load, a, b, c, ra, rb):
    print(
        "  "
        + col(fmt(load), 8, False)
        + " "
        + col(f"{a:.2f}", 8, True)
        + "M "
        + col(f"{b:.2f}", 8, True)
        + "M "
        + col(f"{c:.2f}", 8, True)
        + "M "
        + col(f"{ra:.2f}", 8, True)
        + "x "
        + col(f"{rb:.2f}", 9, True)
        + "x"
    )


def main():
    sizes = [int(a) for a in sys.argv[1:] if a.isdigit() and int(a) > 0]
    if not sizes:
        sizes = [100000, 1000000]

    tcp = fp.Connection(CONN_TCP)
    unix = fp.Connection(CONN_UNIX)
    tcp.exec("SET client_min_messages = warning")  # keep the tables clean
    unix.exec("SET client_min_messages = warning")

    ring = fp.Ring(RING)
    ring.start(unix)  # one SQL call bootstraps the worker; the shm path then uses only the ring

    try:
        header("READ (rows/s)")
        for n in sizes:
            setup_table(unix, n)
            times = {"tcp": [], "unix": [], "shm": []}
            reps = REPS if n <= REPS_MAX_ROWS else 1
            if reps > 1:
                read_text(tcp)  # warm-up, discarded
            for r in range(reps):
                for name in ORDERS[r % len(ORDERS)]:
                    if name == "tcp":
                        t = read_text(tcp)
                    elif name == "unix":
                        t = read_text(unix)
                    else:
                        t = read_shm(ring)
                    times[name].append(t)
            rt, ru, rs = (statistics.median(times[k]) for k in ("tcp", "unix", "shm"))
            row(n, rate(n, rt), rate(n, ru), rate(n, rs), rt / rs, ru / rs)

        print()
        header("WRITE (rows/s)")
        for n in sizes:
            times = {"tcp": [], "unix": [], "shm": []}
            reps = REPS if n <= REPS_MAX_ROWS else 1
            if reps > 1:
                write_copy(tcp, n)  # warm-up, discarded
            for r in range(reps):
                for name in ORDERS[r % len(ORDERS)]:
                    if name == "tcp":
                        t = write_copy(tcp, n)
                    elif name == "unix":
                        t = write_copy(unix, n)
                    else:
                        t = write_shm(unix, ring, n)
                    times[name].append(t)
            wt, wu, ws = (statistics.median(times[k]) for k in ("tcp", "unix", "shm"))
            row(n, rate(n, wt), rate(n, wu), rate(n, ws), wt / ws, wu / ws)
    finally:
        ring.stop()
        tcp.close()
        unix.close()


if __name__ == "__main__":
    main()
