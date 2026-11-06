"""The storage-level columnar path: bulk-load a SELECT into a sealed memfd,
map it read-only, and scan the columns where they lie.

The numbers are checked against the server's own aggregates; the bytes are
checked against the column views, and the text is read out of the mapping with
the offset table the format already carries.
"""
import os
import struct
import unittest

import fastpostgres as fp
from fastpostgres import columnar, dbapi

CONNINFO = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")
TABLE = "fp_col_t"
QUERY = f"SELECT id, sku, price, stock, active FROM {TABLE}"


def setUpModule():
    conn = fp.Connection(CONNINFO)
    try:
        conn.exec(f"DROP TABLE IF EXISTS {TABLE}")
        conn.exec(
            f"CREATE TABLE {TABLE} AS "
            f"SELECT g::int4 AS id, "
            f"       (CASE WHEN g % 11 = 0 THEN NULL ELSE 'SKU-' || g END)::text AS sku, "
            f"       (g * 0.5)::float8 AS price, "
            f"       (CASE WHEN g % 7 = 0 THEN NULL ELSE (g % 500)::int4 END) AS stock, "
            f"       (g % 2 = 0) AS active "
            f"FROM generate_series(1, 5000) g"
        )
        conn.exec(f"ANALYZE {TABLE}")
    finally:
        conn.close()


def tearDownModule():
    conn = fp.Connection(CONNINFO)
    try:
        conn.exec(f"DROP TABLE IF EXISTS {TABLE}")
    finally:
        conn.close()


def server_row(sql):
    conn = fp.Connection(CONNINFO)
    try:
        return conn.exec(sql).rows[0]
    finally:
        conn.close()


class ColumnarLoadTest(unittest.TestCase):
    def setUp(self):
        self.conn = dbapi.connect(CONNINFO)

    def tearDown(self):
        self.conn.close()

    def test_load_maps_the_columns_and_metadata(self):
        table = self.conn.columnar_load(QUERY)
        self.assertEqual(table.row_count, 5000)
        self.assertEqual(table.scan_all()[0], 5000)
        self.assertEqual(table.query, QUERY)

        self.assertEqual([c.name for c in table.columns], ["id", "sku", "price", "stock", "active"])
        self.assertEqual(
            [c.kind for c in table.columns],
            [fp.Kind.INT32, fp.Kind.TEXT, fp.Kind.FLOAT8, fp.Kind.INT32, fp.Kind.BOOL],
        )
        self.assertEqual([c.nrows for c in table.columns], [5000] * 5)
        # The views alias the immutable mapping, not a copy.
        self.assertEqual(len(table.column("id").data), 5000 * 4)
        self.assertEqual(len(table.column("active").data), 5000)

    def test_projection_loads_only_the_selected_columns(self):
        table = self.conn.columnar_load(f"SELECT id, price FROM {TABLE}")
        self.assertEqual(table.row_count, 5000)
        self.assertEqual([c.name for c in table.columns], ["id", "price"])

    def test_reduce_matches_the_server(self):
        table = self.conn.columnar_load(QUERY)
        got = table.reduce(
            {
                "price_sum": ("sum", "price"),
                "price_min": ("min", "price"),
                "price_max": ("max", "price"),
                "rows": ("count_all", None),
                "stock_seen": ("count", "stock"),
                "id_max": ("max", "id"),
                "true_count": ("sum", "active"),
            }
        )
        want = server_row(
            f"SELECT sum(price), min(price), max(price), count(*), "
            f"count(stock), max(id), sum(active::int) FROM {TABLE}"
        )
        self.assertAlmostEqual(got["price_sum"], want[0], places=9)
        self.assertEqual(got["price_min"], want[1])
        self.assertEqual(got["price_max"], want[2])
        self.assertEqual(got["rows"], want[3])
        self.assertEqual(got["stock_seen"], want[4])
        self.assertEqual(got["id_max"], want[5])
        self.assertEqual(got["true_count"], want[6])

    def test_full_scan_reads_every_column_byte_in_place(self):
        table = self.conn.columnar_load(QUERY)
        rows, touched, checksum, ncols = table.scan_all()

        expected = 0
        for col in table.columns:
            if col.kind == fp.Kind.TEXT:
                expected += len(col.bytes)
            else:
                expected += len(col.data)
        self.assertEqual(rows, 5000)
        self.assertEqual(ncols, 5)
        self.assertEqual(touched, expected)
        self.assertEqual(table.scan_all()[2], checksum)  # immutable: same bytes, same digest

    def test_text_can_be_read_from_the_mapping(self):
        table = self.conn.columnar_load(QUERY)
        col = table.column("sku")
        offsets = struct.unpack_from("<%di" % (col.nrows + 1), col.data, 0)
        first = bytes(col.bytes[offsets[0] : offsets[1]]).decode()
        want = server_row(f"SELECT sku FROM {TABLE} WHERE id = 1")[0]
        self.assertEqual(first, want)

    def test_non_select_is_refused(self):
        with self.assertRaises(Exception):
            self.conn.columnar_load(f"DELETE FROM {TABLE}")

    def test_scm_rights_handoff_is_peercred_checked(self):
        """The descriptor arrives over SCM_RIGHTS from this session's backend.

        SO_PEERCRED is checked against PQbackendPID, so the test proves both
        that the capability transfer happened (``handoff == "scm"`` and a local
        handle exists) and that the sender is the backend on this control
        channel — not merely a process that found a pid and an fd in /proc.
        """
        backend_pid = self.conn._conn.backend_pid()
        table = self.conn.columnar_load(QUERY)

        self.assertEqual(table.handoff, "scm")
        self.assertIsNotNone(table._handle)
        self.assertEqual(table.peer[0], backend_pid)
        self.assertEqual(table._pid, backend_pid)
        self.assertGreaterEqual(table.handoff_ms, 0.0)
        self.assertEqual(table.scan_all()[0], 5000)
        self.assertEqual(table.reduce({"rows": ("count_all", None)}), {"rows": 5000})

    def test_proc_descriptor_fallback_still_attaches(self):
        """The unchanged two-argument function keeps the original /proc path."""
        result = self.conn._conn.exec(f"SELECT fastpostgres_columnar_load('{QUERY}', 0)")
        parts = result.rows[0][0].split()
        self.assertEqual(parts[0], "FPQCOL1")
        pid, fd, size, rows = (int(x) for x in parts[1:])

        table = columnar.ColumnarTable(pid, fd, size, rows, QUERY)
        self.assertEqual(table.handoff, "proc")
        self.assertIsNone(table._handle)
        self.assertEqual(table.row_count, 5000)
        self.assertEqual(table.scan_all()[0], 5000)
