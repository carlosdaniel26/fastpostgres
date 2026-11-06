"""A read-only, bulk-loaded columnar table in a sealed memfd.

The heap path moves a result through a frame: the server scans row-major pages,
deforms the tuples, writes a columnar payload into /dev/shm, and the client
decodes it.  A ``ColumnarTable`` is the same columns with the server pass done
once, at load time, and no per-read server work at all:

    table = conn.columnar_load("SELECT id, sku, price FROM sales")
    table.row_count                      # n rows, already columnar
    table.columns[0].data                # a read-only memoryview over the memfd
    table.reduce({"total": ("sum", "price"), "rows": ("count_all", None)})

The store is an anonymous, sealed memfd in the FPCOLUMN layout the client
already reads (``fastpostgres/fastpostgres_shm.h``).  The backend keeps the
descriptor open and reports it as ``FPQCOL1 <pid> <fd> <bytes> <rows>``, but the
capability itself is delivered over an AF_UNIX control socket with
``SCM_RIGHTS``: this module publishes an abstract listening address, passes it
to the load function, and receives the sealed descriptor.  ``SO_PEERCRED`` on
that socket is checked against ``PQbackendPID``, so only the backend this
connection is talking to can hand over a table — and the client and server need
not share a UID.  The mapping outlives the backend: the pages are immutable and
the mapping holds the object.

If the server predates the three-argument function, or the socket handoff
fails, the module falls back to opening ``/proc/<pid>/fd/<fd>`` (the original
path, same-UID only).  The descriptor string is deliberately unchanged, so both
paths attach the identical object.

There is no MVCC here.  The snapshot is the query's snapshot at load time; later
commits are invisible and an UPDATE does not exist.  The table is read-only and
bulk-loaded by construction; see DESIGN.md for what a real table AM would add.
"""
import array
import os
import socket
import struct
import time

from . import _client

__all__ = ["ColumnarTable", "load"]

# One record from the backend: magic, pid, bytes, rows (native layout, both
# sides little-endian because both are the same machine over AF_UNIX).
_HANDOFF_MAGIC = 0x46514348  # "FQCH"
_HANDOFF_STRUCT = struct.Struct("<IiQQ")  # magic, pid, bytes, rows
_HANDOFF_TIMEOUT = 2.0


class _HandoffSocket:
    """The client's end of the SCM_RIGHTS capability transfer.

    A fresh abstract AF_UNIX address per load keeps concurrent loads and
    concurrent processes from colliding; there is no filesystem object to clean
    up.  The backend connects once, sends one SEQPACKET record with the sealed
    memfd attached, and the client checks the sender's credentials before it
    touches the descriptor.
    """

    __slots__ = ("_sock", "_done", "address")

    def __init__(self):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET | socket.SOCK_CLOEXEC)
        # Leading NUL = Linux abstract namespace (no path, auto-reclaimed).
        name = f"\0fpqcol-{os.getpid()}-{time.monotonic_ns():x}"
        try:
            sock.bind(name)
            sock.listen(1)
            sock.settimeout(_HANDOFF_TIMEOUT)
        except OSError:
            sock.close()
            raise
        self._sock = sock
        self.address = "@" + name[1:]  # '@' is how the text argument spells NUL
        self._done = False

    def receive(self):
        """Accept one record; returns (fd, pid, bytes, rows, peercred, ms)."""
        started = time.perf_counter()
        conn = None
        fd = -1
        try:
            conn, _ = self._sock.accept()
            conn.settimeout(_HANDOFF_TIMEOUT)

            # SO_PEERCRED is the sender's identity as the kernel knows it.  The
            # caller compares it with PQbackendPID; the record's pid must agree
            # with both, so a local process that guesses the address still
            # cannot pass off another backend's (or its own) descriptor.
            raw = conn.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i"))
            peer_pid, peer_uid, peer_gid = struct.unpack("3i", raw)

            msg, ancillary, _flags, _addr = conn.recvmsg(256, socket.CMSG_SPACE(4))
            for level, kind, data in ancillary:
                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                    fd = array.array("i", data[:4])[0]
                    break
            if fd < 0:
                raise RuntimeError("columnar handoff: the backend sent no descriptor")
            if len(msg) < _HANDOFF_STRUCT.size:
                raise RuntimeError("columnar handoff: truncated record")

            magic, pid, size, rows = _HANDOFF_STRUCT.unpack_from(msg)
            if magic != _HANDOFF_MAGIC:
                raise RuntimeError("columnar handoff: bad record magic")
            if pid != peer_pid:
                raise RuntimeError("columnar handoff: record pid does not match SO_PEERCRED")

            return fd, pid, size, rows, (peer_pid, peer_uid, peer_gid), (time.perf_counter() - started) * 1000.0
        except BaseException:
            if fd >= 0:
                os.close(fd)
            raise
        finally:
            if conn is not None:
                conn.close()
            self.close()

    def close(self):
        if not self._done:
            self._done = True
            self._sock.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

