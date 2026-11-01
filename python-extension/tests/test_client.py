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

    def test_binary_kinds_on_the_wire(self):
        """The wire form itself: date/time/timestamp/uuid/bytea arrive binary,
        at their fixed widths, instead of as text to be parsed."""
        self.ring.open(
            "fp_py_seg",
            "SELECT '2026-10-30'::date AS d, '01:02:03.5'::time AS t, "
            "'2026-10-30 01:02:03.5'::timestamp AS ts, "
            "'2026-10-30 01:02:03.5+00'::timestamptz AS tstz, "
            "'0f8fad5b-d9cb-469f-a165-70867728950e'::uuid AS u, "
            "'\\x00010203ff'::bytea AS by",
        )
        n = self.ring.next(10)
        seg = fp.read("fp_py_seg")

        self.assertEqual(n, 1)
        self.assertEqual(
            [c.kind for c in seg.columns],
            [
                fp.Kind.DATE,
                fp.Kind.TIME,
                fp.Kind.TIMESTAMP,
                fp.Kind.TIMESTAMPTZ,
                fp.Kind.UUID,
                fp.Kind.BYTEA,
            ],
        )
        widths = {c.name: len(c.data) for c in seg.columns}
        self.assertEqual(
            widths,
            {"d": 4, "t": 8, "ts": 8, "tstz": 8, "u": 16, "by": 2 * 4 + 5},  # bytea: offsets + '\x00010203ff'
        )
        self.ring.close()


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

    def test_shm_binary_types_match_libpq(self):
        """The same query over /dev/shm and over libpq must agree value for
        value: the binary kinds are a re-encoding, not a reinterpretation."""
        sql = (
            "SELECT '2026-10-30'::date AS d, '12:34:56.789012'::time AS t, "
            "'2026-10-30 12:34:56.789012'::timestamp AS ts, "
            "'2026-10-30 12:34:56.789012+02'::timestamptz AS tstz, "
            "'0f8fad5b-d9cb-469f-a165-70867728950e'::uuid AS u, "
            "'\\x00010203ff'::bytea AS by, NULL::date AS null_d"
        )
        cur = self.conn.cursor()
        cur.execute(sql)
        row = cur.fetchone()
        self.assertGreater(self.conn.shm_reads, 0, "the shm path should have served this read")

        plain = fp.Connection(CONNINFO).exec(sql).rows[0]
        self.assertEqual(row, plain)

        d, t, ts, tstz, u, by, null_d = row
        self.assertEqual((d.year, d.month, d.day), (2026, 10, 30))
        self.assertEqual((t.hour, t.minute, t.second, t.microsecond), (12, 34, 56, 789012))
        self.assertEqual((ts.year, ts.month, ts.day, ts.hour, ts.microsecond), (2026, 10, 30, 12, 789012))
        self.assertEqual((tstz.year, tstz.month, tstz.day, tstz.hour), (2026, 10, 30, 10))  # +02 -> UTC
        self.assertEqual(tstz.utcoffset().total_seconds(), 0)
        self.assertEqual(str(u), "0f8fad5b-d9cb-469f-a165-70867728950e")
        self.assertEqual(by, b"\x00\x01\x02\x03\xff")
        self.assertIsNone(null_d)

    def test_shm_row_path_detoasts(self):
        """The row frame carries PostgreSQL's own tuples, so the worker has to
        resolve what it cannot send in place: a compressed value inline in the
        tuple and one stored out of line both arrive as their real value."""
        sql = (
            "SELECT repeat('y', 20000) AS compressed, repeat(md5(g::text), 700) AS external, "
            "'2026-10-30'::date AS d FROM generate_series(1, 2) g"
        )
        cur = self.conn.cursor()
        cur.execute(sql)
        rows = cur.fetchall()
        self.assertGreater(self.conn.shm_reads, 0, "the shm path should have served this read")

        plain = fp.Connection(CONNINFO).exec(sql).rows
        self.assertEqual(rows, plain)
        self.assertEqual(len(rows[0][0]), 20000)
        self.assertEqual(len(rows[0][1]), 32 * 700)
        self.assertEqual((rows[0][2].year, rows[0][2].month, rows[0][2].day), (2026, 10, 30))

    def test_shm_falls_back_for_types_it_cannot_walk(self):
        """A type whose storage is not the wire bytes themselves (numeric) makes
        the batch take the columnar frame; the values and OIDs are unchanged."""
        sql = "SELECT 1234.5::numeric AS num, 'x'::text AS t, 7::int4 AS n, NULL::int8 AS missing"
        cur = self.conn.cursor()
        cur.execute(sql)
        row = cur.fetchone()
        self.assertGreater(self.conn.shm_reads, 0, "the shm path should have served this read")

        plain = fp.Connection(CONNINFO).exec(sql).rows[0]
        self.assertEqual(row, plain)
        self.assertEqual(str(row[0]), "1234.5")
        self.assertEqual(row[1:], ("x", 7, None))
        self.assertEqual(cur.description[0][1], 1700)

    def test_core_read_without_core(self):
        """The CORE stand-in compiles a statement with SQLAlchemy's own compiler
        and hands back the driver's tuples: same SQL, same values, no Row."""
        from sqlalchemy import Column, Integer, MetaData, String, Table, bindparam, select

        cur = self.conn.cursor()
        cur.execute("DROP TABLE IF EXISTS fp_core_t; CREATE TABLE fp_core_t (id int, s text)")
        cur.execute("INSERT INTO fp_core_t VALUES (1, 'a'), (2, 'b'), (3, 'c')")

        md = MetaData()
        t = Table("fp_core_t", md, Column("id", Integer), Column("s", String))
        cols = select(t.c.id, t.c.s).order_by(t.c.id)

        rows = self.conn.fetchall(cols)
        self.assertEqual(rows, [(1, "a"), (2, "b"), (3, "c")])
        self.assertGreater(self.conn.shm_reads, 0, "the shm path should have served this read")

        self.assertEqual(self.conn.first(cols), (1, "a"))
        self.assertIsNone(self.conn.first(select(t.c.s).where(t.c.id == 99)))

        # binds are compiled into the statement, which is the only form the ring takes
        bound = select(t.c.s).where(t.c.id == bindparam("i"))
        self.assertIn("id = 2", self.conn.compile(bound, i=2))
        self.assertEqual(self.conn.fetchall(bound, i=2), [("b",)])

        cur.execute("DROP TABLE fp_core_t")

    def test_core_module_mirrors_the_core_read_shape(self):
        """fastpostgres.core takes the same calls CORE does — engine.connect(),
        execute(statement), .all() — with the driver's tuples underneath."""
        from sqlalchemy import Column, Integer, MetaData, String, Table, select

        from fastpostgres.core import create_engine

        cur = self.conn.cursor()
        cur.execute("DROP TABLE IF EXISTS fp_lean_t; CREATE TABLE fp_lean_t (id int, s text)")
        cur.execute("INSERT INTO fp_lean_t VALUES (1, 'a'), (2, 'b')")

        md = MetaData()
        t = Table("fp_lean_t", md, Column("id", Integer), Column("s", String))
        engine = create_engine(CONNINFO)
        try:
            with engine.connect() as conn:
                res = conn.execute(select(t.c.id, t.c.s).order_by(t.c.id))
                self.assertEqual(res.all(), [(1, "a"), (2, "b")])
                self.assertEqual(res.keys(), ("id", "s"))
                self.assertEqual(res.first(), (1, "a"))
                self.assertEqual(len(res), 2)
                self.assertEqual(list(res), [(1, "a"), (2, "b")])

                # SQL text goes through as it is; rowcount survives for DML
                self.assertEqual(conn.execute("SELECT count(*)::int FROM fp_lean_t").all(), [(2,)])
                self.assertEqual(conn.execute("UPDATE fp_lean_t SET s = s").rowcount, 2)
        finally:
            engine.dispose()

        cur.execute("DROP TABLE fp_lean_t")

    def test_forked_worker_gets_its_own_ring_name(self):
        """Module state carries across fork; the ring stem must not. gunicorn and
        multiprocessing import first and fork after, and two processes on one ring
        is two background workers on one data segment."""
        import os

        from fastpostgres import dbapi

        parent = dbapi._prefix()
        self.assertIn(str(os.getpid()), parent)

        r, w = os.pipe()
        pid = os.fork()
        if pid == 0:
            os.write(w, dbapi._prefix().encode())
            os._exit(0)
        os.close(w)
        child = os.read(r, 64).decode()
        os.waitpid(pid, 0)

        self.assertNotEqual(parent, child)
        self.assertIn(str(pid), child)

    def test_typed_views_are_zero_copy(self):
        """Consuming a chunk without materialising it: numpy wraps the segment in
        place, text is sliced straight out of the offset table, and a value is
        only built when it is asked for."""
        try:
            import numpy as np
        except ImportError:
            np = None

        cur = self.conn.cursor()
        cols = None
        for batch in cur.stream(
            "SELECT g::int AS id, (g * 1.5)::float8 AS f, (g % 3 = 0)::bool AS b, "
            "('row' || g)::text AS s FROM generate_series(1, 1000) g",
            size=1000,
        ):
            if not batch.row_count:
                continue
            cols = {c.name: c for c in batch.columns}
            n = batch.row_count

            self.assertEqual(cols["id"].nrows, n)
            self.assertFalse(cols["id"].has_nulls)

            ids = cols["id"].data.cast("i")  # a typed view, no objects per value
            self.assertEqual((ids[0], ids[-1]), (1, n))

            if np is not None:
                arr = np.asarray(cols["id"])
                self.assertEqual(arr.dtype, np.dtype("<i4"))
                self.assertEqual((int(arr[0]), int(arr[-1])), (1, n))
                self.assertEqual(int(arr.sum()), n * (n + 1) // 2)
                self.assertEqual(float(np.asarray(cols["f"])[0]), 1.5)

            off = cols["s"].offsets.cast("i")
            self.assertEqual(len(off), n + 1)
            self.assertEqual(bytes(cols["s"].bytes[off[0] : off[1]]), b"row1")
            self.assertEqual(cols["s"].value_at(0), "row1")
            break

        self.assertIsNotNone(cols, "stream() produced no batch")

    def test_typed_view_refuses_a_column_with_nulls(self):
        cur = self.conn.cursor()
        for batch in cur.stream(
            "SELECT g::int AS id, CASE WHEN g = 2 THEN NULL ELSE g END AS maybe "
            "FROM generate_series(1, 4) g",
            size=10,
        ):
            if not batch.row_count:
                continue
            cols = {c.name: c for c in batch.columns}

            self.assertFalse(cols["id"].has_nulls)
            self.assertTrue(cols["maybe"].has_nulls)
            with self.assertRaises(ValueError):  # a view would quietly read zeros
                cols["maybe"].__array_interface__
            self.assertEqual(cols["maybe"].value_at(0), 1)
            self.assertIsNone(cols["maybe"].value_at(1))
            break

    def test_dml_stays_on_libpq(self):
        cur = self.conn.cursor()
        cur.execute("DROP TABLE IF EXISTS fp_shm_t; CREATE TABLE fp_shm_t (n int)")
        before = self.conn.shm_reads
        cur.execute("INSERT INTO fp_shm_t VALUES (%s)", (1,))
        self.assertEqual(cur.rowcount, 1)
        self.assertEqual(self.conn.shm_reads, before)
        cur.execute("DROP TABLE fp_shm_t")


    def test_shm_pool_reuses_rings_across_connections(self):
        other = self.dbapi.connect(CONNINFO)
        try:
            self.conn.cursor().execute("SELECT 1::int AS n")  # warm: creates/reuses a ring
            created = self.dbapi.pool_created()
            self.assertGreater(created, 0)

            cur = other.cursor()
            cur.execute("SELECT g::int AS n FROM generate_series(1, 3) g")
            self.assertEqual(cur.fetchall(), [(1,), (2,), (3,)])

            self.assertEqual(self.dbapi.pool_created(), created)  # reused, none new
            self.assertLessEqual(created, self.dbapi.pool_size())
        finally:
            other.close()


    def test_stream_columnar_zero_copy(self):
        cur = self.conn.cursor()
        names = None
        seen = []
        total = 0
        for batch in cur.stream(
            "SELECT g::int AS id, ('x' || g)::text AS s FROM generate_series(1, 250000) g",
            size=100000,
        ):
            self.assertEqual(len(batch.columns), 2)
            if names is None:
                names = [c.name for c in batch.columns]
            ids = cast(batch.columns[0].data, "i")
            off = cast(batch.columns[1].data[: 2 * 4], "i")
            seen.append((ids[0], bytes(batch.columns[1].bytes[off[0] : off[1]])))
            total += batch.row_count

        self.assertEqual(names, ["id", "s"])
        self.assertEqual(total, 250000)
        self.assertEqual(seen[0], (1, b"x1"))
        self.assertEqual(seen[-1], (200001, b"x200001"))

    def test_stream_needs_the_extension(self):
        conn = self.dbapi.connect(CONNINFO, shm=False)
        try:
            with self.assertRaises(self.dbapi.NotSupportedError):
                list(conn.cursor().stream("SELECT 1"))
        finally:
            conn.close()


if __name__ == "__main__":
    unittest.main()
