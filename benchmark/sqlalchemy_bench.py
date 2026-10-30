"""SQLAlchemy read throughput, psycopg2 vs fastpostgres, across load sizes.

The same model and the same queries run on both drivers; only the engine URL
changes. Two phases, to show where the transport shows through:

  ORM   select(Model) -> Python objects   (hydration-bound)
  CORE  select(cols)  -> Row tuples       (driver-bound)

Usage: python3 benchmark/sqlalchemy_bench.py [nrows ...]
       (default 100000 1000000 10000000)
"""
import os
import statistics
import sys
import time

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
    print(f"\n{title}: " + " ".join(col(s, 12, False) for s in (*DRIVERS, "fp/psy")))


def row(load, psy, fpg, ratio, ok):
    print(
        "  "
        + col(fmt(load), 8, False)
        + " "
        + col(f"{psy:.2f}", 11, True)
        + "M "
        + col(f"{fpg:.2f}", 11, True)
        + "M "
        + col(f"{ratio:.2f}", 6, True)
        + "x  "
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


def main():
    sizes = [int(a) for a in sys.argv[1:] if a.isdigit() and int(a) > 0]
    if not sizes:
        sizes = [100000, 1000000, 10000000]

    measure(sizes, read_orm)
    measure(sizes, read_core)


if __name__ == "__main__":
    main()
