"""fastpostgres.aio — the asyncio face of the client.

One event loop, many connections, each connection with one **tier thread** of its
own: every blocking piece of the sync path runs there — ``PQconnectdb``/``PQexec``,
and the ring's semaphore waits (10 s budget) — with the GIL released, so the loop
keeps running while a query is in flight. The GIL-bound decode runs on that same
tier thread; threads overlap the worker's turn with the decode, they do not
parallelize it (see ``bench/NOTES.md`` on the GIL).

    import asyncio
    from fastpostgres import aio

    async def main():
        conn = await aio.connect("host=/tmp dbname=postgres user=postgres")
        rows = await conn.fetchall("select g::int from generate_series(1, 3) g")
        async with conn.stream("select id, price from products") as batches:
            async for batch in batches:            # zero-copy Segment
                total += int(np.asarray(batch.columns[1]).sum())
        await conn.close()

    asyncio.run(main())

The shared-memory ring pool is process-wide, exactly as in ``dbapi``: a stream
checks a ring out for its lifetime, and several connections' tiers can hold
several partitions at once. ``wait=True`` (the default here) blocks a tier for a
free ring instead of silently re-running the statement over libpq, so async
concurrency degrades by queueing, not by quietly doubling the work.

Limitations, deliberately: there is no ``PQcancel`` (v1), so cancelling a task
does not interrupt the tier's job — the job finishes, its result is dropped, and
the stream state has moved on; and a ``memoryview`` a batch hands out is valid
until *that stream's* next pull, so copy whatever must outlive it.
"""
import asyncio
import os
import queue
import threading

from . import dbapi
from .dbapi import (
    DatabaseError,
    Error,
    IntegrityError,
    InterfaceError,
    NotSupportedError,
    OperationalError,
    ProgrammingError,
)

__all__ = [
    "connect",
    "AsyncConnection",
    "AsyncCursor",
    "AsyncStream",
    "Error",
    "InterfaceError",
    "DatabaseError",
    "OperationalError",
    "ProgrammingError",
    "IntegrityError",
    "NotSupportedError",
]

threadsafety = 1  # a connection belongs to one loop and one tier thread

_STOP = object()


def _settle(future, exc, result):
    """Settle a tier result on the loop thread (the loop calls this)."""
    if future.cancelled():
        return
    if exc is not None:
        future.set_exception(exc)
    else:
        future.set_result(result)


class _Tier:
    """A connection's own thread, and the jobs it runs in order.

    One thread per connection, not a shared executor: a ``PGconn`` is one
    libpq handle and a pool could run two calls on it at once. FIFO is the only
    ordering a connection can honestly promise, and a daemon thread keeps a
    stuck ring wait from holding up interpreter exit.
    """

    def __init__(self, pid):
        self._pid = pid
        self._loop = None
        self._jobs = queue.SimpleQueue()
        self._thread = threading.Thread(target=self._run, name="fastpostgres-aio", daemon=True)
        self._thread.start()

    def _run(self):
        while True:
            job = self._jobs.get()
            if job is _STOP:
                return
            fn, future = job
            try:
                result = fn()
            except BaseException as exc:  # carried to the loop, whatever it is
                if future is not None:
                    self._loop.call_soon_threadsafe(_settle, future, exc, None)
            else:
                if future is not None:
                    self._loop.call_soon_threadsafe(_settle, future, None, result)

    def submit(self, fn):
        """Queue ``fn`` on the tier; the returned future settles on this loop."""
        if self._pid != os.getpid():
            raise InterfaceError("fastpostgres.aio: this connection belongs to another process (fork?)")
        loop = asyncio.get_running_loop()
        if self._loop is None:
            self._loop = loop
        elif self._loop is not loop:
            raise InterfaceError("fastpostgres.aio: one connection belongs to one event loop")
        future = loop.create_future()
        self._jobs.put((fn, future))
        return future

    def post(self, fn):
        """Run ``fn`` on the tier without waiting (teardown from ``__del__``)."""
        if self._pid != os.getpid():
            return
        self._jobs.put((fn, None))

    def shutdown(self):
        if self._loop is not None:
            self._jobs.put(_STOP)


def _pull(gen):
    """One pull of the sync stream generator, with its exhaustion as a value."""
    try:
        return next(gen)
    except StopIteration:
        return None


