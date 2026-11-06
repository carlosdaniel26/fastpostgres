#!/usr/bin/env python3
"""ClickBench, on the same rows, for every player.

The rivals comparison, rebuilt on a workload nobody here invented: ClickBench's
`hits` table and its 43 queries (vendored verbatim in this directory), run
against fast-postgres, ADBC + Arrow and psycopg 3.

The model matters as much as the data. ClickBench scores queries per second, and
a query per second is only comparable when both sides get the same machine, so
this harness runs **C clients of each player at once**, each walking the whole
set once, and reports:

    q/s       C * 43 / wall        ClickBench's metric, at that concurrency
    rows/s    rows / wall          what the players actually delivered

C defaults to one physical core less one. In that mode the ring's own fan-out is
switched off (`FASTPOSTGRES_PARALLEL=1`): one client is one backend on each side,
so neither player gets extra cores the other does not have.

    python3 run.py                      # C clients, whole set, one rep
    python3 run.py --clients 1          # the latency regime
    python3 run.py --reps 3             # median of three walks per client
    python3 run.py --only "Q1 Q2"       # a subset, in query order

Data comes from `fetch.py` (git-ignored, downloaded once). Loading is a single
`COPY` from that TSV; the table is dropped and re-created on every run so the
numbers never depend on what a previous run left behind.
"""
from __future__ import annotations

import argparse
import os
import shlex
import statistics
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
CREATE = HERE / "create.sql"
QUERIES = HERE / "queries.sql"
TSV = HERE / "data" / "hits.tsv"

CONNINFO = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")


