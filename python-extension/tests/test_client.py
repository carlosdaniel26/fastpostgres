"""Exercise the real client against a live server.

Needs a PostgreSQL with the fastpostgres extension loaded; the connection
string comes from ``DATABASE_URL`` (the docker harness sets it).

Zero-copy contract: a column's ``data``/``bytes``/``nulls`` memoryviews alias
the mapped segment, which the worker rewrites on the next chunk — decode (or
copy) before the next ``ring.next()``.
"""
import os
import struct
import unittest

import fastpostgres as fp

CONNINFO = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")

QUERY = (
    "SELECT g::int4 AS id, (g*2)::int8 AS big, (g*1.5)::float8 AS f, "
    "(g % 2 = 0)::bool AS b, ('row' || g)::text AS name "
    "FROM generate_series(1, 1000) g"
)


def cast(view, fmt):
    """Reinterpret a byte memoryview as native little-endian values."""
    count = len(view) // struct.calcsize(fmt)
    return struct.unpack_from("<%d%s" % (count, fmt), view, 0)


class ConnectionTest(unittest.TestCase):
    def setUp(self):
        self.conn = fp.Connection(CONNINFO)

    def tearDown(self):
        self.conn.close()

    def test_exec_returns_rows_and_metadata(self):
        res = self.conn.exec("SELECT g::int4 AS n, ('x' || g)::text AS s FROM generate_series(1, 3) g")
        self.assertEqual(res.fields, ["n", "s"])
        self.assertEqual(res.field_types, [23, 25])
        self.assertEqual(res.rows[0], (1, "x1"))
        self.assertEqual(res.rows[-1], (3, "x3"))
        self.assertEqual(res.row_count, 3)  # PQcmdTuples: rows returned by the SELECT

    def test_exec_types(self):
        res = self.conn.exec(
            "SELECT true AS b, 42::int8 AS big, 1.5::float8 AS f, 2.5::numeric AS n, "
            "'x'::text AS s, '2026-10-30 12:00:00+00'::timestamptz AS ts"
        )
        b, big, f, n, s, ts = res.rows[0]
        self.assertIs(b, True)
        self.assertEqual(big, 42)
        self.assertAlmostEqual(f, 1.5)
        self.assertEqual(str(n), "2.5")
        self.assertEqual(s, "x")
        self.assertEqual((ts.year, ts.month, ts.day, ts.hour), (2026, 10, 30, 12))

    def test_exec_command_row_count(self):
        self.conn.exec("DROP TABLE IF EXISTS fp_py_t; CREATE TABLE fp_py_t (id int)")
        res = self.conn.exec("INSERT INTO fp_py_t VALUES (1), (2), (3)")
        self.assertEqual(res.row_count, 3)
        self.conn.exec("DROP TABLE fp_py_t")

    def test_exec_null(self):
        res = self.conn.exec("SELECT NULL::int4 AS n")
        self.assertEqual(res.rows, [(None,)])


class RingReadTest(unittest.TestCase):
    def setUp(self):
        self.conn = fp.Connection(CONNINFO)
        self.ring = fp.Ring("fp_py_ring")
        self.ring.start(self.conn)

    def tearDown(self):
        try:
            self.ring.stop()
        finally:
            self.conn.close()

    def test_stream_decodes_columnar_chunk(self):
        self.ring.open("fp_py_seg", QUERY)

        names = kinds = None
        first = None
        total = 0
        while True:
            n = self.ring.next(500)
            seg = fp.read("fp_py_seg")
            self.assertEqual(seg.row_count, n)
            if names is None:
                names = [c.name for c in seg.columns]
                kinds = [c.kind for c in seg.columns]
            if n:
                if first is None:
                    # decode the first chunk now: the views die on the next next()
                    cols = {c.name: c for c in seg.columns}
                    off = cast(cols["name"].data[: 2 * 4], "i")
                    first = {
                        "id": cast(cols["id"].data, "i")[0],
                        "big": cast(cols["big"].data, "q")[0],
                        "f": cast(cols["f"].data, "d")[0],
                        "b": (cols["b"].data[0], cols["b"].data[1]),
                        "name": bytes(cols["name"].bytes[off[0] : off[1]]),
                    }
                total += n
            if n == 0:
                break
        self.ring.close()

        self.assertEqual(total, 1000)
        self.assertEqual(names, ["id", "big", "f", "b", "name"])
        self.assertEqual(
            kinds,
            [fp.Kind.INT32, fp.Kind.INT64, fp.Kind.FLOAT8, fp.Kind.BOOL, fp.Kind.TEXT],
        )
        self.assertEqual(first, {"id": 1, "big": 2, "f": 1.5, "b": (0, 1), "name": b"row1"})


