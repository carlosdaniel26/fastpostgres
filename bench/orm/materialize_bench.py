#!/usr/bin/env python3
"""Stock SQLAlchemy ORM hydration vs fastpostgres's native materializer.

Same rows, same 100k-row table, same driver underneath: the ORM phase reads
``select(Row)`` through ``postgresql+fastpostgres`` and hydrates entities the
SQLAlchemy way; the materializer reads the same columns as the driver's tuples
and fills flat ``Row`` instances in C++ (``fastpostgres.orm.materialize``).

Three cells, each a warm read and the median of ``REPS`` timed ones:

  stock ORM         session + Result + identity map + per-attribute hydration
  materialize e2e   the driver read + the native instance fill
  materialize only  the same fill on rows already in memory

A fourth cell, when the materializer is present, prices the fallback contract:
a model with a relationship is returned as stock, instrumented instances, so it
is expected to cost about what the ORM's object construction costs.

The baseline image has no ``fastpostgres.orm``; there the script prints the
stock ORM cell and reports the materializer as unavailable, so the same command
produces the before and the after block.

Usage (inside the dev image):
    python3 bench/orm/materialize_bench.py [rows] [reps]
"""
import gc
import os
import statistics
import sys
import time

# The image's tree; mounted scripts and the repo checkout both resolve to it.
sys.path.insert(0, "/src/bench")

from lib.dataset import CORE_SQL, Row, setup  # noqa: E402
from sqlalchemy import ForeignKey, Integer, Text, select  # noqa: E402
from sqlalchemy.orm import (  # noqa: E402
    DeclarativeBase,
    Mapped,
    Session,
    configure_mappers,
    mapped_column,
    relationship,
)
from sqlalchemy.orm.exc import DetachedInstanceError  # noqa: E402

import fastpostgres.dialect  # noqa: F401,E402  registers postgresql+fastpostgres
from fastpostgres import dbapi  # noqa: E402
from sqlalchemy import create_engine  # noqa: E402


class FallbackBase(DeclarativeBase):
    pass


class FallbackParent(FallbackBase):
    """A model the fast path must refuse: it has a relationship."""

    __tablename__ = "fp_orm_fallback_bench"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    name: Mapped[str | None] = mapped_column(Text)
    children = relationship("FallbackChild", back_populates="parent")


class FallbackChild(FallbackBase):
    __tablename__ = "fp_orm_fallback_child_bench"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    parent_id: Mapped[int] = mapped_column(ForeignKey("fp_orm_fallback_bench.id"))
    parent = relationship("FallbackParent", back_populates="children")


DATABASE_URL = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")
ORM_URL = "postgresql+fastpostgres://postgres@/postgres?host=/tmp"

try:
    from fastpostgres.orm import materialize
except ImportError as exc:  # the baseline image, by construction
    materialize = None
    MATERIALIZER_MISSING = str(exc)
else:
    MATERIALIZER_MISSING = None


def now():
    return time.perf_counter()


def rate(seconds, rows):
    return rows / seconds


def report(label, seconds, rows):
    ns = seconds * 1e9 / rows
    print(f"{label:20s} {ns:9.1f} ns/row  {rate(seconds, rows) / 1e6:7.2f}M rows/s")


def timed(fn, rows, reps):
    """One warm call, then the median of `reps` calls (dealloc outside timing)."""
    out = fn()  # warm
    del out
    gc.collect()
    times = []
    for _ in range(reps):
        t0 = now()
        out = fn()
        times.append(now() - t0)
        del out
        gc.collect()
    return statistics.median(times)


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 100_000
    reps = int(sys.argv[2]) if len(sys.argv) > 2 else 3

    setup(n)
    configure_mappers()
    keys = tuple(attr.key for attr in Row.__mapper__.column_attrs)

    engine = create_engine(ORM_URL)
    conn = dbapi.connect(DATABASE_URL)

    stock_sql = str(select(Row).compile(engine))
    # The materializer reads the same six columns; CORE_SQL is the benchmark's
    # canonical column list and matches select(Row)'s projection and order.
    assert CORE_SQL.split(" FROM ")[0].replace("SELECT ", "") == ", ".join(keys), (
        CORE_SQL,
        keys,
    )

    def read_orm():
        with Session(engine) as session:
            return session.execute(select(Row)).scalars().all()

    def read_driver():
        cur = conn.cursor()
        cur.execute(CORE_SQL)
        return cur.fetchall()

    def read_materialize():
        cur = conn.cursor()
        cur.execute(CORE_SQL)
        rows = cur.fetchall()
        return materialize(Row, rows, keys)

    # Validate before timing: both paths must produce the same values, and the
    # instances must behave like the model for column access.
    orm_rows = read_orm()
    if materialize is not None:
        mat_rows = read_materialize()
        assert len(orm_rows) == len(mat_rows) == n
        for a, b in zip(orm_rows, mat_rows):
            assert type(b) is Row and isinstance(b, Row)
            for key in keys:
                assert getattr(a, key) == getattr(b, key), (
                    key,
                    getattr(a, key),
                    getattr(b, key),
                )
            assert set(vars(b)) == set(keys)
        del mat_rows
    del orm_rows
    gc.collect()

    if materialize is not None:
        # The rows for the materialize-only cell, fetched once and reused.
        cur = conn.cursor()
        cur.execute(stock_sql)
        preloaded = cur.fetchall()
        assert len(preloaded) == n

        # The fallback contract, checked before it is timed: a relationship
        # model must come back instrumented and detached, not as a half-ORM.
        fallback_rows = [(i, f"Product {i}") for i in range(1, n + 1)]
        fallback = materialize(FallbackParent, fallback_rows, ("id", "name"))
        assert len(fallback) == n
        assert "_sa_instance_state" in vars(fallback[0])
        try:
            fallback[0].children
        except DetachedInstanceError:
            pass
        else:
            raise AssertionError("relationship model did not fall back to stock")
        del fallback
        gc.collect()

    gc.disable()
    try:
        t_driver = timed(read_driver, n, reps)
        t_orm = timed(read_orm, n, reps)
        if materialize is not None:
            t_e2e = timed(read_materialize, n, reps)
            t_only = timed(lambda: materialize(Row, preloaded, keys), n, reps)
            t_fallback = timed(
                lambda: materialize(FallbackParent, fallback_rows, ("id", "name")),
                n,
                reps,
            )
    finally:
        gc.enable()

    print("\n### ORM MATERIALIZE")
    print(f"rows: {n}  reps: {reps}  shape: {os.environ.get('SHAPE') or 'bench'}")
    print(f"engine: {ORM_URL}")
    report("driver tuples", t_driver, n)
    report("stock ORM", t_orm, n)
    if materialize is None:
        print(f"materialize:        unavailable ({MATERIALIZER_MISSING})")
    else:
        report("materialize e2e", t_e2e, n)
        report("materialize only", t_only, n)
        report("materialize fallback", t_fallback, n)
        print(
            f"speedup e2e:        {t_orm / t_e2e:.2f}x"
            f"  entity-only: {t_orm / t_only:.2f}x"
        )
    print("### END ORM MATERIALIZE")

    conn.close()
    engine.dispose()


if __name__ == "__main__":
    main()
