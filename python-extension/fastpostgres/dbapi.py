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

For a read that only wants rows, `Connection.fetchall(statement)` compiles a
SQLAlchemy statement with SQLAlchemy's own compiler and returns the driver's
tuples — the CORE read without CORE's per-row Result/Row machinery:

    conn.fetchall(select(Row.id, Row.sku))    # [(1, 'SKU-1'), (2, 'SKU-2')]
"""
import datetime
import decimal
import os
import queue
import re
import threading
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


_PGDIALECT = None


def _compiled(statement) -> str:
    """A SQLAlchemy statement as the SQL text to run, or SQL text as it is.

    Literal binds, because the ring has no bind step and `execute()` would inline
    the values anyway — and the compile itself is build-time work, not per row.
    One dialect instance for the process, so a statement compiled again is a hit
    in SQLAlchemy's own compiled cache rather than a fresh compile.
    """
    global _PGDIALECT

    if isinstance(statement, str):
        return statement
    if _PGDIALECT is None:
        from sqlalchemy.dialects import postgresql

        _PGDIALECT = postgresql.dialect()
    return str(statement.compile(dialect=_PGDIALECT, compile_kwargs={"literal_binds": True}))


# ---- Connection / Cursor --------------------------------------------------

_SHM_BATCH = 100000
_READ_RE = re.compile(r"^(select|values|table)\b", re.I | re.S)


def _is_read_only(sql: str) -> bool:
    """True for statements the ring's read-only cursor can serve."""
    s = sql.strip()
    if s.endswith(";"):
        s = s[:-1]
    if ";" in s:  # more than one statement
        return False
    if _READ_RE.match(s) or s.startswith("("):
        return True
    if re.match(r"^with\b", s, re.I | re.S):
        return not re.search(r"\b(insert|update|delete|merge)\b", s, re.I)
    return False


class _Result:
    __slots__ = ("fields", "field_types", "rows", "row_count")

    def __init__(self, fields, field_types, rows, row_count):
        self.fields = fields
        self.field_types = field_types
        self.rows = rows
        self.row_count = row_count


# ---- ring pool ------------------------------------------------------------
#
# A ring is a worker-backed channel that serves one stream at a time, so it is
# shared process-wide instead of being tied to a connection: a read checks one
# out, runs open/next*/close, and hands it back. The pool bounds how many
# workers a process can spawn, whatever the connection pool size; when it is
# exhausted (or the extension is absent) reads fall back to libpq.

_SHM_POOL_SIZE = int(os.environ.get("FASTPOSTGRES_SHM_POOL", "4"))

_pool_idle = queue.Queue()
_pool_lock = threading.Lock()
_pool_created = 0
_pool_broken = False
_pool_pid = None
_pool_prefix = ""


def _prefix() -> str:
    """The ring/segment name stem for *this* process.

    Module state is inherited across fork — gunicorn, uwsgi and multiprocessing all
    import first and fork after — so a stem computed at import time gives every
    worker the parent's name, and two processes on one ring means two background
    workers on one data segment. A pid change means we are the child: take a fresh
    stem and forget the parent's pool, because its rings are not ours to hand out.
    """
    global _pool_pid, _pool_prefix, _pool_created, _pool_broken

    pid = os.getpid()
    if _pool_pid != pid:
        _pool_pid = pid
        _pool_prefix = f"fp_dbapi_{pid}"
        _pool_created = 0
        _pool_broken = False
        while True:
            try:
                _pool_idle.get_nowait()
            except queue.Empty:
                break
    return _pool_prefix


def pool_size():
    """Maximum number of worker-backed rings this process will create."""
    return _SHM_POOL_SIZE


def pool_created():
    """Rings created so far (idle + checked out)."""
    return _pool_created


def _ring_acquire(bootstrap):
    """Check out a (ring, segment) pair; None when there is none to give."""
    global _pool_created, _pool_broken
    prefix = _prefix()  # per process: a forked child must not reuse the parent's
    if _pool_broken:
        return None
    try:
        return _pool_idle.get_nowait()
    except queue.Empty:
        pass

    with _pool_lock:
        if _pool_created >= _SHM_POOL_SIZE:
            return None
        index = _pool_created
        _pool_created += 1

    name = f"{prefix}_{index}"
    try:
        ring = _client.Ring(name)
        ring.start(bootstrap)
    except Exception:
        with _pool_lock:
            _pool_created -= 1
            _pool_broken = True
        return None
    return (ring, name + "_seg")


def _ring_release(info):
    _pool_idle.put_nowait(info)