_AGG_OPS = {
    "count_all": _client.AGG_COUNT_ALL,
    "count": _client.AGG_COUNT,
    "sum": _client.AGG_SUM,
    "min": _client.AGG_MIN,
    "max": _client.AGG_MAX,
}


class ColumnarTable:
    """One bulk-loaded SELECT: immutable columns in a mapped memfd.

    ``columns`` is the client's ordinary ``Segment`` column list, so every
    existing zero-copy consumer works unchanged; ``reduce`` and ``scan_all``
    walk the same bytes in C++ with no per-row Python object.
    """

    __slots__ = (
        "_pid",
        "_fd",
        "_size",
        "_rows",
        "query",
        "_segment",
        "_index",
        "_handle",
        "handoff",
        "peer",
        "handoff_ms",
    )

    def __init__(self, pid, fd, size, rows, query=None, handle=None):
        self._pid = pid
        self._fd = fd
        self._size = size
        self._rows = rows
        self.query = query
        self._segment = None
        self._index = None
        # A received-descriptor handle, or None for the /proc path.
        self._handle = handle
        self.handoff = "proc"  # "scm" once a descriptor arrived over the socket
        self.peer = None  # (pid, uid, gid) from SO_PEERCRED on the control socket
        self.handoff_ms = 0.0

    @property
    def row_count(self):
        return self._rows

    def _attach(self):
        """Map the memfd and build the zero-copy column views (once)."""
        if self._segment is None:
            seg = _client.columnar_attach(self._pid, self._fd, self._size)
            self._segment = seg
            self._index = {c.name: i for i, c in enumerate(seg.columns)}
        return self._segment

    @property
    def columns(self):
        """The zero-copy column views (immutable: safe to keep)."""
        return self._attach().columns

    def column(self, name):
        """One column by name."""
        self._attach()
        try:
            return self._segment.columns[self._index[name]]
        except KeyError:
            raise KeyError(f"no column {name!r}; have {list(self._index)}") from None

    def reduce(self, ops):
        """Reduce the loaded columns in C++, no Python object per row.

        ``ops`` has the same shape as ``Cursor.aggregate``: a mapping from a
        result name to ``(operation, column)``.  The walk happens over the
        mapped memfd with the GIL released; there is one table, so no per-batch
        combination is needed.
        """
        self._attach()

        plan = []
        for name, spec in ops.items():
            op_name, column = spec
            try:
                op = _AGG_OPS[op_name]
            except KeyError:
                raise ValueError(f"reduce: unknown operation {op_name!r}") from None
            if op == _client.AGG_COUNT_ALL:
                if column is not None:
                    raise ValueError("reduce: count_all takes no column (pass None)")
                index = -1
            else:
                if column is None:
                    raise ValueError(f"reduce: {op_name} needs a column")
                if isinstance(column, str):
                    self._attach()
                    try:
                        index = self._index[column]
                    except KeyError:
                        raise KeyError(f"reduce: no column {column!r}; have {list(self._index)}") from None
                else:
                    index = int(column)
                    if index < 0 or index >= len(self._segment.columns):
                        raise ValueError(f"reduce: column index {index} is out of range")
            plan.append((op, index))

        if self._handle is not None:
            values = _client.columnar_reduce_handle(self._handle, self._size, plan)
        else:
            values = _client.columnar_reduce(self._pid, self._fd, self._size, plan)
        return dict(zip(ops, values))

    def scan_all(self):
        """Walk every byte of every column in place.

        Returns ``(rows, data_bytes, checksum, ncols)``.  This is a real
        ``SELECT *`` scan: every fixed-width slot and every varlena payload byte
        is read from the mapping, with no row objects and no copy.  ``checksum``
        is an FNV-1a over the values as laid out, so a caller can prove it read
        the same bytes twice.
        """
        if self._handle is not None:
            return _client.columnar_scan_handle(self._handle, self._size)
        return _client.columnar_scan(self._pid, self._fd, self._size)

    def select(self, *names):
        """A tuple of zero-copy column views, by name."""
        return tuple(self.column(n) for n in names)

    def __repr__(self):
        return f"ColumnarTable(rows={self._rows}, bytes={self._size}, columns={len(self.columns)})"


