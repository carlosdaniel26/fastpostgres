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
import queue
import re
import threading
import time
import warnings
from bisect import bisect_right
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


def _agg_plan(plan) -> str:
    """The worker's plan string: ``op[:i<index>|n<name>]``, comma separated.

    Names escape ',' and '\\'; an integer column is explicit so a column literally
    named like an index cannot be mistaken for one.
    """
    parts = []
    for _name, op, column in plan:
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


class ResultSet:
    """The rows of a read, held columnar and decoded one row at a time.

    ``execute()`` on a read-only SELECT copies each columnar batch out of
    /dev/shm (one memcpy per column, no Python object per value) and hands back
    this sequence instead of a list of tuples. ``len()``, indexing, slicing,
    iteration, ``keys()`` and ``fetchone()``/``fetchmany()`` all work as they do
    on the old list; a tuple is built only for the row that was asked for.

    The copy is what makes the lifetime unbounded: the ring goes straight back
    to the pool, and later reads cannot overwrite the rows this result holds.
    ``to_tuples()`` materialises the whole result in C, through the same
    specialized decode the eager path uses; ``FASTPOSTGRES_MATERIALIZE=1`` (or
    ``force_tuples=True`` on a connection) does it at ``fetchall()`` time.
    """

    __slots__ = ("_frames", "_keys", "_types", "_starts", "_total", "_materialized")

    def __init__(self, frames, keys, field_types, total):
        self._frames = frames  # [(nrows, [Column, ...])], in result order
        self._keys = tuple(keys)
        self._types = tuple(field_types)
        starts = [0]
        for nrows, _columns in frames:
            starts.append(starts[-1] + nrows)
        self._starts = starts
        self._total = total
        self._materialized = None

    # ---- sequence protocol ------------------------------------------------

    def __len__(self):
        return self._total

    def _row_at(self, index):
        frame = bisect_right(self._starts, index) - 1
        nrows, columns = self._frames[frame]
        row = index - self._starts[frame]
        return _client.decode_row(columns, row)

    def __getitem__(self, key):
        if isinstance(key, slice):
            if self._materialized is not None:
                return self._materialized[key]
            return [self._row_at(i) for i in range(*key.indices(self._total))]
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
        for nrows, columns in self._frames:
            decode = _client.decode_row
            for row in range(nrows):
                yield decode(columns, row)

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
            rows = []
            for nrows, columns in self._frames:
                if nrows:
                    rows.extend(_client.decode_frame(columns, nrows))
            self._materialized = rows
        return self._materialized

    materialize = to_tuples

    def fetchall(self):
        """DB-API convenience: the whole result as tuples."""
        return self.to_tuples()


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
# FASTPOSTGRES_MEMFD=1 makes the worker back those segments with sealed memfds
# and hand each descriptor to the client once over SCM_RIGHTS, instead of
# widening a named /dev/shm file to 0666. Default off: the named segment is what
# a client running as a different user than the server needs.
#
# A checked-out ring is a lease: one thread drives it until it hands it back,
# and the C layer refuses a second driver. That is what lets several threads
# (the asyncio layer's tiers) each work their own partition of the shared
# memory; the pool is the partition table.

_POOL_ENV = os.environ.get("FASTPOSTGRES_SHM_POOL")
_SEGMENT_ENV = os.environ.get("FASTPOSTGRES_SEGMENT_BYTES")
_SEGMENT_BYTES = int(_SEGMENT_ENV) if _SEGMENT_ENV else 0
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
        ring = _client.Ring(name, _SEGMENT_BYTES, _TRANSPORT)
        ring.start(bootstrap)
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


