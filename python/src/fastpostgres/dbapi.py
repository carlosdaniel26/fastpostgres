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
SQLAlchemy statement with SQLAlchemy's own compiler and returns the result:
a `ResultSet` holding the columnar frame, decoded one row at a time. `len()`,
indexing, iteration and `keys()` work as they do on a list; a tuple is built
only for the row that is asked for. `force_tuples=True` — or the
`FASTPOSTGRES_MATERIALIZE=1` environment switch — restores the old list of
tuples:

    conn.fetchall(select(Row.id, Row.sku))    # ResultSet, lazy per row
    conn.fetchall(select(Row.id, Row.sku), force_tuples=True)  # [(1, 'SKU-1'), ...]
"""
import datetime
import decimal
import math
import os
import sys
import queue
import re
import threading
import time
import warnings
from bisect import bisect_right
from typing import Any, Optional, Sequence

from . import _client
from . import partition

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
    "ResultSet",
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


# Rows per generated statement. Large enough that 1000-row inserts are one round
# trip, small enough that a runaway parameter list cannot build a statement the
# server refuses on size.
_INSERT_BATCH = 1000

_INSERT_RE = re.compile(r"^(\s*INSERT\s+INTO\s.+?)\bVALUES\s*\((.+)\)\s*;?\s*$", re.I | re.S)


def _batch_insert(operation, params):
    """(head, inner) when `operation` is a batchable INSERT, else None.

    `inner` is the text between the VALUES parentheses, so a row is
    `"(" + _inline(inner, row) + ")"` and a batch is those joined by commas.
    """
    if not isinstance(operation, str):
        return None
    low = operation.lower()
    if "returning" in low or "on conflict" in low or "values" not in low:
        return None
    m = _INSERT_RE.match(operation)
    if m is None:
        return None
    head, inner = m.group(1), m.group(2)
    if "%s" not in inner:
        return None
    # One verify: every row must supply exactly the placeholders the template
    # has, and none of them may be a mapping (this driver inlines sequences).
    width = inner.count("%s")
    for row in params:
        if isinstance(row, (str, bytes, dict)) or row is None:
            return None
        try:
            if len(row) != width:
                return None
        except TypeError:
            return None
    return head, inner


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

# Rows per batch: the unit of both the ring's work and each data segment's
# footprint. Smaller batches put less in each segment (less resident memory per
# ring, and a client working set that stays in cache) at the cost of more
# per-batch fixed cost in the worker. FASTPOSTGRES_BATCH overrides it.
_SHM_BATCH = max(1, int(os.environ.get("FASTPOSTGRES_BATCH", "100000")))
_READ_RE = re.compile(r"^(select|values|table)\b", re.I | re.S)

# The C++ reduce's operations (fastpostgres/native/aggregate.cpp).
_AGG_OPS = {
    "count_all": _client.AGG_COUNT_ALL,
    "count": _client.AGG_COUNT,
    "sum": _client.AGG_SUM,
    "min": _client.AGG_MIN,
    "max": _client.AGG_MAX,
}

# Ask the worker to reduce each batch where the tuples are (FASTPOSTGRES_OP_REDUCE)
# instead of shipping the batch and reducing it in C++. 0 keeps the client-side
# path, which is also the automatic fallback for an extension that predates the op.
_AGG_PUSHDOWN = os.environ.get("FASTPOSTGRES_AGG_PUSHDOWN", "1") != "0"

# How many rings one aggregate() may fan a simple single-table SELECT out over.
# 1 — the default — is the single-ring path, unchanged; a call's `parallel=`
# overrides this process-wide default. Only a plain SELECT from one base table
# splits (fastpostgres.partition), and only when the driver can drive the rings
# at once (_client.aggregate_frames); anything else keeps the one-ring path.
_PARALLEL_ENV = "FASTPOSTGRES_PARALLEL"
# A fan-out asks for more ranges than it has rings, so a partition that is
# slower than its neighbours (dead tuples, a cold page range) does not set the
# wall time: the thread that finishes early takes the next range.
_RANGE_MULT = 3
# Below this the fan-out is not worth its fixed costs (plan per range, wakeups).
# One page = 8 KiB; 1024 pages is 8 MiB.
_FANOUT_MIN_BYTES = 8 << 20
# Import one exported snapshot into every partition, so N backends read one
# point in time instead of N. Costs a BEGIN/export/COMMIT round trip per call.
# OFF by default: the worker-side import (stream_open -> SET TRANSACTION
# SNAPSHOT) still wedges the ring on this build, so the fan-out's partitions
# take independent snapshots unless FASTPOSTGRES_SNAPSHOT=1 asks for the shared
# one. See reduce/worker notes; this is the one item left unfinished.
_SNAPSHOT = os.environ.get("FASTPOSTGRES_SNAPSHOT", "0") != "0"
_fanout_max = None


def _agg_plan(plan) -> str:
    """The worker's plan string: ``op[:i<index>|n<name>]``, comma separated.

    `plan` is a sequence of ``(op, column)``. Names escape ',' and '\\'; an
    integer column is explicit so a column literally named like an index cannot
    be mistaken for one.
    """
    parts = []
    for op, column in plan:
        if column is None:
            parts.append(str(op))
        elif isinstance(column, int):
            parts.append(f"{op}:i{column}")
        else:
            col = str(column).replace("\\", "\\\\").replace(",", "\\,")
            parts.append(f"{op}:n{col}")
    return ",".join(parts)


def _agg_combine(acc, ops, values):
    """Fold one batch's scalars into the running accumulators, in plan order."""
    for i, op in enumerate(ops):
        v = values[i]
        if op in (_client.AGG_MIN, _client.AGG_MAX):
            if v is None:
                continue
            if acc[i] is None or (v < acc[i] if op == _client.AGG_MIN else v > acc[i]):
                acc[i] = v
        else:
            acc[i] = (acc[i] or 0) + v


def _numeric_parts(value):
    """(unscaled integer, decimal scale) of an int/Decimal: value = n / 10**scale."""
    if isinstance(value, decimal.Decimal):
        total = value.copy_abs()
        _sign, digits, exponent = total.as_tuple()
        n = int("".join(map(str, digits)) or "0")
        if exponent >= 0:
            return n * 10**exponent, 0
        return n, -exponent
    return abs(int(value)), 0


def _numeric_weight_first(n, scale):
    """PostgreSQL's numeric weight and leading base-10000 digit.

    `value` is n / 10**scale. The server stores base-10000 digits and weight is
    the position of the leading one, so a value below 1 has a negative weight.
    """
    if n == 0:
        return 0, 0
    integer_digits = len(str(n)) - scale
    weight = (integer_digits - 1) // 4
    if weight >= 0:
        first = n // (10 ** (scale + 4 * weight))
    else:
        first = (n * 10000 ** (-weight)) // (10**scale)
    return weight, first


def _pg_avg(total, count):
    """SUM/COUNT with PostgreSQL's exact numeric division.

    ``avg(int)``/``avg(bigint)``/``avg(numeric)`` return numeric on the server,
    and its division scale is not "always 16 decimal places": it keeps sixteen
    *significant* base-10000 digits, which for a large sum means fewer (or no)
    decimals. Mirror select_div_scale() and round half away from zero, so the
    value here equals the server's rather than merely approximating it.
    """
    if count == 0:
        return None
    if isinstance(total, float):
        return total / count

    numerator, scale = _numeric_parts(total)
    divisor = int(count)
    weight, first = _numeric_weight_first(numerator, scale)
    divisor_weight, divisor_first = _numeric_weight_first(divisor, 0)
    qweight = weight - divisor_weight
    if first <= divisor_first:
        qweight -= 1
    rscale = max(scale, 16 - qweight * 4, 0)

    quotient, remainder = divmod(numerator * 10**rscale, divisor * 10**scale)
    if 2 * remainder >= divisor * 10**scale:  # round half away from zero
        quotient += 1
    digits = str(quotient)
    if rscale:
        digits = digits.rjust(rscale + 1, "0")
        text = f"{digits[:-rscale]}.{digits[-rscale:]}"
    else:
        text = digits
    if total < 0 and quotient:
        text = "-" + text
    return decimal.Decimal(text)


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


