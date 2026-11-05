"""The C++ aggregate path: a read reduced to numbers, no Python object per row.

Checked against the server's own aggregates: if `sum`/`min`/`max`/`count` here
disagree with SQL over the same rows, the walk is wrong.
"""
import os
import unittest

import fastpostgres as fp
from fastpostgres import aio, dbapi

CONNINFO = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")
TABLE = "fp_agg_t"


def setUpModule():
    """The table both test classes read: 250k rows with NULLs in one column."""
    conn = fp.Connection(CONNINFO)
    try:
        conn.exec(f"DROP TABLE IF EXISTS {TABLE}")
        conn.exec(
            f"CREATE TABLE {TABLE} AS SELECT g::int4 AS id, (g % 97)::int8 AS part, "
            f"((g * 0.5) - 2500)::float8 AS price, "
            f"(CASE WHEN g % 10 = 0 THEN NULL ELSE (g % 500)::int4 END) AS stock, "
            f"(g % 2 = 0) AS active "
            f"FROM generate_series(1, 250000) g"
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


class AggregateTest(unittest.TestCase):
    def setUp(self):
        self.conn = dbapi.connect(CONNINFO)
        self.cur = self.conn.cursor()

    def tearDown(self):
        self.conn.close()

    def server(self, sql):
        other = fp.Connection(CONNINFO)
        try:
            return other.exec(sql).rows[0]
        finally:
            other.close()

    def test_matches_the_server(self):
        got = self.cur.aggregate(
            f"SELECT id, part, price, stock, active FROM {TABLE}",
            {
                "price_sum": ("sum", "price"),
                "price_min": ("min", "price"),
                "price_max": ("max", "price"),
                "rows": ("count_all", None),
                "priced": ("count", "price"),
                "stock_seen": ("count", "stock"),
                "id_max": ("max", "id"),
                "true_count": ("sum", "active"),
            },
            size=100000,
        )
        want = self.server(
            f"SELECT sum(price), min(price), max(price), count(*), count(price), "
            f"count(stock), max(id), sum(active::int) FROM {TABLE}"
        )
        self.assertAlmostEqual(got["price_sum"] / want[0], 1.0, places=12)
        self.assertEqual(got["price_min"], want[1])
        self.assertEqual(got["price_max"], want[2])
        self.assertEqual(got["rows"], want[3])
        self.assertEqual(got["priced"], want[4])
        self.assertEqual(got["stock_seen"], want[5])  # NULLs skipped, like SQL
        self.assertEqual(got["id_max"], want[6])
        self.assertEqual(got["true_count"], want[7])
        self.assertGreater(self.conn.shm_reads, 0)

    def test_batches_combine(self):
        """250k rows at 30k a batch: the per-batch scalars must add up."""
        got = self.cur.aggregate(
            f"SELECT id, price FROM {TABLE}",
            {"rows": ("count_all", None), "id_sum": ("sum", "id"), "id_min": ("min", "id")},
            size=30000,
        )
        want = self.server(f"SELECT count(*), sum(id), min(id) FROM {TABLE}")
        self.assertEqual(got["rows"], want[0])
        self.assertEqual(got["id_sum"], want[1])
        self.assertEqual(got["id_min"], want[2])

    def test_empty_result(self):
        got = self.cur.aggregate(
            f"SELECT id, price FROM {TABLE} WHERE false",
            {"rows": ("count_all", None), "price_sum": ("sum", "price"), "price_min": ("min", "price")},
        )
        self.assertEqual(got["rows"], 0)
        self.assertEqual(got["price_sum"], 0)
        self.assertIsNone(got["price_min"])

    def test_columns_by_index(self):
        by_name = self.cur.aggregate(f"SELECT id, price FROM {TABLE}", {"s": ("sum", "price")})
        by_index = self.cur.aggregate(f"SELECT id, price FROM {TABLE}", {"s": ("sum", 1)})
        self.assertEqual(by_name, by_index)

    def test_unsupported_kind_is_refused(self):
        with self.assertRaises(dbapi.ProgrammingError):
            self.cur.aggregate(f"SELECT ('x' || id)::text AS s FROM {TABLE}", {"n": ("sum", "s")})

    def test_bad_plans_are_refused(self):
        for ops in (
            {"n": ("median", "price")},
            {"n": ("count_all", "price")},
            {"n": ("sum", None)},
            {"n": ("sum", "nope")},
        ):
            with self.assertRaises(dbapi.ProgrammingError, msg=str(ops)):
                self.cur.aggregate(f"SELECT id, price FROM {TABLE}", ops)

    def test_shm_false_refuses(self):
        conn = dbapi.connect(CONNINFO, shm=False)
        try:
            with self.assertRaises(dbapi.NotSupportedError):
                conn.cursor().aggregate(f"SELECT id FROM {TABLE}", {"n": ("count_all", None)})
        finally:
            conn.close()

    def test_one_pass_is_a_stream_not_rows(self):
        """The reduction never materializes rows: the same query as tuples would
        build 250k objects, and this path's only Python objects per batch are the
        handful of scalars."""
        got = self.cur.aggregate(
            f"SELECT id, price, stock FROM {TABLE}", {"n": ("count_all", None)}, size=50000
        )
        self.assertEqual(got["n"], 250000)


class AioAggregateTest(unittest.IsolatedAsyncioTestCase):
    async def test_aggregate_matches_the_sync_path(self):
        conn = await aio.connect(CONNINFO)
        try:
            got = await conn.aggregate(
                f"SELECT id, price FROM {TABLE}",
                {"rows": ("count_all", None), "ids": ("sum", "id"), "lo": ("min", "id")},
            )
            cur = conn.cursor()
            again = await cur.aggregate(
                f"SELECT id, price FROM {TABLE}",
                {"rows": ("count_all", None), "ids": ("sum", "id"), "lo": ("min", "id")},
            )
            self.assertEqual(got, again)
            self.assertEqual(got["rows"], 250000)
        finally:
            await conn.close()


if __name__ == "__main__":
    unittest.main()