def _literal(text):
    return "'" + text.replace("'", "''") + "'"


def _descriptor(result):
    if not result.rows:
        raise RuntimeError("fastpostgres: columnar load returned no descriptor")
    descriptor = result.rows[0][0]
    parts = descriptor.split()
    if len(parts) != 5 or parts[0] != "FPQCOL1":
        raise RuntimeError(f"fastpostgres: unexpected columnar descriptor {descriptor!r}")
    try:
        pid, fd, size, rows = (int(x) for x in parts[1:])
    except ValueError:
        raise RuntimeError(f"fastpostgres: unexpected columnar descriptor {descriptor!r}") from None
    return pid, fd, size, rows


def _load_scm(connection, query, capacity, expected_pid):
    """Try the SCM_RIGHTS handoff; return a table, or None for the /proc path."""
    handoff = _HandoffSocket()
    try:
        sql = (
            f"SELECT fastpostgres_columnar_load({_literal(query)}, {int(capacity)}, "
            f"{_literal(handoff.address)})"
        )
        try:
            result = connection._conn.exec(sql)
        except RuntimeError as exc:
            # A server without the three-argument overload is the one case that
            # falls back silently: it has no handoff to offer.  Everything else
            # (a bad query, a permission error) is the caller's to see.
            message = str(exc)
            if "fastpostgres_columnar_load" in message and "does not exist" in message:
                return None
            raise

        pid, fd, size, rows = _descriptor(result)
        try:
            got_fd, peer_pid, got_size, got_rows, peer, recv_ms = handoff.receive()
        except (OSError, RuntimeError):
            # The three-argument function exists but the capability transfer
            # did not arrive.  The object is already loaded and its descriptor
            # string still works through /proc for a same-UID client, so keep
            # this table rather than running the query a second time.
            table = ColumnarTable(pid, fd, size, rows, query)
            table._attach()
            return table

        if peer_pid != expected_pid:
            os.close(got_fd)
            raise RuntimeError(
                "fastpostgres: columnar handoff came from pid %d, not this "
                "connection's backend %d" % (peer_pid, expected_pid)
            )
        if got_size != size or got_rows != rows:
            os.close(got_fd)
            raise RuntimeError("fastpostgres: columnar handoff does not match its descriptor")

        try:
            segment, handle = _client.columnar_attach_fd(got_fd, size)
        finally:
            os.close(got_fd)  # the mapping holds the object
        table = ColumnarTable(pid, fd, size, rows, query, handle=handle)
        table._segment = segment
        table._index = {c.name: i for i, c in enumerate(segment.columns)}
        table.handoff = "scm"
        table.peer = peer
        table.handoff_ms = recv_ms
        return table
    finally:
        handoff.close()


def load(connection, query, capacity=0):
    """Bulk-load a read-only SELECT into a sealed memfd via ``connection``.

    ``connection`` is a ``fastpostgres.dbapi.Connection``; the load runs on its
    libpq control channel (not the ring), because the descriptor has to be owned
    by the backend this session is talking to.  ``capacity`` is the memfd's
    virtual size before the exact payload is known (0 = the extension's 1 GiB
    default).

    The descriptor is handed over an AF_UNIX ``SCM_RIGHTS`` socket and checked
    with ``SO_PEERCRED`` against ``PQbackendPID``.  If that path is unavailable,
    the original ``/proc/<pid>/fd/<fd>`` attach is used instead; both map the
    same sealed FPCOLUMN object.
    """
    expected_pid = connection._conn.backend_pid()

    table = _load_scm(connection, query, capacity, expected_pid)
    if table is not None:
        return table

    result = connection._conn.exec(
        f"SELECT fastpostgres_columnar_load({_literal(query)}, {int(capacity)})"
    )
    pid, fd, size, rows = _descriptor(result)
    table = ColumnarTable(pid, fd, size, rows, query)
    # Map now, while the backend that owns the descriptor is known to be alive:
    # the mapping then keeps the object after the connection goes away.
    table._attach()
    return table