def physical_cores() -> int:
    n = len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else (os.cpu_count() or 4)
    return max(1, n // 2)


def queries(only: list[str] | None = None) -> list[tuple[str, str]]:
    """[(name, sql)], in file order; the file is upstream's, never edited here."""
    out = []
    text = QUERIES.read_text()
    lines, current = [], []
    for line in text.splitlines():
        if line.strip().startswith("--") or not line.strip():
            continue
        current.append(line)
        if line.rstrip().endswith(";"):
            out.append((" ".join(current).rstrip(";")))
            current = []
    if current:
        out.append(" ".join(current))
    named = [(f"Q{i + 1}", sql) for i, sql in enumerate(out)]
    if only:
        wanted = {q if q.startswith("Q") else f"Q{q}" for q in only}
        named = [(name, sql) for name, sql in named if name in wanted]
    return named


def load() -> int:
    """Drop, create and COPY `hits`. Returns the row count."""
    import psycopg

    if not TSV.exists():
        raise SystemExit(f"run: {TSV} is missing -- python3 {HERE}/fetch.py")

    with psycopg.connect(CONNINFO, autocommit=True) as conn:
        with conn.cursor() as cur:
            cur.execute("DROP TABLE IF EXISTS hits")
            cur.execute(CREATE.read_text())
            cols = 105
            with TSV.open("rb") as f:
                f.readline()  # upstream's header names the columns; COPY takes data only
                with cur.copy(f"COPY hits FROM STDIN") as copy:
                    for line in f:
                        copy.write(line)
            cur.execute("SELECT count(*) FROM hits")
            rows = cur.fetchone()[0]
        cur = conn.cursor()
        cur.execute("ANALYZE hits")
        cur.close()
    print(f"load: hits {rows:,} rows from {TSV.name}")
    return rows


# ---- the players ---------------------------------------------------------


def open_fastpostgres():
    import fastpostgres.dbapi as dbapi

    return dbapi.connect(CONNINFO)


def _adbc_uri() -> str:
    """ADBC wants a URI, the rest of the tree speaks libpq conninfo."""
    kv = dict(part.split("=", 1) for part in shlex.split(CONNINFO) if "=" in part)
    return f"postgresql://{kv.get('user', 'postgres')}@/{kv.get('dbname', 'postgres')}?host={kv.get('host', '/tmp')}"


def open_adbc():
    import adbc_driver_postgresql.dbapi as adbc

    return adbc.connect(_adbc_uri())


def open_psycopg():
    import psycopg

    return psycopg.connect(CONNINFO)


def read_fastpostgres(conn, sql: str) -> int:
    cur = conn.cursor()
    cur.execute(sql)
    rows = cur.fetchall()          # the lazy ResultSet: no tuple per row until asked
    n = len(rows)
    cur.close()
    return n


def read_adbc(conn, sql: str) -> int:
    cur = conn.cursor()
    cur.execute(sql)
    table = cur.fetch_arrow_table()
    cur.close()
    return table.num_rows


def read_psycopg(conn, sql: str) -> int:
    cur = conn.cursor()
    cur.execute(sql)
    rows = cur.fetchall()
    cur.close()
    return len(rows)


PLAYERS = {
    "fastpostgres": (open_fastpostgres, read_fastpostgres),
    "adbc": (open_adbc, read_adbc),
    "psycopg": (open_psycopg, read_psycopg),
}


def walk(player: str, queries_: list[tuple[str, str]], reps: int) -> tuple[float, int, list[float]]:
    """One client: connect, walk the whole set `reps` times.

    Returns (seconds, rows, per-query ms), where `seconds` is the sum of each
    query's fastest run — ClickBench's own rule — and `rows` is one full walk's
    delivery. A warm query runs first so connection setup, plan caching and the
    first-page faults are not billed to the set.
    """
    opener, reader = PLAYERS[player]
    conn = opener()
    try:
        reader(conn, queries_[0][1])
        best = [float("inf")] * len(queries_)
        rows = 0
        for _ in range(reps):
            total = 0
            for i, (_name, sql) in enumerate(queries_):
                t0 = time.perf_counter()
                total += reader(conn, sql)
                best[i] = min(best[i], (time.perf_counter() - t0) * 1000)
            rows = total
        return sum(best) / 1000.0, rows, best
    finally:
        conn.close()


def run_player(player: str, clients: int, queries_, reps: int) -> list[tuple[float, int, list[float]]]:
    with ThreadPoolExecutor(max_workers=clients) as pool:
        return list(pool.map(lambda _: walk(player, queries_, reps), range(clients)))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clients", type=int, default=max(1, min(8, physical_cores() - 1)))
    ap.add_argument("--reps", type=int, default=1, help="walks of the whole set per client (default 1)")
    ap.add_argument("--only", nargs="+", metavar="Q", help="query names, e.g. --only Q1 Q7")
    ap.add_argument("--skip-load", action="store_true", help="use the table as it stands")
    args = ap.parse_args()

    if args.clients < 1 or args.reps < 1:
        raise SystemExit("run: --clients and --reps must be positive")

    picked = queries(args.only)
    if not picked:
        raise SystemExit("run: no queries selected")

    # Fair by construction: one client is one backend on every side, so the
    # ring's fan-out is off in this mode. Set before any connection is opened.
    os.environ["FASTPOSTGRES_PARALLEL"] = "1"

    if not args.skip_load:
        load()

    # No subprocess here: the ring's worker is a server-side background worker.
    print(
        f"\nClickBench: {len(picked)} queries, {args.clients} clients, {args.reps} rep(s), "
        f"shape {TSV.name}"
    )
    print(f"{'player':<14}{'wall s':>9}{'q/s':>10}{'rows/s':>12}")

    results = {}
    for player in PLAYERS:
        runs = run_player(player, args.clients, picked, args.reps)
        wall = max(r[0] for r in runs)                 # the slowest client sets the wall
        rows = sum(r[1] for r in runs)
        qps = args.clients * len(picked) / wall
        results[player] = (wall, qps, rows / wall, runs)
        print(f"{player:<14}{wall:>9.2f}{qps:>10.1f}{rows / wall:>12,.0f}")

    base = results["fastpostgres"][1]
    for player in ("adbc", "psycopg"):
        print(f"  fastpostgres vs {player}: {100 * base / results[player][1]:.1f}% of its q/s")

    # Per-query, the median client's ms: where the ring wins and where it loses.
    print(f"\n{'query':<7}{'fastpostgres':>14}{'adbc':>10}{'psycopg':>10}   (ms, median client)")
    for i, (name, _sql) in enumerate(picked):
        cells = []
        for player in ("fastpostgres", "adbc", "psycopg"):
            per_client = [runs[2][i] for runs in results[player][3]]
            cells.append(statistics.median(per_client))
        print(f"{name:<7}{cells[0]:>14.2f}{cells[1]:>10.2f}{cells[2]:>10.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