class DbapiTest(unittest.TestCase):
    def setUp(self):
        from fastpostgres import dbapi

        self.dbapi = dbapi
        self.conn = dbapi.connect(CONNINFO)

    def tearDown(self):
        self.conn.close()

    def test_parameter_inlining(self):
        cur = self.conn.cursor()
        cur.execute("SELECT %s::int + %s::int AS total", (2, 3))
        self.assertEqual(cur.fetchone(), (5,))
        self.assertEqual(cur.description[0][0], "total")
        self.assertEqual(cur.rowcount, 1)

    def test_fetchall_and_iter(self):
        cur = self.conn.cursor()
        cur.execute("SELECT g::int FROM generate_series(1, 3) g")
        self.assertEqual(cur.fetchall(), [(1,), (2,), (3,)])
        cur.execute("SELECT g::int FROM generate_series(1, 2) g")
        self.assertEqual(list(cur), [(1,), (2,)])

    def test_literal_quoting_is_safe(self):
        cur = self.conn.cursor()
        cur.execute("SELECT %s::text AS s", ("it's; DROP TABLE x --",))
        self.assertEqual(cur.fetchone(), ("it's; DROP TABLE x --",))

    def test_executemany(self):
        cur = self.conn.cursor()
        cur.execute("DROP TABLE IF EXISTS fp_dbapi_t; CREATE TABLE fp_dbapi_t (n int)")
        cur.executemany("INSERT INTO fp_dbapi_t VALUES (%s)", [(1,), (2,), (3,)])
        self.assertEqual(cur.rowcount, 3)
        cur.execute("SELECT count(*)::int FROM fp_dbapi_t")
        self.assertEqual(cur.fetchone(), (3,))
        cur.execute("DROP TABLE fp_dbapi_t")


    def test_shm_select_path_batches(self):
        # 120000 rows crosses the 100000-row shm batch boundary
        cur = self.conn.cursor()
        cur.execute("SELECT g::int AS id, ('x' || g)::text AS s FROM generate_series(1, 120000) g")
        rows = cur.fetchall()
        self.assertEqual(len(rows), 120000)
        self.assertEqual(rows[0], (1, "x1"))
        self.assertEqual(rows[-1], (120000, "x120000"))
        self.assertGreater(self.conn.shm_reads, 0)

    def test_shm_types(self):
        cur = self.conn.cursor()
        cur.execute(
            "SELECT NULL::int4 AS n, 1.5::float8 AS f, (2 % 2 = 0)::bool AS b, "
            "'2026-10-30 12:00:00+00'::timestamptz AS ts"
        )
        n, f, b, ts = cur.fetchone()
        self.assertIsNone(n)
        self.assertAlmostEqual(f, 1.5)
        self.assertIs(b, True)
        self.assertEqual((ts.year, ts.month, ts.day, ts.hour), (2026, 10, 30, 12))
        self.assertGreater(self.conn.shm_reads, 0)

    def test_dml_stays_on_libpq(self):
        cur = self.conn.cursor()
        cur.execute("DROP TABLE IF EXISTS fp_shm_t; CREATE TABLE fp_shm_t (n int)")
        before = self.conn.shm_reads
        cur.execute("INSERT INTO fp_shm_t VALUES (%s)", (1,))
        self.assertEqual(cur.rowcount, 1)
        self.assertEqual(self.conn.shm_reads, before)
        cur.execute("DROP TABLE fp_shm_t")


if __name__ == "__main__":
    unittest.main()
