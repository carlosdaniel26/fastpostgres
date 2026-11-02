"""One PgCache-comparison measurement, in a process of its own.

Run inside the dev image, as postgres, by benchmarks/harness/pgcache_bench.sh:

    player.py setup <n>
    player.py read <player> <workload> <n> <reps> [--cold]

`setup` builds the same table `just bench` reads (SHAPES and the model come from
benchmarks/rivals/sqlalchemy_bench.py) and adds the view the pass-through workload needs.
`read` runs one player against one workload and prints one whitespace-separated
line for the caller to format:

    median_ms min_ms max_ms hits misses warm_reads warm_ms count id_sum price_sum stock_sum

Two players, same output (Python tuples), both autocommit:

    fastpostgres  the shm ring, via the driver's own DBAPI
    pgcache       psycopg 3 to the PgCache proxy on 127.0.0.1:6432

`--cold` skips warming, so the one timed read is a cache miss: that is the fill.
"""
import os
import statistics
import sys
import time
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import psycopg  # noqa: E402
from fastpostgres import dbapi  # noqa: E402

from sqlalchemy_bench import CORE_SQL, SHAPES, SHAPE, setup  # noqa: E402

DATABASE_URL = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")
PGCACHE_URL = os.environ.get(
    "PGCACHE_URL", "host=127.0.0.1 port=6432 dbname=postgres user=postgres"
)
METRICS_URL = os.environ.get("PGCACHE_METRICS_URL", "http://127.0.0.1:9090/metrics")

VIEW = "fp_bench_v"
WORKLOADS = {
    # the existing bench's statement: a large result, delivery-bound
    "large": CORE_SQL,
    # one row out of n rows read: execution-bound, and materialized by the cache
    "small": "SELECT count(*), sum(price), sum(stock) FROM fp_bench_rows",
    # the same six columns through a view - PgCache forwards views, so this is
    # the proxy hop with no cache in front of it
    "through": f"SELECT id, sku, name, price, stock, active FROM {VIEW}",
}


def now():
    return time.perf_counter()


# ---- the players ---------------------------------------------------------


def open_fastpostgres():
    return dbapi.connect(DATABASE_URL)


def open_psycopg3(url):
    conn = psycopg.connect(url)
    conn.autocommit = True
    return conn


def read_rows(conn, sql):
    cur = conn.cursor()
    t0 = now()
    cur.execute(sql)
    rows = cur.fetchall()
    ms = (now() - t0) * 1000
    cur.close()
    return ms, rows


PLAYERS = {
    "fastpostgres": (open_fastpostgres, read_rows),
    "pgcache": (lambda: open_psycopg3(PGCACHE_URL), read_rows),
}


# ---- the cache's own counters --------------------------------------------


def scrape():
    """Every pgcache_* counter the proxy exports, as name -> value."""
    counters = {}
    with urllib.request.urlopen(METRICS_URL, timeout=10) as response:
        for line in response.read().decode().splitlines():
            if line.startswith("#") or not line.startswith("pgcache_"):
                continue
            name, _, value = line.rpartition(" ")
            try:
                counters[name] = float(value)
            except ValueError:
                continue
    return counters


def served_from_cache(before, after):
    """Did the read that just ran come from the cache?"""
    hit = "pgcache_queries_cache_hit"
    return after.get(hit, 0.0) > before.get(hit, 0.0)


def miss_total():
    return int(scrape().get("pgcache_queries_cache_miss", 0.0))


# A miss forwards to the origin and *starts* a population; the entry appears a
# moment later. Timing the next read as a hit means waiting for that moment, so
# the warm phase reads until the proxy says the read was served from cache - and
# reports how many reads and how long that took, because the population latency
# is part of what a result cache costs.
#
# One hit is not enough. The table was just built, so the origin's replication
# stream is still delivering that change, and the first hit can be invalidated
# by events already in flight: the timed read after it would be a miss. The warm
# phase therefore waits for the hit to hold still for WARM_STABLE_SECONDS - the
# state a cache is in between changes, which is the regime being measured. What
# a transient costs is the fill measurement, not this one.
WARM_DEADLINE_SECONDS = 120.0
WARM_STABLE_SECONDS = 2.0
WARM_POLL_SECONDS = 0.5