class Connection:
    """A DB-API connection over one libpq control channel (autocommit).

    Read-only SELECTs are served by the /dev/shm ring; everything else (DML,
    DDL, SET, ...) takes libpq. If the extension is absent the ring fails to
    start and every statement stays on libpq.
    """

    def __init__(self, conninfo: str = "", shm: bool = True, **kwargs):
        if not conninfo and kwargs:
            conninfo = " ".join(f"{k}={v}" for k, v in kwargs.items() if v is not None)
        try:
            self._conn = _client.Connection(conninfo)
        except Exception as exc:  # pragma: no cover - exercised via connect()
            raise _translate(exc) from exc
        self._closed = False
        self._shm = shm
        self.shm_reads = 0

    def _shm_read(self, sql):
        if not self._shm:
            return None
        info = _ring_acquire(self._conn)
        if info is None:
            return None  # pool exhausted, or the extension is absent
        ring, seg = info
        try:
            fields = field_types = None
            rows = []
            # Two segments alternate: the request for the next batch is posted
            # before the current one is decoded, so the worker's turn is hidden
            # behind the decode instead of adding to it. One request is in flight
            # at a time, and a segment is only reused once it has been decoded.
            segs = (seg + "_a", seg + "_b")
            pending = False
            ring.open(seg, sql)  # the stream; its batches go to segs[i]
            try:
                i = 0
                ring.post_rows(segs[0], _SHM_BATCH)
                pending = True
                n = ring.wait()
                pending = False
                while True:
                    more = n != 0
                    if more:
                        ring.post_rows(segs[(i + 1) & 1], _SHM_BATCH)
                        pending = True
                    chunk = _client.read_rows(segs[i & 1])
                    if fields is None:
                        fields, field_types = chunk.fields, chunk.field_types
                    rows.extend(chunk.rows)
                    if not more:
                        break
                    n = ring.wait()
                    pending = False
                    i += 1
            finally:
                if pending:  # an exception after a post: leave the ring balanced
                    try:
                        ring.wait()
                    except Exception:
                        pass
                ring.close()
        except Exception:
            return None  # re-run on libpq so the caller sees the real error
        finally:
            _ring_release(info)
        self.shm_reads += 1
        return _Result(fields or [], field_types or [], rows, len(rows))

    def cursor(self):
        return Cursor(self)

    # ---- the CORE read without CORE --------------------------------------
    #
    # SQLAlchemy's CORE layer costs ~375 ns/row on the benchmark's shape, all of
    # it its own Result and Row machinery once the driver is subtracted. These
    # three do the same job — a statement in, its rows out — by compiling once
    # and running the plain SQL the ring already speaks, so what comes back is
    # the driver's list of tuples: no Result, no Row, nothing per row.
    #
    #     rows = conn.fetchall(select(Row.id, Row.sku))
    #
    # `params` are bound into the statement before compiling (they are literals
    # in the SQL, which is what the ring requires); SQL text passes through.

    def compile(self, statement, **params) -> str:
        """The SQL text a statement would run, for logging or a plain cursor."""
        if params:
            if isinstance(statement, str):
                raise ValueError("compile(sql, **params): bind them into the SQL text yourself")
            statement = statement.params(**params)
        return _compiled(statement)

    def fetchall(self, statement, **params) -> list:
        """Run a SQLAlchemy statement (or SQL text) and return the driver's rows."""
        cur = self.cursor()
        cur.execute(self.compile(statement, **params))
        return cur.fetchall()

    def first(self, statement, **params):
        """The first row of a statement, or None."""
        cur = self.cursor()
        cur.execute(self.compile(statement, **params))
        return cur.fetchone()

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

        result = None
        if self.connection._shm and _is_read_only(sql):
            result = self.connection._shm_read(sql)
        if result is None:
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

    def stream(self, operation: str, parameters=None, size=_SHM_BATCH):
        """Yield columnar batches for a read-only SELECT, zero-copy.

        Each batch is a ``Segment``: ``row_count`` plus ``columns``, whose
        ``data``/``bytes``/``nulls`` are memoryviews aliasing the /dev/shm
        segment. Those views are only valid until the next iteration — copy (or
        reduce into numpy) whatever you keep. This is the fast path: no Python
        row objects, so it runs at transport speed rather than decode speed.

        The ring is held for the stream's lifetime; exhausting or closing the
        generator returns it. Requires the extension.
        """
        sql = operation if parameters is None else _inline(operation, parameters)
        return self._stream(sql, size)

    def _stream(self, sql, size):
        if not self.connection._shm or not _is_read_only(sql):
            raise NotSupportedError("stream() serves read-only SELECTs over /dev/shm")
        info = _ring_acquire(self.connection._conn)
        if info is None:
            raise NotSupportedError("stream() needs the fastpostgres extension (ring unavailable)")
        ring, seg = info
        yielded = False
        try:
            ring.open(seg, sql)
            while True:
                n = ring.next(size)
                batch = _client.read(seg)
                if n == 0 and yielded:
                    break
                yielded = True
                yield batch
                if n == 0:
                    break
        finally:
            try:
                ring.close()
            except Exception:
                pass
            _ring_release(info)

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
        if self._pos == 0:  # the common case: hand the list over instead of copying it
            self._pos = len(self._rows)
            return self._rows
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