def _env_flag(name):
    """A boolean environment switch: unset, 0, false, no and off mean off."""
    return os.environ.get(name, "").strip().lower() not in ("", "0", "false", "no", "off")


# The DB-API default is the lazy result below. This restores the old behaviour —
# a tuple per row built while the ring is still checked out — for a consumer
# that really does want a plain list of tuples.
_FORCE_TUPLES = _env_flag("FASTPOSTGRES_MATERIALIZE")

# Opt-in lease mode: keep the ring checked out for the result's lifetime and
# decode directly out of the segment instead of copying each column. Only the
# last two batches can stay borrowed (there are two data segments); older ones
# are copied when their segment is reused. A lease therefore holds a pool
# partition until the result is consumed, materialised, closed or collected.
_LEASE = _env_flag("FASTPOSTGRES_LEASE")
_LEASE_SIZE_ENV = os.environ.get("FASTPOSTGRES_LEASE_BATCH")
_LEASE_BATCH = int(_LEASE_SIZE_ENV) if _LEASE_SIZE_ENV else _SHM_BATCH


def _own_columns(columns, nrows):
    """Copy a columnar batch out of the segment into buffers the result owns."""
    owned = []
    for column in columns:
        data = bytes(column.data)
        payload = None
        if column.bytes is not None:
            payload = memoryview(data)[len(data) - len(column.bytes):]
        nulls = bytes(column.nulls) if column.nulls is not None else None
        owned.append(
            _client.column(
                column.name, column.kind, column.pgtype, nrows, data, payload, nulls
            )
        )
    return owned


def _borrow_columns(columns, nrows):
    """Keep a columnar batch as views over the live segment, with no copy."""
    return [
        _client.column(
            column.name, column.kind, column.pgtype, nrows,
            column.data, column.bytes, column.nulls,
        )
        for column in columns
    ]


# Rows one C call decodes when a lazy result is iterated. Small enough that an
# early-stopping consumer does not build a whole batch, large enough that the
# kind switch and the null-map test amortise across rows rather than values.
_ITER_BLOCK = 1024
# Materialising has no early stop, so it can use a larger window and keep more
# of a column's bytes in L1 across the row loop.
_MATERIALIZE_BLOCK = 4096


class ResultSet:
    """The rows of a read, held columnar and decoded block by block.

    ``execute()`` on a read-only SELECT copies each columnar batch out of
    /dev/shm (one memcpy per column, no Python object per value) and hands back
    this sequence instead of a list of tuples. ``len()``, indexing, slicing,
    iteration, ``keys()`` and ``fetchone()``/``fetchmany()`` all work as they do
    on the old list; a block of tuples is decoded only when a row in it is
    asked for, and indexing decodes just the one row.

    The copy is what makes the lifetime unbounded: the ring goes straight back
    to the pool, and later reads cannot overwrite the rows this result holds.
    ``to_tuples()`` materialises the whole result in C, through the same
    specialized decode the eager path uses; ``FASTPOSTGRES_MATERIALIZE=1`` (or
    ``force_tuples=True`` on a connection) does it at ``fetchall()`` time.

    A libpq result uses the same class with a native PGresult-backed source:
    it has this shape and laziness, but libpq itself already has the text in
    memory, so the source decodes a row on demand instead of a column batch.
    """

    __slots__ = (
        "_frames",
        "_keys",
        "_types",
        "_starts",
        "_total",
        "_materialized",
        "_source",
        "_lease",
        "_lease_owner",
    )

    def __init__(self, frames, keys, field_types, total, source=None, lease=None, owner=None):
        self._frames = frames  # [(nrows, [Column, ...])], in result order
        self._keys = tuple(keys)
        self._types = tuple(field_types)
        if frames is None:
            self._starts = None
        else:
            starts = [0]
            for nrows, _columns in frames:
                starts.append(starts[-1] + nrows)
            self._starts = starts
        self._total = total
        self._materialized = None
        self._source = source
        self._lease = lease  # (ring, seg, pid): held until close()/to_tuples()
        self._lease_owner = owner

    # ---- sequence protocol ------------------------------------------------

    def __len__(self):
        return self._total

    def _row_at(self, index):
        if self._source is not None:
            return self._source.decode_row(index)
        frame = bisect_right(self._starts, index) - 1
        nrows, columns = self._frames[frame]
        row = index - self._starts[frame]
        return _client.decode_row(columns, row)

    def _rows_range(self, start, count):
        """Decode `count` rows from `start`, honouring frame boundaries."""
        if count <= 0:
            return []
        if self._source is not None:
            decode = getattr(self._source, "decode_rows", None)
            if decode is not None:
                return decode(start, count)
            return [self._source.decode_row(start + i) for i in range(count)]

        rows = []
        offset = start
        frame = bisect_right(self._starts, start) - 1
        while len(rows) < count:
            nrows, columns = self._frames[frame]
            at = offset - self._starts[frame]
            take = min(count - len(rows), nrows - at)
            rows.extend(_client.decode_rows(columns, at, take))
            offset += take
            frame += 1
        return rows

    def __getitem__(self, key):
        if isinstance(key, slice):
            if self._materialized is not None:
                return self._materialized[key]
            start, stop, step = key.indices(self._total)
            if step == 1:
                return self._rows_range(start, stop - start)
            return [self._row_at(i) for i in range(start, stop, step)]
        try:
            index = key.__index__()
        except AttributeError:
            raise TypeError("result indices must be integers or slices") from None
        if index < 0:
            index += self._total
        if index < 0 or index >= self._total:
            raise IndexError("result row index out of range")
        if self._materialized is not None:
            return self._materialized[index]
        return self._row_at(index)

    def __iter__(self):
        if self._materialized is not None:
            return iter(self._materialized)
        return self._iter_rows()

    def _iter_rows(self):
        if self._source is not None:
            decode = getattr(self._source, "decode_rows", None)
            start = 0
            while start < self._total:
                n = min(_ITER_BLOCK, self._total - start)
                if decode is not None:
                    block = decode(start, n)
                else:
                    block = [self._source.decode_row(start + i) for i in range(n)]
                for row in block:
                    yield row
                start += n
            return

        for nrows, columns in self._frames:
            start = 0
            while start < nrows:
                n = min(_ITER_BLOCK, nrows - start)
                block = _client.decode_rows(columns, start, n)
                for row in block:
                    yield row
                start += n

    def __eq__(self, other):
        if isinstance(other, ResultSet):
            if self._total != other._total:
                return False
            return self.to_tuples() == other.to_tuples()
        if isinstance(other, (list, tuple)):
            if self._total != len(other):
                return False
            return all(left == right for left, right in zip(self, other))
        return NotImplemented

    def __repr__(self):
        return f"ResultSet(rows={self._total})"

    # ---- metadata and the materialising escape hatch ----------------------

    def keys(self):
        """The result's column names, in order."""
        return list(self._keys)

    def field_types(self):
        """The PostgreSQL type OIDs, in column order."""
        return list(self._types)

    def to_tuples(self):
        """Materialise every row now, in C, and return the list of tuples."""
        if self._materialized is None:
            if self._source is not None:
                self._materialized = self._source.to_tuples()
            else:
                rows = []
                for nrows, columns in self._frames:
                    start = 0
                    while start < nrows:
                        n = min(_MATERIALIZE_BLOCK, nrows - start)
                        rows.extend(_client.decode_rows(columns, start, n))
                        start += n
                self._materialized = rows
            # The tuples own their values now, so a lease taken to avoid the
            # copy has done its job: return the partition instead of holding it
            # until this result is garbage-collected.
            self._release_lease()
            self._frames = []
            self._starts = None
        return self._materialized

    materialize = to_tuples

    def fetchall(self):
        """DB-API convenience: the whole result as tuples."""
        return self.to_tuples()

    def _release_lease(self):
        if self._lease is not None:
            info, self._lease = self._lease, None
            _ring_release(info, self._lease_owner)

    def close(self):
        """Release a ring lease this result is holding, if any.

        A result copied out of the segment owns its rows and needs nothing; a
        leased result borrows them, so closing it invalidates further row
        access. ``to_tuples()`` closes the lease for you once the values are
        copied into Python objects.
        """
        try:
            self._release_lease()
        except Exception:
            pass

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass


# ---- ring pool ------------------------------------------------------------
#
# A ring serves one stream at a time and is shared process-wide: a read checks
# one out, runs open/next*/close, and hands it back. When the pool is exhausted
# (or the extension is absent) reads fall back to libpq.
#
# The size is resolved once per process, from the machine and the server: a
# stream needs ~1.2 hardware threads (a GIL-bound decode plus its ring worker).
# With the default "shm" engine every ring is a background worker the server
# budgets via max_worker_processes, so the server clamps the pool. With
# FASTPOSTGRES_TRANSPORT=eventfd a ring no longer costs a worker (a small pool
# of workers polls many rings), so only the machine bounds it. The pool size is
# still overridable with FASTPOSTGRES_SHM_POOL; 0 disables the shared-memory
# path. FASTPOSTGRES_SEGMENT_BYTES sizes each data segment per ring (0 = the
# worker's 1 GiB default, clamped to 64 KiB-1 GiB).
# Data segments are sealed memfds by default: the worker backs each one with an
# anonymous memfd and hands its descriptor to the client once over SCM_RIGHTS,
# instead of widening a named /dev/shm file to 0666. FASTPOSTGRES_MEMFD=0 (or
# off/false/no) selects the named segment, which is what a client running as a
# different user than the server still needs; memfd also falls back to it when
# the listener or the worker's handoff is unavailable.
#
# A checked-out ring is a lease: one thread drives it until it hands it back,
# and the C layer refuses a second driver. That is what lets several threads
# (the asyncio layer's tiers) each work their own partition of the shared
# memory; the pool is the partition table.

_POOL_ENV = os.environ.get("FASTPOSTGRES_SHM_POOL")
_SEGMENT_ENV = os.environ.get("FASTPOSTGRES_SEGMENT_BYTES")
_SEGMENT_BYTES = int(_SEGMENT_ENV) if _SEGMENT_ENV else 0
# The whole shared-memory budget this process may spend on data segments, in
# bytes; "3g"/"512m" are accepted. It is split across the pool's rings (two
# segments each), so the number is a cap, not an allocation: the pages appear as
# batches are written. FASTPOSTGRES_SEGMENT_BYTES (an exact per-segment size)
# wins over it. Unset means the worker's 1 GiB default per segment.
_BUDGET_ENV = os.environ.get("FASTPOSTGRES_SHM_BYTES")
_SEGMENT_MIN = 64 << 10
_SEGMENT_MAX = 1 << 30
# The doorbell engine. "shm" is the original semaphore ring (one background
# worker per ring); "eventfd" lets a small pool of workers serve many rings, so
# the ring count is no longer bounded by max_worker_processes. Default stays
# "shm": the eventfd pool is opt-in while it is a prototype.
_TRANSPORT = os.environ.get("FASTPOSTGRES_TRANSPORT", "shm")
_THREADS_PER_STREAM = 1.2
_POOL_MAX = 8
# The backend_type the ring workers show in pg_stat_activity, counted against
# the server's worker budget by _server_ring_slots.
_RING_BACKEND = "fastpostgres ring worker"

# A blocked acquire's budget and its re-check interval: the pool grows lazily, so
# a sleeper may wake to find a ring that another thread just made.
_ACQUIRE_WAIT = 10.0
_ACQUIRE_POLL = 0.05

_pool_idle = queue.Queue()
_pool_lock = threading.Lock()
_pool_cond = threading.Condition()  # releases wake acquirers blocked on the pool
_pool_created = 0
_pool_broken = False
_pool_pid = None
_pool_prefix = ""
_pool_size = None
_pool_leases = {}  # thread id -> (segment name, pid): the rings checked out now

# Reads that left the shared-memory path, and why. Silence is how a pool that is
# too small, or a worker that died, goes unnoticed. Locked: on a free-threaded
# build two decoders can fall back at once.
libpq_reads = 0
last_fallback = None
_fallback_causes = {}
_fallback_lock = threading.Lock()


def _cgroup_cpu_threads():
    """The CPU allowance of this cgroup, in hardware threads, or None."""
    try:  # cgroup v2
        with open("/sys/fs/cgroup/cpu.max") as f:
            quota, period = f.read().split()[:2]
        if quota == "max":
            return None
        return max(1, math.ceil(int(quota) / int(period)))
    except (OSError, ValueError):
        pass
    try:  # cgroup v1
        with open("/sys/fs/cgroup/cpu/cpu.cfs_quota_us") as f:
            quota = int(f.read())
        with open("/sys/fs/cgroup/cpu/cpu.cfs_period_us") as f:
            period = int(f.read())
        if quota <= 0:
            return None
        return max(1, math.ceil(quota / period))
    except (OSError, ValueError):
        return None


