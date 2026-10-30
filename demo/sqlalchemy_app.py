"""A small, ordinary SQLAlchemy app.

Nothing here knows about the transport: the driver is chosen entirely by the
engine URL, so pointing an existing app at fastpostgres is a one-line change.

    python demo/sqlalchemy_app.py "postgresql+psycopg2://postgres@/postgres?host=/tmp" 5000
    python demo/sqlalchemy_app.py "postgresql+fastpostgres://postgres@/postgres?host=/tmp" 5000
"""
import datetime
import sys
import time

from sqlalchemy import (
    Boolean,
    DateTime,
    Float,
    Integer,
    String,
    create_engine,
    func,
    select,
)
from sqlalchemy.orm import DeclarativeBase, Mapped, Session, mapped_column

import fastpostgres.dialect  # noqa: F401  registers postgresql+fastpostgres


class Base(DeclarativeBase):
    pass


class Product(Base):
    __tablename__ = "fp_demo_products"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    sku: Mapped[str] = mapped_column(String(32), unique=True)
    name: Mapped[str] = mapped_column(String(64))
    price: Mapped[float] = mapped_column(Float)
    stock: Mapped[int] = mapped_column(Integer)
    active: Mapped[bool] = mapped_column(Boolean, default=True)
    created_at: Mapped[datetime.datetime] = mapped_column(DateTime(timezone=True))


def seed(session, n):
    session.add_all(
        Product(
            sku=f"SKU-{i:06d}",
            name=f"Product {i}",
            price=(i % 100) + 0.5,
            stock=(i * 7) % 500,
            active=(i % 3 != 0),
            created_at=datetime.datetime(2026, 1, 1, tzinfo=datetime.timezone.utc),
        )
        for i in range(n)
    )
    session.commit()


def main():
    url = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 5000

    engine = create_engine(url)
    driver = engine.dialect.driver

    Base.metadata.drop_all(engine)
    Base.metadata.create_all(engine)

    with Session(engine) as session:
        seed(session, n)

        t0 = time.perf_counter()
        products = session.execute(select(Product)).scalars().all()
        read_ms = (time.perf_counter() - t0) * 1000

        active = session.scalar(
            select(func.count()).select_from(Product).where(Product.active.is_(True))
        )

    print(f"driver={driver}")
    print(f"rows={len(products)}")
    print(f"stock_checksum={sum(p.stock for p in products)}")
    print(f"active={active}")
    print(f"read_ms={read_ms:.1f}")


if __name__ == "__main__":
    main()