class AsyncStream:
    """``async for`` over the columnar batches of one read-only SELECT.

    Each batch is the ``Segment`` the sync path yields — zero-copy column views
    aliasing the mapped /dev/shm partition. The views are valid until this
    stream's next pull (the worker rewrites that partition then); nothing writes
    it in between, so a batch is safe to read across the awaits inside the loop
    body. Copy or reduce what must outlive the next pull.

    One pull is in flight at a time: a second concurrent pull raises
    ``RuntimeError``. Close it with ``async with`` or ``aclose()`` — abandoning
    the loop body returns the partition through the sync ``finally``, and the
    connection closing under the stream ends it too.
    """

    def __init__(self, connection, cursor, operation, parameters, size):
        self._connection = connection
        self._cursor = cursor
        self._operation = operation
        self._parameters = parameters
        self._size = size
        self._gen = None
        self._done = False
        self._busy = False

    def _open(self):
        if self._gen is None:
            self._gen = self._cursor._sync().stream(self._operation, self._parameters, size=self._size)
        return self._gen

    def __aiter__(self):
        return self

    async def __anext__(self):
        if self._done:
            raise StopAsyncIteration
        if self._busy:
            raise RuntimeError("fastpostgres.aio: a pull is already in flight on this stream")
        self._busy = True
        try:
            await self._connection._open()  # the sync cursor needs the dbapi connection
            batch = await self._connection._submit(lambda: _pull(self._open()))
        finally:
            self._busy = False
        if batch is None:
            self._done = True
            self._connection._streams.discard(self)
            raise StopAsyncIteration
        return batch

    async def aclose(self):
        """End the stream on its tier: CLOSE over the ring, then release it."""
        gen, self._gen = self._gen, None
        self._done = True
        self._connection._streams.discard(self)
        if gen is not None:
            await self._connection._submit(gen.close)

    async def __aenter__(self):
        if self._connection._closed:
            raise InterfaceError("fastpostgres.aio: connection is closed")
        await self._connection._open()
        return self

    async def __aexit__(self, *exc):
        await self.aclose()

    def __del__(self):
        try:
            gen, self._gen = self._gen, None
            if gen is not None:
                self._connection._tier.post(gen.close)
        except Exception:
            pass


class AsyncCursor:
    """The awaitable cursor: ``await execute()``, then fetch, or ``stream()``.

    A cursor belongs to one task at a time. Fetch calls walk the rows
    ``execute()`` already materialized, so they need no tier round trip.
    """

    def __init__(self, connection):
        self._connection = connection
        self._cur = None
        self._closed = False
        self.arraysize = 1
        self.description = None
        self.rowcount = -1

    def _sync(self):
        """The dbapi cursor, made once the connection exists (no I/O)."""
        if self._cur is None:
            self._cur = self._connection._conn.cursor()
        return self._cur

    async def execute(self, operation, parameters=None):
        if self._closed:
            raise InterfaceError("fastpostgres.aio: cursor is closed")
        await self._connection._open()
        cur = self._sync()
        await self._connection._submit(lambda: cur.execute(operation, parameters))
        self.description = cur.description
        self.rowcount = cur.rowcount
        return self

    async def executemany(self, operation, seq_of_parameters):
        if self._closed:
            raise InterfaceError("fastpostgres.aio: cursor is closed")
        await self._connection._open()
        cur = self._sync()
        await self._connection._submit(lambda: cur.executemany(operation, seq_of_parameters))
        self.description = cur.description
        self.rowcount = cur.rowcount
        return self

    async def fetchone(self):
        return self._sync().fetchone()

    async def fetchmany(self, size=None):
        return self._sync().fetchmany(size)

    async def fetchall(self, force_tuples=None):
        return self._sync().fetchall(force_tuples)

    def stream(self, operation, parameters=None, size=dbapi._SHM_BATCH) -> AsyncStream:
        sql = operation if parameters is None else dbapi._inline(operation, parameters)
        if not self._connection._shm or not dbapi._is_read_only(sql):
            raise NotSupportedError("stream() serves read-only SELECTs over /dev/shm")
        stream = AsyncStream(self._connection, self, operation, parameters, size)
        self._connection._streams.add(stream)
        return stream

    async def aggregate(self, operation, ops, parameters=None, size=dbapi._SHM_BATCH) -> dict:
        """Reduce a read-only SELECT in C++ (see ``dbapi.Cursor.aggregate``).

        One tier job, and inside it the segment walk runs with the GIL released:
        an async caller gets numbers out without any Python object per row.
        """
        if self._closed:
            raise InterfaceError("fastpostgres.aio: cursor is closed")
        await self._connection._open()
        cur = self._sync()
        return await self._connection._submit(
            lambda: cur.aggregate(operation, ops, parameters, size)
        )

    async def close(self):
        self._closed = True
        self._cur = None


