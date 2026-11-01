"""SQLAlchemy read throughput, psycopg2 vs fastpostgres, across load sizes.

The same model and the same queries run on both drivers; only the engine URL
changes. Two phases, to show where the transport shows through:

  ORM   select(Model) -> Python objects   (hydration-bound)
  CORE  select(cols)  -> Row tuples       (driver-bound)

A third phase runs the fast paths this client offers against CORE on the same
rows, all fastpostgres-only: the same statement through the lean core interface
(no Result/Row machinery), the zero-copy column views (`stream()`), and CORE
across four processes (work parallelism). Each carries its improvement over CORE.

Usage: python3 benchmark/sqlalchemy_bench.py [nrows ...]
       (default 100000 1000000 10000000)
"""
import os
import statistics
import sys
import time
from multiprocessing import Pool

import fastpostgres.dialect  # noqa: F401  registers postgresql+fastpostgres
from sqlalchemy import (
    Boolean,
    Float,
    Integer,
    String,
    create_engine,
    select,
    text,
)
from sqlalchemy.orm import DeclarativeBase, Mapped, Session, mapped_column

BASE = os.environ.get("SQLALCHEMY_DB", "postgres@/postgres?host=/tmp")
URLS = {
    "psycopg2": f"postgresql+psycopg2://{BASE}",
    "fastpostgres": f"postgresql+fastpostgres://{BASE}",
}
DRIVERS = ("psycopg2", "fastpostgres")
REPS = 3
# Repeats are cheap for small loads; at 1M+ a single read is already slow, so one
# run suffices (and bounds the memory a full materialisation needs).
REPS_MAX_ROWS = 1_000_000


class Base(DeclarativeBase):
    pass


class Row(Base):
    __tablename__ = "fp_bench_rows"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    sku: Mapped[str] = mapped_column(String(32))
    name: Mapped[str] = mapped_column(String(64))
    price: Mapped[float] = mapped_column(Float)
    stock: Mapped[int] = mapped_column(Integer)
    active: Mapped[bool] = mapped_column(Boolean)


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


def setup(n):
    engine = create_engine(URLS["psycopg2"])
    Base.metadata.drop_all(engine)
    Base.metadata.create_all(engine)
    with engine.begin() as conn:
        conn.execute(
            text(
                """INSERT INTO fp_bench_rows (sku, name, price, stock, active)
                   SELECT 'SKU-' || g, 'Product ' || g, g * 0.5, g % 500, g % 2 = 0
                   FROM generate_series(1, :n) g"""
            ),
            {"n": n},
        )
    engine.dispose()


def read_orm(engine):
    t0 = now()
    with Session(engine) as session:
        rows = session.execute(select(Row)).scalars().all()
    ms = (now() - t0) * 1000
    return ms, len(rows)


def read_core(engine):
    t0 = now()
    with engine.connect() as conn:
        rows = conn.execute(
            select(Row.id, Row.sku, Row.name, Row.price, Row.stock, Row.active)
        ).all()
    ms = (now() - t0) * 1000
    return ms, len(rows)


def header(title):
    print(f"\n{title}: " + " ".join(col(s, 12, False) for s in (*DRIVERS, "vs psy")))


def pct(ratio):
    """Improvement over psycopg2, in percent (ratio is psy/fastpostgres time)."""
    return f"{(ratio - 1) * 100:+.1f}%"


def row(load, psy, fpg, ratio, ok):
    print(
        "  "
        + col(fmt(load), 8, False)
        + " "
        + col(f"{psy:.2f}", 11, True)
        + "M "
        + col(f"{fpg:.2f}", 11, True)
        + "M "
        + col(pct(ratio), 9, True)
        + "  "
        + ("ok" if ok else "MISMATCH")
    )


def measure(sizes, read):
    engines = {name: create_engine(url) for name, url in URLS.items()}
    header(f"SQLALCHEMY {'ORM' if read is read_orm else 'CORE'} READ (rows/s)")
    for n in sizes:
        setup(n)  # a fresh table of exactly n rows, so each read returns n
        times = {name: [] for name in DRIVERS}
        counts = {}
        reps = REPS if n <= REPS_MAX_ROWS else 1
        for r in range(reps):
            order = DRIVERS if r % 2 == 0 else DRIVERS[::-1]
            for name in order:
                ms, count = read(engines[name])
                times[name].append(ms)
                counts[name] = count
        psy = statistics.median(times["psycopg2"])
        fpg = statistics.median(times["fastpostgres"])
        ok = counts.get("psycopg2") == counts.get("fastpostgres") == n
        row(n, n / (psy / 1000) / 1e6, n / (fpg / 1000) / 1e6, psy / fpg, ok)
    for engine in engines.values():
        engine.dispose()