def warm_until_hit(handle, read, sql):
    started = now()
    reads = 0
    stable_since = None
    while True:
        before = scrape()
        read(handle, sql)
        reads += 1
        if served_from_cache(before, scrape()):
            if stable_since is None:
                stable_since = now()
            elif now() - stable_since >= WARM_STABLE_SECONDS:
                return reads, (now() - started) * 1000
        else:
            stable_since = None
        if now() - started > WARM_DEADLINE_SECONDS:
            return reads, (now() - started) * 1000
        time.sleep(WARM_POLL_SECONDS)


# ---- the output check ----------------------------------------------------


def digest(rows, workload):
    """Order-insensitive checksums of what came back.

    A cached result set is not guaranteed to arrive in the origin's scan order,
    so the check is on the multiset: count, and sums over the columns. `price` is
    g * 0.5 in both shapes and every partial sum of those is exactly
    representable in a double, so these sums are order-independent.
    """
    if workload == "small":
        count, price_sum, stock_sum = rows[0]
        return len(rows), int(count), float(price_sum), int(stock_sum or 0)
    id_sum = 0
    price_sum = 0.0
    stock_sum = 0
    for row in rows:
        id_sum += row[0]
        price_sum += row[3]
        stock_sum += row[4] or 0
    return len(rows), id_sum, price_sum, stock_sum


# ---- the two subcommands -------------------------------------------------


def do_setup(n):
    conn = psycopg.connect(DATABASE_URL)
    conn.autocommit = True
    with conn.cursor() as cur:
        cur.execute(f"DROP VIEW IF EXISTS {VIEW}")
    conn.close()
    setup(n)  # the same drop/create/insert `just bench` uses
    conn = psycopg.connect(DATABASE_URL)
    conn.autocommit = True
    with conn.cursor() as cur:
        cur.execute(f"CREATE VIEW {VIEW} AS {CORE_SQL}")
    conn.close()


def do_read(player, workload, n, reps, cold, evidence=None):
    sql = WORKLOADS[workload]
    open_handle, read = PLAYERS[player]
    handle = open_handle()
    try:
        misses_before = miss_total() if player == "pgcache" else 0
        warm_reads = warm_ms = 0
        if not cold:
            if player == "pgcache" and workload != "through":
                # views are never cached, so there is no hit to wait for
                warm_reads, warm_ms = warm_until_hit(handle, read, sql)
            else:
                read(handle, sql)  # warm: connections, rings, plans
        warm_misses = (miss_total() - misses_before) if player == "pgcache" else 0
        ring_before = getattr(handle, "shm_reads", 0)
        times = []
        rows = None
        for _ in range(reps):
            ms, rows = read(handle, sql)
            times.append(ms)
        if evidence is not None:
            # the timed reads that actually took the ring: one that fell back to
            # libpq is not counted, which is what lets a caller notice the
            # fallback instead of measuring it as if it were the ring
            evidence["shm_reads"] = getattr(handle, "shm_reads", 0) - ring_before
        rep_misses = (miss_total() - misses_before - warm_misses) if player == "pgcache" else 0
        check = digest(rows, workload)
        if len(rows) != (1 if workload == "small" else n):
            raise SystemExit(
                f"{player}/{workload}: {len(rows)} rows came back, wanted "
                f"{1 if workload == 'small' else n}"
            )
    finally:
        handle.close()

    fields = [
        f"{statistics.median(times):.3f}",
        f"{min(times):.3f}",
        f"{max(times):.3f}",
        str(warm_misses),
        str(rep_misses),
        str(warm_reads),
        f"{warm_ms:.0f}",
        str(check[0]),
        str(check[1]),
        f"{check[2]:.1f}",
        str(check[3]),
    ]
    return fields


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    if argv[1] == "setup":
        do_setup(int(argv[2]))
        return
    if argv[1] == "read":
        player, workload = argv[2], argv[3]
        n, reps = int(argv[4]), int(argv[5])
        if player not in PLAYERS:
            raise SystemExit(f"unknown player {player!r}: {sorted(PLAYERS)}")
        if workload not in WORKLOADS:
            raise SystemExit(f"unknown workload {workload!r}: {sorted(WORKLOADS)}")
        if SHAPE not in SHAPES:
            raise SystemExit(f"SHAPE={SHAPE!r}: choose one of {sorted(SHAPES)}")
        fields = do_read(player, workload, n, reps, "--cold" in argv)
        print(" ".join(fields))
        return
    raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
