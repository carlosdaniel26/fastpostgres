"""fastpostgres.dbapi — a PEP 249 (DB-API 2.0) driver over the fastpostgres client.

The upstream seam every Python ORM sits on: hand this module to SQLAlchemy (or
anything else that speaks DB-API) and queries arrive here. Statements run on the
libpq control channel; `execute()` inlines parameters as literals, because the
shared-memory ring executes text and has no bind step (a small scanner leaves
`%s` inside string literals, identifiers, dollar-quotes and comments alone).

    import fastpostgres.dbapi as dbapi
    conn = dbapi.connect("host=/tmp dbname=postgres user=postgres")
    cur = conn.cursor()
    cur.execute("select %s::int + %s::int", (2, 3))
    cur.fetchone()            # (5,)
"""
import datetime
import decimal
import re
from typing import Any, Optional, Sequence

from . import _client

apilevel = "2.0"
threadsafety = 1
paramstyle = "format"

__all__ = [
    "apilevel",
    "threadsafety",
    "paramstyle",
    "connect",
    "Connection",
    "Cursor",
    "Date",
    "Time",
    "Timestamp",
    "DateFromTicks",
    "TimeFromTicks",
    "TimestampFromTicks",
    "Binary",
    "STRING",
    "BINARY",
    "NUMBER",
    "DATETIME",
    "ROWID",
    "Error",
]


# ---- type constructors (PEP 249) -----------------------------------------


def Date(year, month, day):
    return datetime.date(year, month, day)


def Time(hour, minute, second):
    return datetime.time(hour, minute, second)


def Timestamp(year, month, day, hour, minute, second):
    return datetime.datetime(year, month, day, hour, minute, second)


def DateFromTicks(ticks):
    return datetime.date.fromtimestamp(ticks)


def TimeFromTicks(ticks):
    return datetime.datetime.fromtimestamp(ticks).time()


def TimestampFromTicks(ticks):
    return datetime.datetime.fromtimestamp(ticks)


def Binary(data):
    return bytes(data)


class DBAPITypeObject:
    def __init__(self, *values):
        self.values = values

    def __eq__(self, other):
        return other in self.values

    def __ne__(self, other):
        return other not in self.values

    def __hash__(self):
        return hash(self.values)


STRING = DBAPITypeObject(18, 19, 25, 1042, 1043)
BINARY = DBAPITypeObject(17)
NUMBER = DBAPITypeObject(20, 21, 23, 26, 700, 701, 1700)
DATETIME = DBAPITypeObject(1082, 1083, 1114, 1184, 1266)
ROWID = DBAPITypeObject(26)


# ---- exception hierarchy (PEP 249) ---------------------------------------


class Warning(Exception):
    pass


class Error(Exception):
    pass


class InterfaceError(Error):
    pass


class DatabaseError(Error):
    pass


class DataError(DatabaseError):
    pass


class OperationalError(DatabaseError):
    pass


class IntegrityError(DatabaseError):
    pass


class InternalError(DatabaseError):
    pass


class ProgrammingError(DatabaseError):
    pass


class NotSupportedError(DatabaseError):
    pass


def _translate(exc):
    """Map a client error onto the DB-API hierarchy."""
    message = str(exc)
    if isinstance(exc, (ConnectionError, TimeoutError)):
        return OperationalError(message)
    if isinstance(exc, (TypeError, ValueError)):
        return ProgrammingError(message)
    if isinstance(exc, RuntimeError) and "libpq:" in message:
        lowered = message.lower()
        if "duplicate key" in lowered or "unique constraint" in lowered:
            return IntegrityError(message)
        if "not-null" in lowered or "null value in column" in lowered:
            return IntegrityError(message)
        if "violates foreign key" in lowered:
            return IntegrityError(message)
        return DatabaseError(message)
    return DatabaseError(message)


# ---- parameter inlining ---------------------------------------------------


_QUOTE_RE = re.compile(r"'(?:[^']|'')*'", re.S)
_DOUBLE_RE = re.compile(r'"(?:[^"]|"")*"', re.S)
_LINE_COMMENT_RE = re.compile(r"--[^\n]*")
_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)
_DOLLAR_RE = re.compile(r"\$(?:[A-Za-z_]\w*)?\$")