def _ring_release(info):
    """Hand a checked-out partition back, unless it is not this process's.

    A forked child inherits its parent's rings: driving one would race the
    parent, and returning it to the child's idle queue would hand a stranger's
    ring to its own next reader. Drop it; the parent keeps using its own.
    """
    ring, _seg, pid = info
    with _pool_lock:
        _pool_leases.pop(threading.get_ident(), None)
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
                 force_tuples=None, **kwargs):
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

    def _shm_read_lazy(self, sql, size=_SHM_BATCH):
        """The same read, but the result is a columnar ResultSet.

        Two data segments alternate exactly as they do for the eager read: the
        request for the next batch is posted before the current one is copied,
        so the worker's turn hides behind the copy. What is copied is each
        column's region, not a tuple: the ring then goes straight back to the
        pool and the result owns its bytes. A row is decoded when it is asked
        for, so a consumer that only counts or narrows pays no per-value work.
        """
        if not self._shm:
            return None
        info = _ring_acquire(self._conn, block=self._wait)
        if info is None:
            if pool_size() > 0:  # size 0 is the explicit "no ring" configuration
                _note_fallback("pool exhausted", warn=False)
            return None
        ring, seg, _pid = info

        frames = []
        keys = types = None
        total = 0
        pending = False
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
                        ring.post(segs[(i + 1) & 1], size)
                        pending = True
                    batch = _client.read(segs[i & 1])
                    if keys is None:
                        keys = [column.name for column in batch.columns]
                        types = [column.pgtype for column in batch.columns]
                    if n:
                        columns = []
                        for column in batch.columns:
                            # One copy of the whole column region. The varlena
                            # kinds keep offsets and bytes together in `data`,
                            # so `bytes` is a slice of the copy, not a second
                            # one.
                            data = bytes(column.data)
                            payload = None
                            if column.bytes is not None:
                                payload = memoryview(data)[len(data) - len(column.bytes):]
                            nulls = bytes(column.nulls) if column.nulls is not None else None
                            columns.append(
                                _client.column(
                                    column.name, column.kind, column.pgtype, n,
                                    data, payload, nulls,
                                )
                            )
                        frames.append((n, columns))
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
        except Exception as exc:
            _note_fallback(type(exc).__name__, detail=f"{type(exc).__name__}: {exc}")
            return None  # re-run on libpq so the caller sees the real error
        finally:
            _ring_release(info)

        self.shm_reads += 1
        return ResultSet(frames, keys or [], types or [], total)

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
        generator returns it, and so does closing the connection. Requires the
        extension.
        """
        sql = operation if parameters is None else _inline(operation, parameters)
        return self._track(self._stream(sql, size))

    def aggregate(self, operation: str, ops, parameters=None, size=_SHM_BATCH) -> dict:
        """Reduce a read-only SELECT in C++: numbers out, no row objects.

        `ops` maps a result name to ``(operation, column)``:

            {"price_sum": ("sum", "price"), "rows": ("count_all", None),
             "cheapest": ("min", "price"), "priced": ("count", "price")}

        Operations are ``sum``, ``min``, ``max``, ``count`` (non-NULL values) and
        ``count_all`` (rows, no column needed — pass ``None``); a column is a name
        or an index. The reduce runs in the worker by default
        (``FASTPOSTGRES_OP_REDUCE``): it accumulates each batch where the tuples
        are and publishes a few scalars, so the client never receives the rows.
        Set ``FASTPOSTGRES_AGG_PUSHDOWN=0`` to force the older client-side walk;
        an extension that predates the op falls back to it automatically. Either
        way no Python object is built per row, so a thread reducing its own
        partition runs in parallel with the others on any interpreter — the
        decode that materializes rows is what serializes, and this never does it.

        Semantics are the obvious SQL ones: NULLs are skipped by sum/min/max and
        not counted by count; an empty result gives 0 for sums and counts and
        None for min/max. Integer sums accumulate in 64 bits and wrap on overflow
        (the server's ``sum(bigint)`` is numeric and exact, so a sum that leaves
        int64 will differ). Only the numeric kinds and bool are walked — anything
        else (text, bytea, dates, numeric) raises ``ProgrammingError``; let the
        server aggregate those.
        """
        sql = operation if parameters is None else _inline(operation, parameters)
        if not self.connection._shm:
            raise NotSupportedError("aggregate() is disabled on this connection (shm=False)")
        if not _is_read_only(sql):
            raise NotSupportedError("aggregate() serves read-only SELECTs over /dev/shm")

        plan = []
        for name, spec in ops.items():
            op_name, column = spec
            try:
                op = _AGG_OPS[op_name]
            except KeyError:
                raise ProgrammingError(f"aggregate: unknown operation {op_name!r}") from None
            if op == _client.AGG_COUNT_ALL:
                if column is not None:
                    raise ProgrammingError("aggregate: count_all takes no column (pass None)")
            elif column is None:
                raise ProgrammingError(f"aggregate: {op_name} needs a column")
            plan.append((name, op, column))

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
                plan_str = _agg_plan(plan)
                ops = [op for _, op, _ in plan]
                acc = [None] * len(plan)
                seen = False
                try:
                    while True:
                        n = ring.reduce(seg, plan_str, size)
                        values = _client.read_reduce(seg)
                        if len(values) != len(plan):
                            raise ProgrammingError("aggregate: the worker returned a different plan")
                        _agg_combine(acc, ops, values)
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
                acc = [None] * len(plan)
                while True:
                    n = ring.next(size)
                    if n == 0 and seen:
                        break
                    if resolved is None:
                        if any(column is not None for _, _, column in plan):
                            batch = _client.read(seg)  # metadata only: the names and their order
                            index = {c.name: i for i, c in enumerate(batch.columns)}
                            resolved = []
                            for name, op, column in plan:
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
                            resolved = [(op, -1) for _, op, _ in plan]
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
        return {name: acc[i] for i, (name, _, _) in enumerate(plan)}

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
        self._rows = []
        self._pos = 0
        self.description = None

    def setinputsizes(self, sizes):
        pass

    def setoutputsize(self, size, column=None):
        pass


def connect(conninfo: str = "", **kwargs) -> Connection:
    return Connection(conninfo, **kwargs)
