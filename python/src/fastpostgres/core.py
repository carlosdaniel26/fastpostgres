"""fastpostgres.core — the lean interface: a statement in, tuples out.

A statement goes in, ``.all()`` comes out, the connection is a context manager,
and the engine takes a URL or a conninfo — the call shape SQLAlchemy users know,
minus the per-row machinery: no result proxy, no ``Row`` objects, no attribute
mapping. ``execute()`` compiles the statement once (SQLAlchemy's compiler, and
cached) and runs the SQL through the DB-API driver, so ``.all()`` is the
driver's list of tuples.

    from fastpostgres.core import create_engine
    engine = create_engine("host=/tmp dbname=postgres user=postgres")
    with engine.connect() as conn:
        rows = conn.execute(select(Row.id, Row.sku)).all()   # [(1, 'SKU-1'), ...]

Rows are tuples: index them, unpack them, or ask ``result.keys()`` for the names.
``row.sku`` is the one thing this does not do, because that wrapper is the per-row
cost it exists to avoid.

One connection, reused (no pool): a read path does not need one, and opening a
libpq connection per ``connect()`` would cost milliseconds. Nothing here is
thread-safe.
"""
from . import dbapi
from .dbapi import _compiled

__all__ = ["create_engine", "Engine", "Connection", "Result"]


class Result:
    """The rows, already a list, and their names."""

    __slots__ = ("_rows", "_keys", "_rowcount")

    def __init__(self, rows, keys, rowcount):
        self._rows = rows
        self._keys = keys
        self._rowcount = rowcount

    def all(self):
        return self._rows

    fetchall = all

    def first(self):
        return self._rows[0] if self._rows else None

    def keys(self):
        return self._keys

    @property
    def rowcount(self):
        return self._rowcount

    def __iter__(self):
        return iter(self._rows)

    def __len__(self):
        return len(self._rows)

    def __repr__(self):
        return f"Result(rows={len(self._rows)})"


class Connection:
    """CORE's connection: ``execute``, the context-manager protocol, and the
    transaction no-ops the driver's autocommit makes of them."""

    __slots__ = ("_conn", "_engine")

    def __init__(self, engine, conn):
        self._engine = engine
        self._conn = conn

    def execute(self, statement, **params):
        if params and not isinstance(statement, str):
            statement = statement.params(**params)
        cur = self._conn.cursor()
        cur.execute(_compiled(statement))
        rows = cur.fetchall()
        keys = tuple(column[0] for column in (cur.description or ()))
        return Result(rows, keys, cur.rowcount)

    def close(self):
        pass  # the engine keeps the one connection; close() is not its owner

    def commit(self):
        pass

    def rollback(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class Engine:
    """Holds the connection string — and the one connection, opened on demand."""

    __slots__ = ("_conninfo", "_conn")

    def __init__(self, conninfo: str):
        self._conninfo = conninfo
        self._conn = None

    def connect(self):
        if self._conn is None:
            self._conn = dbapi.connect(self._conninfo)
        return Connection(self, self._conn)

    begin = connect

    def dispose(self):
        if self._conn is not None:
            self._conn.close()
            self._conn = None


def create_engine(url_or_conninfo: str, **kwargs) -> Engine:
    """A lean engine for a libpq conninfo string, or a SQLAlchemy URL
    (``postgresql+fastpostgres://…``); other keyword arguments are ignored."""
    if "://" in url_or_conninfo:
        from sqlalchemy.engine import make_url

        from .dialect import PGDialect_fastpostgres

        args, _ = PGDialect_fastpostgres().create_connect_args(make_url(url_or_conninfo))
        url_or_conninfo = " ".join(args)
    return Engine(url_or_conninfo)
