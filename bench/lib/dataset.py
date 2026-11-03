"""The generated table both rival comparisons read: fp_bench_rows.

One fixture, so the comparisons agree on what they measured: the ADBC + Arrow
comparison reads these rows to price delivery, the PgCache comparison reads them
to price a cache in front of the same server. `setup(n)` rebuilds the table at
exactly n rows - every read in either comparison returns n rows - and SHAPES
picks the data's shape, fixed at import from SHAPE.

The model is SQLAlchemy's because the harness's own DDL and inserts are the
psycopg path (SETUP_URL); the players read the table through their own
transports.
"""
import os

from sqlalchemy import Boolean, Float, Integer, Text, create_engine, text
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column

BASE = os.environ.get("SQLALCHEMY_DB", "postgres@/postgres?host=/tmp")
SETUP_URL = f"postgresql+psycopg://{BASE}"  # the harness's own DDL and inserts
# The columns both players read, in one place.
CORE_SQL = "SELECT id, sku, name, price, stock, active FROM fp_bench_rows"
# The data behind those columns. The shape is selectable because the row path's
# cost is shape-dependent: `bench` is the friendly shape the transport was built
# against (short ASCII text, no NULLs), `mixed` adds what a real table has -
# NULLs, text on both sides of the 126-byte varlena split, non-ASCII values.
SHAPES = {
    "bench": (
        "short ASCII text, no NULLs",
        """INSERT INTO fp_bench_rows (sku, name, price, stock, active)
           SELECT 'SKU-' || g, 'Product ' || g, g * 0.5, g % 500, g % 2 = 0
           FROM generate_series(1, :n) g""",
    ),
    "mixed": (
        "~10% NULLs, text 1..54 and 130..200 bytes, a non-ASCII every eighth row",
        """INSERT INTO fp_bench_rows (sku, name, price, stock, active)
           SELECT
             CASE WHEN g % 10 = 0 THEN NULL
                  WHEN g % 8 = 0 THEN repeat('é', 1 + g % 12)
                  ELSE 'SKU-' || g END,
             CASE WHEN g % 7 = 0 THEN repeat('x', 130 + g % 70)
                  WHEN g % 10 = 0 THEN NULL
                  ELSE repeat('y', 1 + g % 54) END,
             g * 0.5,
             CASE WHEN g % 10 = 0 THEN NULL ELSE g % 500 END,
             g % 2 = 0
           FROM generate_series(1, :n) g""",
    ),
}
SHAPE = os.environ.get("SHAPE") or "bench"


class Base(DeclarativeBase):
    pass


class Row(Base):
    __tablename__ = "fp_bench_rows"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    # text, not varchar(n), and nullable: the `mixed` shape has NULLs and values
    # on both sides of the varlena split; PG stores both text kinds as varlena,
    # so this is the same wire shape as before for the `bench` shape
    sku: Mapped[str | None] = mapped_column(Text)
    name: Mapped[str | None] = mapped_column(Text)
    price: Mapped[float] = mapped_column(Float)
    stock: Mapped[int | None] = mapped_column(Integer)
    active: Mapped[bool] = mapped_column(Boolean)


def setup(n):
    engine = create_engine(SETUP_URL)
    Base.metadata.drop_all(engine)
    Base.metadata.create_all(engine)
    with engine.begin() as conn:
        conn.execute(text(SHAPES[SHAPE][1]), {"n": n})
    engine.dispose()