class AsyncConnection:
    """One libpq channel, driven by its own tier thread.

    Autocommit, like the sync driver: ``commit()``/``rollback()`` are awaitable
    no-ops. Use it as an async context manager; ``close()`` ends any live stream
    first, so a partition is never left checked out.
    """

    def __init__(self, conninfo: str = "", shm: bool = True, wait: bool = True,
                 force_tuples=None, **kwargs):
        self._conninfo = conninfo
        self._shm = shm
        self._wait = wait
        self._force_tuples = force_tuples
        self._kwargs = kwargs
        self._tier = _Tier(os.getpid())
        self._conn = None
        self._closed = False
        self._streams = set()

    @classmethod
    async def connect(cls, conninfo: str = "", **kwargs) -> "AsyncConnection":
        """Open a connection now: ``await AsyncConnection.connect(dsn)``."""
        conn = cls(conninfo, **kwargs)
        await conn._open()
        return conn

    async def _open(self):
        """The dbapi connection, opened on the tier on first use."""
        if self._conn is None:
            if self._closed:
                raise InterfaceError("fastpostgres.aio: connection is closed")
            self._conn = await self._submit(
                lambda: dbapi.Connection(self._conninfo, shm=self._shm, wait=self._wait,
                                         force_tuples=self._force_tuples, **self._kwargs)
            )
        return self._conn

    async def _submit(self, fn):
        """Run ``fn`` on this connection's tier, translating the ring's timeout."""
        try:
            return await self._tier.submit(fn)
        except TimeoutError as exc:  # a ring wait, not an asyncio timeout
            raise OperationalError(str(exc)) from exc

    # ---- statements -------------------------------------------------------

    def cursor(self) -> AsyncCursor:
        return AsyncCursor(self)

    def compile(self, statement, **params) -> str:
        """The SQL text a statement would run — pure SQLAlchemy work, no I/O."""
        if params:
            if isinstance(statement, str):
                raise ValueError("compile(sql, **params): bind them into the SQL text yourself")
            statement = statement.params(**params)
        return dbapi._compiled(statement)

    async def fetchall(self, statement, force_tuples=None, **params):
        """Run a SQLAlchemy statement (or SQL text) and return its rows."""
        sql = self.compile(statement, **params)
        conn = await self._open()

        def job():
            cur = conn.cursor()
            cur.execute(sql)
            return cur.fetchall(force_tuples)

        return await self._submit(job)

    async def first(self, statement, **params):
        """The first row of a statement, or None."""
        sql = self.compile(statement, **params)
        conn = await self._open()

        def job():
            cur = conn.cursor()
            cur.execute(sql)
            return cur.fetchone()

        return await self._submit(job)

    def stream(self, operation, parameters=None, size=dbapi._SHM_BATCH) -> AsyncStream:
        """An async iterator of zero-copy batches, as ``Cursor.stream()``."""
        return self.cursor().stream(operation, parameters, size=size)

    async def aggregate(self, statement, ops, **params) -> dict:
        """Reduce a statement in C++ and return its numbers (no row objects)."""
        sql = self.compile(statement, **params)
        conn = await self._open()

        def job():
            return conn.cursor().aggregate(sql, ops)

        return await self._submit(job)

    # ---- lifecycle --------------------------------------------------------

    async def commit(self):
        """No-op: the driver is autocommit."""

    async def rollback(self):
        """No-op: the driver is autocommit."""

    def stats(self) -> dict:
        """The pool's counters and fallbacks, plus this connection's own."""
        stats = dbapi.pool_stats()
        stats["shm_reads"] = self._conn.shm_reads if self._conn is not None else 0
        stats["closed"] = self._closed
        return stats

    async def close(self):
        """End the live streams, close the channel, stop the tier thread."""
        if self._closed:
            return
        self._closed = True
        for stream in list(self._streams):
            try:
                await stream.aclose()
            except Exception:
                pass
        self._streams.clear()
        conn, self._conn = self._conn, None
        if conn is not None:
            await self._submit(lambda: conn.close())
        self._tier.shutdown()

    async def __aenter__(self):
        await self._open()
        return self

    async def __aexit__(self, *exc):
        await self.close()

    def __del__(self):
        try:
            conn, self._conn = self._conn, None
            if conn is not None:
                self._tier.post(conn.close)
            self._tier.post(lambda: None)
        except Exception:
            pass


async def connect(conninfo: str = "", shm: bool = True, wait: bool = True, **kwargs) -> AsyncConnection:
    """Open an async connection: one libpq channel and one tier thread.

    ``wait`` (default True) blocks a tier for a free ring instead of falling
    back to libpq; ``shm=False`` disables the ring path for this connection.
    """
    conn = AsyncConnection(conninfo, shm=shm, wait=wait, **kwargs)
    await conn._open()
    return conn