# ---- the fast paths -------------------------------------------------------


def conninfo(url):
    """The libpq conninfo behind a SQLAlchemy URL."""
    from sqlalchemy.engine import make_url

    from fastpostgres.dialect import PGDialect_fastpostgres

    args, _ = PGDialect_fastpostgres().create_connect_args(make_url(url))
    return " ".join(args)


_WORKER_ENGINE = None


def core_read_worker(_):
    """One CORE read in a process of its own: own GIL, own ring worker. The
    engine is made once per worker, so a warm round pays the bootstraps."""
    global _WORKER_ENGINE

    if _WORKER_ENGINE is None:
        _WORKER_ENGINE = create_engine(URLS["fastpostgres"])
    return read_core(_WORKER_ENGINE)[1]


def measure_paths(sizes):
    """The fast paths this client offers, against CORE on the same rows."""
    from fastpostgres import dbapi
    from fastpostgres.core import create_engine as lean_engine

    sql = "SELECT id, sku, name, price, stock, active FROM fp_bench_rows"
    cols = (Row.id, Row.sku, Row.name, Row.price, Row.stock, Row.active)

    print("\nFAST PATHS, fastpostgres only (rows/s):" + " " * 12 + "vs CORE")
    for n in sizes:
        setup(n)
        stmt = select(*cols)
        engine = create_engine(URLS["fastpostgres"])
        lean = lean_engine(URLS["fastpostgres"])
        dconn = dbapi.connect(conninfo(URLS["fastpostgres"]))
        dcur = dconn.cursor()

        def best_of(fn, reps=REPS):
            best = None
            for _ in range(reps):
                t0 = now()
                got = fn()
                ms = (now() - t0) * 1000
                if got != n:
                    raise SystemExit(f"fast path returned {got} rows, expected {n}")
                best = ms if best is None or ms < best else best
            return best

        def stream_read():
            total = 0
            for batch in dcur.stream(sql, size=100000):
                total += batch.row_count
                if batch.row_count:
                    # a typed view, summed in place: no object per value
                    import numpy as np

                    np.frombuffer(batch.columns[3].data, dtype="<f8").sum()
            return total

        core_ms = statistics.median(read_core(engine)[0] for _ in range(REPS))
        lean_ms = best_of(lambda: len(lean.connect().execute(stmt).all()))
        stream_ms = best_of(stream_read)

        with Pool(4) as pool:
            pool.map(core_read_worker, range(4))  # warm: engines, rings, plans
            t0 = now()
            counts = pool.map(core_read_worker, range(4))
            par_ms = (now() - t0) * 1000
        if sum(counts) != 4 * n:
            raise SystemExit("parallel reads returned the wrong row counts")

        core_rate = n / (core_ms / 1000) / 1e6
        for label, ms, rows in (
            ("SQLAlchemy CORE", core_ms, n),
            ("core stand-in (no Result/Row)", lean_ms, n),
            ("stream() views + numpy sum", stream_ms, n),
            ("CORE, 4 processes", par_ms, 4 * n),
        ):
            rate = rows / (ms / 1000) / 1e6
            pct = "" if ms is core_ms else f"{(rate / core_rate - 1) * 100:+.0f}%"
            print("  " + col(fmt(n), 8, False) + " " + col(label, 30, False)
                  + col(f"{rate:.2f}M", 11, True) + " " + col(pct, 9, True))

        dconn.close()
        lean.dispose()
        engine.dispose()


def main():
    sizes = [int(a) for a in sys.argv[1:] if a.isdigit() and int(a) > 0]
    if not sizes:
        sizes = [100000, 1000000, 10000000]

    measure(sizes, read_orm)
    measure(sizes, read_core)
    measure_paths(sizes)


if __name__ == "__main__":
    main()