def _literal(value: Any) -> str:
    if value is None:
        return "NULL"
    if value is True:
        return "TRUE"
    if value is False:
        return "FALSE"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        return repr(value)
    if isinstance(value, decimal.Decimal):
        return str(value)
    if isinstance(value, (bytes, bytearray, memoryview)):
        return "'\\x" + bytes(value).hex() + "'::bytea"
    if isinstance(value, (datetime.date, datetime.time)):
        return "'" + value.isoformat() + "'"
    if isinstance(value, (list, tuple)):
        return "ARRAY[" + ", ".join(_literal(v) for v in value) + "]"
    return "'" + str(value).replace("'", "''") + "'"


_SKIP_RE = re.compile(
    "|".join(
        [
            _QUOTE_RE.pattern,
            _DOUBLE_RE.pattern,
            _LINE_COMMENT_RE.pattern,
            _BLOCK_COMMENT_RE.pattern,
        ]
    ),
    re.S,
)


def _inline(sql: str, params: Sequence[Any]) -> str:
    """Replace each `%s` with the next parameter's literal, leaving `%%` as `%`."""
    params = list(params)
    out = []
    i = 0
    n = len(sql)
    index = 0
    while i < n:
        if sql[i] == "$":
            m = _DOLLAR_RE.match(sql, i)
            if m:
                # dollar-quoted body: copy verbatim up to the closing tag
                close = sql.find(m.group(0), m.end())
                end = n if close == -1 else close + len(m.group(0))
                out.append(sql[i:end])
                i = end
                continue
        m = _SKIP_RE.match(sql, i)
        if m:
            out.append(m.group(0))
            i = m.end()
            continue
        if sql[i] == "%" and i + 1 < n:
            nxt = sql[i + 1]
            if nxt == "s":
                out.append(_literal(params[index]))
                index += 1
                i += 2
                continue
            if nxt == "%":
                out.append("%")
                i += 2
                continue
        out.append(sql[i])
        i += 1
    return "".join(out)


# ---- Connection / Cursor --------------------------------------------------


class Connection:
    """A DB-API connection over one libpq control channel (autocommit)."""

    def __init__(self, conninfo: str = "", **kwargs):
        if not conninfo and kwargs:
            conninfo = " ".join(f"{k}={v}" for k, v in kwargs.items() if v is not None)
        try:
            self._conn = _client.Connection(conninfo)
        except Exception as exc:  # pragma: no cover - exercised via connect()
            raise _translate(exc) from exc
        self._closed = False

    def cursor(self):
        return Cursor(self)

    # autocommit: each statement is its own transaction, so these are no-ops
    def commit(self):
        pass

    def rollback(self):
        pass

    def close(self):
        if not self._closed:
            self._conn.close()
            self._closed = True

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass


class Cursor:
    def __init__(self, connection: Connection):
        self.connection = connection
        self.arraysize = 1
        self.description: Optional[list] = None
        self.rowcount = -1
        self._rows: list = []
        self._pos = 0

    def execute(self, operation: str, parameters=None):
        sql = operation if parameters is None else _inline(operation, parameters)
        try:
            result = self.connection._conn.exec(sql)
        except Exception as exc:
            raise _translate(exc) from exc
        self.description = [(name, oid, None, None, None, None, None) for name, oid in zip(result.fields, result.field_types)]
        self._rows = result.rows
        self.rowcount = result.row_count  # rows returned (SELECT) or affected (DML)
        self._pos = 0
        return self

    def executemany(self, operation: str, seq_of_parameters):
        total = 0
        for parameters in seq_of_parameters:
            self.execute(operation, parameters)
            total += self.rowcount
        self.rowcount = total
        return self

    def fetchone(self):
        if self._pos >= len(self._rows):
            return None
        row = self._rows[self._pos]
        self._pos += 1
        return row

    def fetchmany(self, size=None):
        size = self.arraysize if size is None else size
        rows = self._rows[self._pos : self._pos + size]
        self._pos += len(rows)
        return rows

    def fetchall(self):
        rows = self._rows[self._pos :]
        self._pos = len(self._rows)
        return rows

    def __iter__(self):
        while True:
            row = self.fetchone()
            if row is None:
                return
            yield row

    def close(self):
        self._rows = []
        self._pos = 0
        self.description = None

    def setinputsizes(self, sizes):
        pass

    def setoutputsize(self, size, column=None):
        pass


def connect(conninfo: str = "", **kwargs) -> Connection:
    return Connection(conninfo, **kwargs)
