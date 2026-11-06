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
descriptor open and reports it as ``FPQCOL1 <pid> <fd> <bytes> <rows>``; this
module opens ``/proc/<pid>/fd/<fd>`` read-only and maps it once.  The mapping
outlives the backend: the pages are immutable and the mapping holds the object.

There is no MVCC here.  The snapshot is the query's snapshot at load time; later
commits are invisible and an UPDATE does not exist.  The table is read-only and
bulk-loaded by construction; see DESIGN.md for what a real table AM would add.
"""
from . import _client

__all__ = ["ColumnarTable", "load"]

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

    __slots__ = ("_pid", "_fd", "_size", "_rows", "query", "_segment", "_index")

    def __init__(self, pid, fd, size, rows, query=None):
        self._pid = pid
        self._fd = fd
        self._size = size
        self._rows = rows
        self.query = query
        self._segment = None
        self._index = None

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
        return _client.columnar_scan(self._pid, self._fd, self._size)

    def select(self, *names):
        """A tuple of zero-copy column views, by name."""
        return tuple(self.column(n) for n in names)

    def __repr__(self):
        return f"ColumnarTable(rows={self._rows}, bytes={self._size}, columns={len(self.columns)})"


def _literal(text):
    return "'" + text.replace("'", "''") + "'"


def load(connection, query, capacity=0):
    """Bulk-load a read-only SELECT into a sealed memfd via ``connection``.

    ``connection`` is a ``fastpostgres.dbapi.Connection``; the load runs on its
    libpq control channel (not the ring), because the descriptor has to be owned
    by the backend the client can name.  ``capacity`` is the memfd's virtual size
    before the exact payload is known (0 = the extension's 1 GiB default).
    """
    sql = f"SELECT fastpostgres_columnar_load({_literal(query)}, {int(capacity)})"
    result = connection._conn.exec(sql)  # libpq: the backend that will hold the fd
    if not result.rows:
        raise RuntimeError("fastpostgres: columnar load returned no descriptor")

    descriptor = result.rows[0][0]
    parts = descriptor.split()
    if len(parts) != 5 or parts[0] != "FPQCOL1":
        raise RuntimeError(f"fastpostgres: unexpected columnar descriptor {descriptor!r}")

    pid, fd, size, rows = (int(x) for x in parts[1:])
    table = ColumnarTable(pid, fd, size, rows, query)
    # Map now, while the backend that owns the descriptor is known to be alive:
    # the mapping then keeps the object after the connection goes away.
    table._attach()
    return table