def _threads_per_core():
    """SMT width (/proc/cpuinfo: threads per socket over cores per socket)."""
    try:
        siblings = cores = None
        with open("/proc/cpuinfo") as f:
            for line in f:
                key, _, value = line.partition(":")
                if key.strip() == "siblings" and siblings is None:
                    siblings = int(value)
                elif key.strip() == "cpu cores" and cores is None:
                    cores = int(value)
                if siblings and cores:
                    break
        if siblings and cores:
            return max(1, siblings // cores)
    except (OSError, ValueError):
        pass
    return 1


def _parse_bytes(text):
    """'3g', '512m', '1 GiB', or a plain byte count; None when it is not one."""
    if text is None:
        return None
    t = str(text).strip().lower().replace(" ", "")
    for suffix, scale in (("gib", 1 << 30), ("gb", 1 << 30), ("g", 1 << 30),
                          ("mib", 1 << 20), ("mb", 1 << 20), ("m", 1 << 20),
                          ("kib", 1 << 10), ("kb", 1 << 10), ("k", 1 << 10)):
        if t.endswith(suffix):
            t = t[: -len(suffix)]
            break
    else:
        scale = 1
    try:
        return max(0, int(float(t) * scale))
    except (TypeError, ValueError):
        return None


_SHM_BUDGET = _parse_bytes(_BUDGET_ENV)
_shm_reported = False


def _segment_bytes() -> int:
    """Bytes of data segment per ring: the exact override, else the budget split
    over two segments per ring in the pool, else the worker's 1 GiB default."""
    if _SEGMENT_BYTES:
        return _SEGMENT_BYTES
    if _SHM_BUDGET:
        rings = max(1, pool_size())
        return max(_SEGMENT_MIN, min(_SEGMENT_MAX, _SHM_BUDGET // (2 * rings)))
    return 0  # 0: the worker's FASTPOSTGRES_SEGMENT_BYTES


def _human_bytes(n: int) -> str:
    for scale, unit in ((1 << 30, "GiB"), (1 << 20, "MiB"), (1 << 10, "KiB")):
        if n >= scale:
            return f"{n / scale:.1f} {unit}".replace(".0 ", " ")
    return f"{n} B"


def shm_config() -> dict:
    """What this process resolved for shared memory, for logs and charts.

    ``budget`` is the configured total (None when unset), ``segment`` the size a
    ring asks the worker for (0 = the worker's default), ``rings`` the pool, and
    ``text`` a one-line summary a chart footer can print.
    """
    segment = _segment_bytes()
    rings = pool_size()
    budget = _SHM_BUDGET
    if segment:
        text = f"{_human_bytes(segment)}/ring · {rings} rings"
        if budget:
            text += f" · {_human_bytes(budget)} budget"
    else:
        text = f"worker default · {rings} rings"
    return {"budget": budget, "segment": segment, "rings": rings, "text": text}


def _machine_streams() -> int:
    """How many concurrent reads one process can usefully keep going here."""
    try:
        cpus = len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        cpus = os.cpu_count() or 1
    quota = _cgroup_cpu_threads()
    if quota:
        cpus = min(cpus, quota)
    physical = max(1, cpus // _threads_per_core())
    return max(1, min(_POOL_MAX, int(physical / _THREADS_PER_STREAM)))


def _server_ring_slots(conn):
    """Rings the server can still register: max_worker_processes, less the one it
    holds back, less the ring workers already running — ours or another
    process's, which is what makes several app processes share one budget."""
    try:
        budget = int(conn.exec("SHOW max_worker_processes").rows[0][0])
        taken = int(conn.exec(
            f"SELECT count(*) FROM pg_stat_activity WHERE backend_type = '{_RING_BACKEND}'"
        ).rows[0][0])
    except Exception:
        return None
    return max(1, budget - 1 - taken)


def _resolve_pool_size(conn) -> int:
    """Size the pool once per process: the machine's streams, clamped by what the
    server has left to register."""
    global _pool_size

    if _pool_size is not None:
        return _pool_size
    if _POOL_ENV is not None:
        _pool_size = int(_POOL_ENV)
        return _pool_size
    size = _machine_streams()
    # The eventfd engine decouples rings from background workers: a ring no
    # longer costs a server slot, so the machine is the bound. The shm engine
    # still clamps to max_worker_processes.
    if _TRANSPORT == "shm":
        slots = _server_ring_slots(conn)
        if slots is not None:
            size = min(size, slots)
    _pool_size = max(1, size)
    return _pool_size


def _prefix() -> str:
    """The ring/segment name stem for *this* process.

    Module state is inherited across fork — gunicorn, uwsgi and multiprocessing all
    import first and fork after — so a stem computed at import time gives every
    worker the parent's name, and two processes on one ring means two background
    workers on one data segment. A pid change means we are the child: take a fresh
    stem and forget the parent's pool, because its rings are not ours to hand out.
    """
    global _pool_pid, _pool_prefix, _pool_created, _pool_broken, _pool_size

    pid = os.getpid()
    if _pool_pid != pid:
        _pool_pid = pid
        _pool_prefix = f"fp_dbapi_{pid}"
        _pool_created = 0
        _pool_broken = False
        _pool_size = None  # and its size is the child's to work out
        with _pool_lock:
            _pool_leases.clear()  # the parent's leases died with the fork
        while True:
            try:
                _pool_idle.get_nowait()
            except queue.Empty:
                break
    return _pool_prefix


def pool_size():
    """Maximum number of worker-backed rings this process will create: the env
    override, the size resolved on the first read that needed a ring, or the
    machine's own estimate before that."""
    if _pool_size is not None:
        return _pool_size
    if _POOL_ENV is not None:
        return int(_POOL_ENV)
    return _machine_streams()


def pool_created():
    """Rings created so far (idle + checked out)."""
    return _pool_created


def pool_stats() -> dict:
    """What the shared-memory path is doing: its size, its leases, its fallbacks."""
    with _pool_lock:
        leases = sorted(name for name, _pid in _pool_leases.values())
        created, broken = _pool_created, _pool_broken
    return {
        "size": _pool_size,
        "created": created,
        "idle": _pool_idle.qsize(),
        "leases": leases,
        "broken": broken,
        "libpq_reads": libpq_reads,
        "last_fallback": last_fallback,
        "causes": dict(_fallback_causes),
    }


def _note_fallback(reason, detail=None, warn=True):
    """Count a read that left the shared-memory path; say so once per reason.

    A fallback is invisible by design — same rows, slower road — but a process
    that has quietly lost the ring path should not look healthy. Count every
    cause; warn the first time a failure class (not routine pool pressure)
    shows up.
    """
    global libpq_reads, last_fallback

    with _fallback_lock:
        libpq_reads += 1
        last_fallback = detail or reason
        count = _fallback_causes.get(reason, 0)
        _fallback_causes[reason] = count + 1
    if warn and count == 0:
        warnings.warn(
            f"fastpostgres: a read left the shared-memory path ({last_fallback})",
            RuntimeWarning,
            stacklevel=3,
        )


def _report_shm():
    """One line per process naming the shared-memory budget, into the run's log.

    A benchmark container sets BENCH_GIT_REV, and every chart parses `key: value`
    lines out of the log, so this is how the figure footer states how much
    shared memory the run was given instead of leaving it implicit.
    """
    global _shm_reported

    if _shm_reported or not os.environ.get("BENCH_GIT_REV"):
        return
    _shm_reported = True
    print(f"shm: {shm_config()['text']}", file=sys.stderr)


def _try_acquire(bootstrap, prefix):
    """The idle queue, else a fresh ring while the pool may still grow."""
    global _pool_created

    try:
        return _pool_idle.get_nowait()
    except queue.Empty:
        pass

    with _pool_lock:
        size = _resolve_pool_size(bootstrap)  # once per process, from machine + server
        if _pool_created >= size:
            return None
        index = _pool_created
        _pool_created += 1

    name = f"{prefix}_{index}"
    try:
        ring = _client.Ring(name, _segment_bytes(), _TRANSPORT)
        ring.start(bootstrap)
        _report_shm()
    except Exception as exc:
        with _pool_lock:
            _pool_created -= 1
            _pool_broken = True
        if "could not register background worker" in str(exc):
            _warn_no_workers()
        return None
    return (ring, name + "_seg", os.getpid())


def _ring_acquire(bootstrap, block=False, timeout=None):
    """Check out a (ring, segment, pid) partition, or None when there is none.

    The idling queue first, then a fresh ring while the pool may still grow.
    With ``block``, wait for either — up to ``timeout`` (``_ACQUIRE_WAIT`` by
    default) — instead of dropping straight to libpq, which is what the
    awaitable paths pass: an async caller queues for a partition.

    The acquiring thread owns the lease and must be the one to release it.
    """
    prefix = _prefix()  # per process: a forked child must not reuse the parent's
    deadline = None
    if block:
        deadline = time.monotonic() + (_ACQUIRE_WAIT if timeout is None else timeout)

    while True:
        if _pool_broken:
            return None
        info = _try_acquire(bootstrap, prefix)
        if info is not None:
            with _pool_lock:
                _pool_leases[threading.get_ident()] = (info[1], info[2])
            return info
        if not block or time.monotonic() >= deadline:
            return None
        with _pool_cond:
            _pool_cond.wait(min(_ACQUIRE_POLL, max(0.0, deadline - time.monotonic())))


def _physical_cores() -> int:
    """Hardware cores, not SMT threads: a ring worker is one backend on one core."""
    try:
        cpus = len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        cpus = os.cpu_count() or 1
    quota = _cgroup_cpu_threads()
    if quota:
        cpus = min(cpus, quota)
    return max(1, cpus // _threads_per_core())


def _fanout_capacity(conn) -> int:
    """How many rings one query may fan out over.

    The pool's own size divides the machine by ``_THREADS_PER_STREAM`` because a
    GIL-bound read costs a client decode beside its worker. A fan-out's client
    threads only block in ``wait()`` (or, for ``stream()``, copy nothing), so the
    bound is the cores themselves — and the server's worker budget, which is what
    actually caps it on the default transport.
    """
    global _fanout_max, _pool_size

    if _fanout_max is not None:
        return _fanout_max
    want = min(_physical_cores(), _POOL_MAX)
    if _TRANSPORT == "shm":
        slots = _server_ring_slots(conn)
        if slots is not None:
            want = min(want, slots)
    # The fan-out borrows the pool, it does not resize it, and it leaves one
    # ring free: every ring is a server worker slot, and a query that takes the
    # whole pool knocks every other read in the process (or its tests, or the
    # next connection) off the shared-memory path.
    want = max(1, min(want, pool_size() - 1))
    _fanout_max = want
    return want


def _fanout_want(conn, parallel) -> int:
    """The rings a call asks for: the argument, the env override, or the machine."""
    if parallel is not None:
        try:
            return int(parallel)
        except (TypeError, ValueError):
            return 1
    env = os.environ.get(_PARALLEL_ENV, "auto").strip().lower()
    if env in ("", "auto", "all"):
        return _fanout_capacity(conn)
    if env in ("0", "1", "off", "false", "no"):
        return 1
    try:
        return int(env)
    except ValueError:
        return 1


class _Split:
    """One query split across ctid page ranges, and the snapshot they share.

    ``close()`` ends the exporting transaction when one was started; every path
    that creates a _Split must call it, or the connection stays in a transaction
    and its snapshot stays pinned.
    """

    __slots__ = ("sqls", "relpages", "snapshot", "_conn", "_began")

    def __init__(self, conn):
        self.sqls = None
        self.relpages = 0
        self.snapshot = ""
        self._conn = conn
        self._began = False

    def close(self):
        if self._began:
            self._began = False
            try:
                self._conn.exec("COMMIT")
            except Exception:
                pass


def _split(conn, sql, want, min_bytes=_FANOUT_MIN_BYTES):
    """Split `sql` into wat*_RANGE_MULT disjoint page ranges, or None.

    None means "do not fan out": the statement is not a simple single-table
    SELECT, the relation has fewer bytes than the break-even, or the snapshot
    could not be exported. The caller keeps the single-ring path, unchanged.
    """
    if want < 2:
        return None
    table = partition.table_of(sql)
    if table is None:
        return None

    sp = _Split(conn)
    if _SNAPSHOT:
        try:
            conn.exec("BEGIN ISOLATION LEVEL REPEATABLE READ")
            sp._began = True
            sp.snapshot = str(conn.exec("SELECT pg_export_snapshot()").rows[0][0])
        except Exception:
            sp.close()

    quoted = table.replace("'", "''")
    try:
        sp.relpages = int(
            conn.exec(
                "SELECT greatest(relpages, pg_relation_size(oid)"
                " / current_setting('block_size')::int)"
                f" FROM pg_class WHERE oid = '{quoted}'::regclass"
            ).rows[0][0]
        )
    except Exception:
        sp.close()
        return None  # not a table, not visible: the serial path says why

    if sp.relpages * 8192 < min_bytes:
        sp.close()  # too small to pay for a plan and a wakeup per range
        return None

    sqls = partition.partition_sql(sql, want * _RANGE_MULT, sp.relpages)
    if sqls is None or len(sqls) < 2:
        sp.close()
        return None
    sp.sqls = sqls
    return sp


def _acquire_many(conn, want, block=False):
    """Up to `want` leases, in this thread: one ring per partition."""
    out = []
    for _ in range(want):
        info = _ring_acquire(conn, block=block)
        if info is None:
            break
        out.append(info)
    return out


def _release_many(infos):
    for info in infos:
        _ring_release(info)


class _FanRing:
    """One ring of a stream fan-out: its ranges and the batch in flight.

    Each ring alternates two data segments, so the next batch is already being
    written while the caller reads the current one — the same pipelining the
    single-ring lazy read uses, applied per partition.
    """

    __slots__ = ("info", "ring", "seg", "ranges", "idx", "slot", "posted")

    def __init__(self, info, ranges):
        self.info = info
        self.ring, self.seg, _pid = info
        self.ranges = ranges
        self.idx = 0
        self.slot = 0
        self.posted = False

    def segments(self):
        return (self.seg + "_a", self.seg + "_b")

    def open(self, size):
        self.ring.open(self.seg, self.ranges[0])
        self.post(0, size)

    def post(self, slot, size):
        self.slot = slot
        self.ring.post(self.segments()[slot], size)
        self.posted = True

    def advance(self, size):
        """The current range ended: open the next one, or stop."""
        self.idx += 1
        if self.idx >= len(self.ranges):
            self.posted = False
            return False
        try:
            self.ring.close()  # the worker's OPEN would abort it anyway; be explicit
        except Exception:
            pass
        self.ring.open(self.seg, self.ranges[self.idx])
        self.post(0, size)
        return True

    def close(self):
        try:
            if self.posted:
                self.ring.wait()  # an unanswered post must be drained first
        except Exception:
            pass
        try:
            self.ring.close()
        except Exception:
            pass


_warned_no_workers = False


def _warn_no_workers():
    """The server's worker budget is what is blocking the shared-memory path: say
    so once, rather than reading over libpq in silence for the life of the
    process."""
    global _warned_no_workers

    if _warned_no_workers:
        return
    _warned_no_workers = True
    warnings.warn(
        "fastpostgres: no background worker slots left on the server "
        "(max_worker_processes); this process now reads over libpq",
        RuntimeWarning,
        stacklevel=4,
    )


def _ring_release(info, owner=None):
    """Hand a checked-out partition back, unless it is not this process's.

    A forked child inherits its parent's rings: driving one would race the
    parent, and returning it to the child's idle queue would hand a stranger's
    ring to its own next reader. Drop it; the parent keeps using its own.

    ``owner`` is the thread that took the lease when the release happens
    somewhere else — a leased result can be materialised or dropped on another
    thread, and the bookkeeping must drop that thread's entry, not this one's.
    """
    ring, _seg, pid = info
    with _pool_lock:
        _pool_leases.pop(threading.get_ident() if owner is None else owner, None)
    if pid != os.getpid():
        return
    _pool_idle.put_nowait(info)
    with _pool_cond:
        _pool_cond.notify()


class Connection:
    """A DB-API connection over one libpq control channel (autocommit).

    Read-only SELECTs are served by the /dev/shm ring; everything else (DML,
    DDL, SET, ...) takes libpq. If the extension is absent the ring fails to
    start and every statement stays on libpq.

    ``wait`` blocks for a ring when the pool is at its limit instead of falling
    back to libpq — what the awaitable paths pass, so an async caller queues for
    a partition rather than dropping to the slow road.
    """

    def __init__(self, conninfo: str = "", shm: bool = True, wait: bool = False,
                 force_tuples=None, lease=None, **kwargs):
        if not conninfo and kwargs:
            conninfo = " ".join(f"{k}={v}" for k, v in kwargs.items() if v is not None)
        try:
            self._conn = _client.Connection(conninfo)
        except Exception as exc:  # pragma: no cover - exercised via connect()
            raise _translate(exc) from exc
        self._closed = False
        self._shm = shm
        self._wait = wait
        self._force_tuples = _FORCE_TUPLES if force_tuples is None else bool(force_tuples)
        self._lease = _LEASE if lease is None else bool(lease)
        self._streams = set()  # live stream generators, so close() can end them
        self.shm_reads = 0

    def _shm_read(self, sql):
        if not self._shm:
            return None
        info = _ring_acquire(self._conn, block=self._wait)
        if info is None:
            if pool_size() > 0:  # size 0 is the explicit "no ring" configuration
                _note_fallback("pool exhausted", warn=False)
            return None  # pool exhausted, or the extension is absent
        ring, seg, _pid = info
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
        except Exception as exc:
            _note_fallback(type(exc).__name__, detail=f"{type(exc).__name__}: {exc}")
            return None  # re-run on libpq so the caller sees the real error
        finally:
            _ring_release(info)
        self.shm_reads += 1
        return _Result(fields or [], field_types or [], rows, len(rows))

    def _fan_lazy(self, sql, size):
        """The columnar result of a split query as one ResultSet, or None.

        The batches are copied out of each partition's segment as they arrive
        (`_own_columns`), so the result owns its bytes and the rings go back to
        the pool immediately — the same contract the single-ring lazy read has.
        """
        gen = Cursor(self)._stream_fanout(sql, size)
        if gen is None:
            return None
        frames = []
        keys = types = None
        total = 0
        try:
            for batch in gen:
                if keys is None:
                    keys = [column.name for column in batch.columns]
                    types = [column.pgtype for column in batch.columns]
                if batch.row_count:
                    frames.append((batch.row_count, _own_columns(batch.columns, batch.row_count)))
                    total += batch.row_count
        except Exception as exc:
            _note_fallback(type(exc).__name__, detail=f"{type(exc).__name__}: {exc}")
            return None
        finally:
            gen.close()
        return ResultSet(frames, keys or [], types or [], total)

    def _shm_read_lazy(self, sql, size=_SHM_BATCH, lease=None):
        """The same read, but the result is a columnar ResultSet.

        Two data segments alternate exactly as they do for the eager read: the
        request for the next batch is posted before the current one is copied,
        so the worker's turn hides behind the copy. What is copied is each
        column's region, not a tuple: the ring then goes straight back to the
        pool and the result owns its bytes. A row is decoded when it is asked
        for, so a consumer that only counts or narrows pays no per-value work.

        With ``lease`` (``FASTPOSTGRES_LEASE=1``, ``Connection(lease=True)``)
        the batch is not copied: the result keeps views into the two data
        segments and holds the ring checked out until it is materialised,
        closed or collected. Only the last two batches can stay borrowed; when
        a segment is reused its older frame is copied first. ``size`` is raised
        to ``FASTPOSTGRES_LEASE_BATCH`` (default one normal batch) so a result
        that fits one batch — the case the lease is for — stays entirely
        borrowed.
        """
        if not self._shm:
            return None
        if lease is None:
            lease = self._lease
        if lease and _LEASE_BATCH > size:
            size = _LEASE_BATCH
        if not lease:
            fan = self._fan_lazy(sql, size)
            if fan is not None:
                self.shm_reads += 1
                return fan
        info = _ring_acquire(self._conn, block=self._wait)
        if info is None:
            if pool_size() > 0:  # size 0 is the explicit "no ring" configuration
                _note_fallback("pool exhausted", warn=False)
            return None
        ring, seg, _pid = info

        frames = []
        borrowed = {}  # segment slot -> frame index, for lease mode
        keys = types = None
        total = 0
        pending = False
        keep_lease = False
        try:
            segs = (seg + "_a", seg + "_b")
            ring.open(seg, sql)
            try:
                i = 0
                ring.post(segs[0], size)
                pending = True
                n = ring.wait()
                pending = False
                while True:
                    more = n != 0
                    if more:
                        slot = (i + 1) & 1
                        if slot in borrowed:
                            # The worker is about to overwrite this segment:
                            # give the frame that still borrows it its own copy
                            # first. Frames in the other, newer segment are
                            # untouched.
                            at = borrowed.pop(slot)
                            nrows, columns = frames[at]
                            frames[at] = (nrows, _own_columns(columns, nrows))
                        ring.post(segs[slot], size)
                        pending = True
                    batch = _client.read(segs[i & 1])
                    if keys is None:
                        keys = [column.name for column in batch.columns]
                        types = [column.pgtype for column in batch.columns]
                    if n:
                        if lease:
                            frames.append((n, _borrow_columns(batch.columns, n)))
                            borrowed[i & 1] = len(frames) - 1
                        else:
                            # One copy of the whole column region. The varlena
                            # kinds keep offsets and bytes together in `data`,
                            # so `bytes` is a slice of the copy, not a second
                            # one.
                            frames.append((n, _own_columns(batch.columns, n)))
                        total += n
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
            keep_lease = bool(lease and borrowed)
        except Exception as exc:
            _note_fallback(type(exc).__name__, detail=f"{type(exc).__name__}: {exc}")
            return None  # re-run on libpq so the caller sees the real error
        finally:
            if not keep_lease:
                _ring_release(info)

        self.shm_reads += 1
        owner = threading.get_ident() if keep_lease else None
        return ResultSet(
            frames, keys or [], types or [], total,
            lease=info if keep_lease else None, owner=owner,
        )

    def cursor(self):
        return Cursor(self)

    def columnar_load(self, query: str, capacity: int = 0):
        """Bulk-load a read-only SELECT into a sealed memfd, read-only.

        Returns a ``fastpostgres.columnar.ColumnarTable`` whose columns are
        zero-copy views over the mapped memfd.  The snapshot is taken once, at
        load time; the store is immutable and has no MVCC.  Runs on this
        connection's libpq backend, which keeps the descriptor open.
        """
        from .columnar import load as _columnar_load

        return _columnar_load(self, query, capacity)

    # ---- the CORE read without CORE --------------------------------------
    #
    # SQLAlchemy's CORE layer costs ~375 ns/row on the benchmark's shape, all of
    # it its own Result and Row machinery once the driver is subtracted. These
    # three do the same job — a statement in, its rows out — by compiling once
    # and running the plain SQL the ring already speaks, so what comes back is
    # the columnar ResultSet: no Result, no Row, no tuple until one is read.
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

    def fetchall(self, statement, force_tuples=None, **params) -> Any:
        """Run a SQLAlchemy statement (or SQL text) and return its rows.

        The default is the lazy ``ResultSet`` for a SELECT; ``force_tuples=True``
        returns the list of tuples the old path built.
        """
        cur = self.cursor()
        cur.execute(self.compile(statement, **params))
        return cur.fetchall(force_tuples)

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
        """End any live stream (its ring goes back), then the control channel."""
        if not self._closed:
            for stream in list(self._streams):
                try:
                    stream.close()  # CLOSE over the ring, then release
                except Exception:
                    pass
            self._streams.clear()
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

        if self.connection._shm and _is_read_only(sql):
            if self.connection._force_tuples:
                result = self.connection._shm_read(sql)
                if result is not None:
                    self.description = [
                        (name, oid, None, None, None, None, None)
                        for name, oid in zip(result.fields, result.field_types)
                    ]
                    self._rows = result.rows
                    self.rowcount = result.row_count
                    self._pos = 0
                    return self
            else:
                rows = self.connection._shm_read_lazy(sql)
                if rows is not None:
                    self.description = [
                        (name, oid, None, None, None, None, None)
                        for name, oid in zip(rows._keys, rows._types)
                    ]
                    self._rows = rows
                    self.rowcount = len(rows)
                    self._pos = 0
                    return self

        # The libpq fallback keeps the lazy result's shape: a read-only SELECT
        # over libpq is a PGresult-backed ResultSet, so a consumer that counts
        # or stops early does not pay for tuples it never reads. Forced tuples
        # and non-read statements keep the eager result (and its list).
        if (
            not self.connection._force_tuples
            and _is_read_only(sql)
            and hasattr(self.connection._conn, "exec_lazy")
        ):
            try:
                result = self.connection._conn.exec_lazy(sql)
            except Exception as exc:
                raise _translate(exc) from exc
            rows = ResultSet(
                None, result.fields, result.field_types, len(result), source=result
            )
            self.description = [
                (name, oid, None, None, None, None, None)
                for name, oid in zip(rows._keys, rows._types)
            ]
            self._rows = rows
            self.rowcount = len(rows)
            self._pos = 0
            return self

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
        """Run a statement once per parameter set.

        A plain ``INSERT ... VALUES (...)`` is batched into multi-row VALUES
        statements of up to `_INSERT_BATCH` rows: one round trip per batch
        instead of one per row. That is the whole difference between this
        driver and a prepared pipeline on a batched insert — measured on
        MagicStack's 1000-row cell, one statement per row cost ~5/s against
        asyncpg's 885/s, all of it round trips.

        Anything else — a statement with RETURNING or ON CONFLICT, a parameter
        count this cannot verify, a non-literal placeholder — keeps the row
        loop, because a wrong rewrite would change what the insert means.
        """
        params = list(seq_of_parameters)
        batched = _batch_insert(operation, params) if len(params) > 1 else None
        if batched is None:
            total = 0
            for parameters in params:
                self.execute(operation, parameters)
                total += self.rowcount
            self.rowcount = total
            return self

        total = 0
        head, inner = batched
        for start in range(0, len(params), _INSERT_BATCH):
            group = params[start : start + _INSERT_BATCH]
            tuples = ", ".join("(" + _inline(inner, row) + ")" for row in group)
            self.execute(f"{head} VALUES {tuples}")
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
        generator returns it, and so does closing the connection. Requires the
        extension.
        """
        sql = operation if parameters is None else _inline(operation, parameters)
        return self._track(self._stream(sql, size))

    def aggregate(self, operation: str, ops, parameters=None, size=_SHM_BATCH,
                  parallel=None) -> dict:
        """Reduce a read-only SELECT in C++: numbers out, no row objects.

        `ops` maps a result name to ``(operation, column)``:

            {"price_sum": ("sum", "price"), "rows": ("count_all", None),
             "cheapest": ("min", "price"), "priced": ("count", "price")}

        Operations are ``sum``, ``min``, ``max``, ``avg``, ``count`` (non-NULL
        values) and ``count_all`` (rows, no column needed — pass ``None``); a
        column is a name or an index. The reduce runs in the worker by default
        (``FASTPOSTGRES_OP_REDUCE``): it accumulates each batch where the tuples
        are and publishes a few scalars, so the client never receives the rows.
        Set ``FASTPOSTGRES_AGG_PUSHDOWN=0`` to force the older client-side walk;
        an extension that predates the op falls back to it automatically. Either
        way no Python object is built per row, so a thread reducing its own
        partition runs in parallel with the others on any interpreter — the
        decode that materializes rows is what serializes, and this never does it.

        Semantics are the server's: NULLs are skipped by sum/min/max/avg and not
        counted by count; an empty result gives 0 for sums and counts and None
        for min/max/avg. Integer sums are exact (128-bit accumulation, then a
        Python int), so a bigint sum that leaves int64 matches ``sum(bigint)``
        instead of wrapping. ``avg`` returns a float for float columns and a
        ``Decimal`` for integer and numeric columns, divided with PostgreSQL's
        own numeric scale. ``min``/``max`` work on text (the worker compares
        with the column's collation), date/time and numeric as well as numbers;
        ``count`` works on every kind. Numeric is exact on the worker path;
        with the pushdown off the columnar frame's float8 widening applies.

        One call also fans out over the ring pool. ``parallel=N`` sets the width;
        with no argument the process-wide ``FASTPOSTGRES_PARALLEL`` decides:
        ``auto`` (the default) uses one ring per physical core, bounded by the
        server's free worker slots, and only for a relation of at least 8 MiB;
        ``1`` disables the fan-out. A split is a simple single-table SELECT cut
        into disjoint ctid page ranges — each a ``Tid Range Scan`` — with every
        range reduced where its tuples are and the partial scalars folded by the
        serial path's own combiner, so the answer is the one scan's answer,
        exactly. Every range is imported into one exported snapshot, so the N
        backends read one point in time, not N. Only
        ``SELECT ... FROM one_table [WHERE ...]`` splits (see
        ``fastpostgres.partition``); a join, grouping, ordering, limit, set
        operation, CTE, window or a subquery in FROM keeps the single-ring path,
        as does a relation too small to pay for the fan-out.
        """
        sql = operation if parameters is None else _inline(operation, parameters)
        if not self.connection._shm:
            raise NotSupportedError("aggregate() is disabled on this connection (shm=False)")
        if not _is_read_only(sql):
            raise NotSupportedError("aggregate() serves read-only SELECTs over /dev/shm")

        # The internal plan. AVG is not associative across batches, so it is
        # expanded into SUM + COUNT (one extra scalar per batch) and divided once
        # at the end — an average of per-batch averages would be wrong, and this
        # keeps integer/numeric results exact.
        internal = []  # [(op, column)]
        outputs = []  # [(result name, "avg" | "plain", positions)]
        for name, spec in ops.items():
            op_name, column = spec
            if op_name == "avg":
                if column is None:
                    raise ProgrammingError("aggregate: avg needs a column")
                internal.append((_client.AGG_SUM, column))
                internal.append((_client.AGG_COUNT, column))
                outputs.append((name, "avg", (len(internal) - 2, len(internal) - 1)))
                continue
            try:
                op = _AGG_OPS[op_name]
            except KeyError:
                raise ProgrammingError(f"aggregate: unknown operation {op_name!r}") from None
            if op == _client.AGG_COUNT_ALL:
                if column is not None:
                    raise ProgrammingError("aggregate: count_all takes no column (pass None)")
            elif column is None:
                raise ProgrammingError(f"aggregate: {op_name} needs a column")
            internal.append((op, column))
            outputs.append((name, "plain", (len(internal) - 1,)))

        op_codes = [op for op, _column in internal]

        # The fan-out: a simple single-table SELECT splits into disjoint ctid page
        # ranges and the driver reduces them all at once, one range per pooled
        # worker (with more ranges than workers, so a slow partition does not set
        # the wall time). None means "not split": the single-ring path below runs,
        # unchanged.
        acc = None
        if (
            _AGG_PUSHDOWN  # the fan-out is the worker's reduce, driven N times
            and hasattr(_client, "aggregate_frames")
            and hasattr(_client, "read_reduce_bytes")  # it ships with its frame decoder
        ):
            infos = []
            split = None
            try:
                want = _fanout_want(self.connection._conn, parallel)
                split = _split(self.connection._conn, sql, want)
                if split is not None:
                    infos = _acquire_many(self.connection._conn, min(want, len(split.sqls)))
                    if len(infos) >= 2:
                        # OPEN/REDUCE/CLOSE per ring is the driver's: do not drive
                        # these rings from here, they are leased to that one call.
                        # Every range is handed over, not one per ring: the driver
                        # takes them from a shared index as threads free up.
                        frames = _client.aggregate_frames(
                            [info[0] for info in infos],
                            [info[1] for info in infos],
                            split.sqls,
                            _agg_plan(internal),
                            size,
                            split.snapshot,
                        )
                        # One buffer per ring is the contract. Fewer would mean a
                        # partition's contribution is missing, which reads as a
                        # smaller answer: fall back rather than guess. None is the
                        # driver's own "could not drive these rings".
                        if frames is not None and len(frames) != len(infos):
                            frames = None
                        if frames:
                            # Each buffer is one partition's run of FPREDUCE frames,
                            # u32 LE length + payload, in plan order: scalars only,
                            # so this is a handful of Python objects per batch and
                            # never a single row object. A buffer that does not walk
                            # exactly frame by frame is not trusted — a batch that
                            # went missing would read as a smaller answer — so the
                            # fold only becomes the result once every buffer is whole.
                            combined = [None] * len(internal)
                            walked = True
                            for buf in frames:
                                view = memoryview(buf)
                                off, end = 0, len(view)
                                while off + 4 <= end:
                                    n = int.from_bytes(view[off : off + 4], "little")
                                    off += 4
                                    if off + n > end:
                                        break  # truncated payload
                                    values = _client.read_reduce_bytes(view[off : off + n])
                                    off += n
                                    if len(values) != len(internal):
                                        raise ProgrammingError(
                                            "aggregate: the worker returned a different plan"
                                        )
                                    _agg_combine(combined, op_codes, values)
                                if off != end:
                                    walked = False
                                    break
                            if walked:
                                acc = combined
            finally:
                for info in infos:  # every lease goes back, on every path
                    _ring_release(info)
                if split is not None:
                    split.close()  # ends the exporting transaction, if one was begun

        if acc is None:
            info = _ring_acquire(self.connection._conn, block=self.connection._wait)
            if info is None:
                if pool_size() > 0:
                    _note_fallback("pool exhausted", warn=False)
                raise NotSupportedError("aggregate() needs the fastpostgres extension (ring unavailable)")
            ring, seg, _pid = info

            try:
                ring.open(seg, sql)
                acc = None

                # The pushdown: the worker accumulates the plan over each batch and
                # publishes one scalar per op (FPREDUCE), so the full result never
                # enters the segment. An older extension answers "unknown op" to the
                # first request without consuming a row; the client-side reduce
                # below is then the fallback, unchanged.
                if _AGG_PUSHDOWN and hasattr(_client, "read_reduce") and hasattr(ring, "reduce"):
                    plan_str = _agg_plan(internal)
                    acc = [None] * len(internal)
                    seen = False
                    try:
                        while True:
                            n = ring.reduce(seg, plan_str, size)
                            values = _client.read_reduce(seg)
                            if len(values) != len(internal):
                                raise ProgrammingError("aggregate: the worker returned a different plan")
                            _agg_combine(acc, op_codes, values)
                            seen = True
                            if n == 0:
                                break
                    except RuntimeError as exc:
                        msg = str(exc)
                        if not seen and "unknown op" in msg:
                            acc = None
                        elif msg.startswith("reduce: "):
                            raise ProgrammingError(msg) from None
                        else:
                            raise

                if acc is None:
                    seen = False
                    resolved = None  # [(op, column index)], once the metadata is known
                    acc = [None] * len(internal)
                    while True:
                        n = ring.next(size)
                        if n == 0 and seen:
                            break
                        if resolved is None:
                            if any(column is not None for _, column in internal):
                                batch = _client.read(seg)  # metadata only: the names and their order
                                index = {c.name: i for i, c in enumerate(batch.columns)}
                                resolved = []
                                for op, column in internal:
                                    if column is None:
                                        resolved.append((op, -1))
                                        continue
                                    if isinstance(column, int):
                                        at = column if column >= 0 else len(index) + column
                                        if not 0 <= at < len(index):
                                            raise ProgrammingError(f"aggregate: column index {column} is out of range")
                                    else:
                                        try:
                                            at = index[column]
                                        except KeyError:
                                            raise ProgrammingError(
                                                f"aggregate: no column {column!r} in the result"
                                            ) from None
                                    resolved.append((op, at))
                            else:
                                resolved = [(op, -1) for op, _column in internal]
                        try:
                            values = _client.reduce(seg, resolved)
                        except ValueError as exc:
                            raise ProgrammingError(str(exc)) from exc
                        _agg_combine(acc, [op for op, _at in resolved], values)
                        seen = True
                        if n == 0:
                            break
            finally:
                try:
                    ring.close()
                except Exception:
                    pass
                _ring_release(info)

        self.connection.shm_reads += 1
        result = {}
        for name, kind, positions in outputs:
            if kind == "avg":
                result[name] = _pg_avg(acc[positions[0]], acc[positions[1]])
            else:
                result[name] = acc[positions[0]]
        return result

    def _track(self, stream):
        """One live stream per connection, trackable so close() can end it.

        The tracked generator is the inner one: closing it runs its ``finally``
        (CLOSE then release), whichever way the stream ends — exhaustion, the
        caller's ``.close()``, GC, or the connection closing under it.
        """
        conn = self.connection
        conn._streams.add(stream)
        try:
            yield from stream
        finally:
            conn._streams.discard(stream)

    def _stream(self, sql, size):
        if not self.connection._shm:
            raise NotSupportedError("stream() is disabled on this connection (shm=False)")
        if not _is_read_only(sql):
            raise NotSupportedError("stream() serves read-only SELECTs over /dev/shm")
        fan = self._stream_fanout(sql, size)
        if fan is not None:
            yield from fan
            return
        info = _ring_acquire(self.connection._conn, block=self.connection._wait)
        if info is None:
            if pool_size() > 0:
                _note_fallback("pool exhausted", warn=False)
            raise NotSupportedError("stream() needs the fastpostgres extension (ring unavailable)")
        ring, seg, _pid = info
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

    def _stream_fanout(self, sql, size):
        """Batches from N partitions of `sql`, or None when it does not split.

        One ring per partition, every worker driven concurrently, batches handed
        back as each partition produces them. The views alias that partition's
        segment and are valid until it pulls again (its next batch, ~N batches
        later), which is the same contract the single-ring stream has.
        """
        want = _fanout_want(self.connection._conn, None)
        if want < 2:
            return None
        # This path concatenates each partition's rows. That is only the answer
        # when the projection preserves rows; an aggregate there would yield one
        # partial row per partition. `aggregate()` splits those and combines.
        if not partition.concat_safe(sql):
            return None
        split = _split(self.connection._conn, sql, want)
        if split is None:
            return None
        infos = []
        try:
            infos = _acquire_many(self.connection._conn, min(want, len(split.sqls)))
        except Exception:
            for info in infos:
                _ring_release(info)
            split.close()
            return None
        if len(infos) < 2:
            for info in infos:
                _ring_release(info)
            split.close()
            return None

        rings = [_FanRing(info, split.sqls[n :: len(infos)]) for n, info in enumerate(infos)]
        return self._fan_generator(rings, split, size)

    def _fan_generator(self, rings, split, size):
        last = None
        yielded = False
        try:
            for part in rings:
                part.open(size)
            while any(part.posted for part in rings):
                for part in rings:
                    if not part.posted:
                        continue
                    n = part.ring.wait()
                    part.posted = False
                    segname = part.segments()[part.slot]
                    last = segname
                    if n:
                        part.slot ^= 1
                        part.post(part.slot, size)
                        yielded = True
                        yield _client.read(segname)
                    else:
                        part.advance(size)  # next range, or this ring is done
            if not yielded and last is not None:
                yield _client.read(last)  # an empty result still carries its schema
        finally:
            for part in rings:
                part.close()
                _ring_release(part.info)
            split.close()

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

    def fetchall(self, force_tuples=None):
        """The remaining rows.

        A lazy result is handed back as the ``ResultSet`` itself, so asking for
        "all" does not build a tuple per row. ``force_tuples=True`` (or a
        connection opened with ``force_tuples=True``/``FASTPOSTGRES_MATERIALIZE=1``)
        returns a plain list instead.
        """
        force = self.connection._force_tuples if force_tuples is None else force_tuples
        if self._pos == 0:  # the common case: hand the result over instead of copying it
            self._pos = len(self._rows)
            rows = self._rows
        else:
            rows = self._rows[self._pos :]
            self._pos = len(self._rows)
        if force and isinstance(rows, ResultSet):
            return rows.to_tuples()
        return rows

    def __iter__(self):
        rows = self._rows
        if isinstance(rows, ResultSet) and self._pos == 0:
            self._pos = len(rows)
            return iter(rows)
        return self._iter_fetch()

    def _iter_fetch(self):
        while True:
            row = self.fetchone()
            if row is None:
                return
            yield row

    def close(self):
        rows, self._rows = self._rows, []
        if isinstance(rows, ResultSet):
            rows.close()  # a leased result returns its partition now
        self._pos = 0
        self.description = None

    def setinputsizes(self, sizes):
        pass

    def setoutputsize(self, size, column=None):
        pass


def connect(conninfo: str = "", **kwargs) -> Connection:
    return Connection(conninfo, **kwargs)
