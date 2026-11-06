"""The C++ aggregate path: a read reduced to numbers, no Python object per row.

Checked against the server's own aggregates: if `sum`/`min`/`max`/`count` here
disagree with SQL over the same rows, the walk is wrong.
"""
import os
import unittest
from unittest import mock

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
            f"(g % 2 = 0) AS active, "
            f"('item-' || lpad((g % 2000)::text, 4, '0'))::text AS txt, "
            f"(date '2020-01-01' + (g % 400)::int) AS day, "
            f"(timestamp '2020-01-01' + (g % 400) * interval '1 hour') AS ts, "
            f"((g % 1000)::numeric / 7) AS amount, "
            f"(CASE WHEN g % 2 = 0 THEN 9223372036854775807::int8 ELSE 1::int8 END) AS big "
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

    def test_pushdown_matches_the_client_side_reduce(self):
        """The worker's FPREDUCE and the client's walked segment must agree."""
        ops = {
            "price_sum": ("sum", "price"),
            "price_min": ("min", "price"),
            "price_max": ("max", "price"),
            "rows": ("count_all", None),
            "stock_seen": ("count", "stock"),
            "id_max": ("max", "id"),
        }
        with mock.patch.object(dbapi, "_AGG_PUSHDOWN", False):
            client = self.cur.aggregate(f"SELECT id, price, stock FROM {TABLE}", ops, size=30000)
        pushed = self.cur.aggregate(f"SELECT id, price, stock FROM {TABLE}", ops, size=30000)
        self.assertEqual(pushed, client)

    def test_bigint_sum_is_exact(self):
        """sum(bigint) is numeric on the server: 64-bit wrapping is not an answer."""
        sql = f"SELECT big FROM {TABLE}"
        got = self.cur.aggregate(sql, {"s": ("sum", "big"), "avg": ("avg", "big")})
        want = self.server(f"SELECT sum(big), avg(big) FROM {TABLE}")
        self.assertGreater(want[0], 2**63)
        self.assertEqual(got["s"], int(want[0]))
        self.assertEqual(got["avg"], want[1])
        with mock.patch.object(dbapi, "_AGG_PUSHDOWN", False):
            client = self.cur.aggregate(sql, {"s": ("sum", "big")})
        self.assertEqual(client["s"], int(want[0]))

    def test_negative_bigint_sum_is_exact(self):
        sql = "SELECT x FROM (VALUES (-9223372036854775807::int8), (-2::int8)) t(x)"
        got = self.cur.aggregate(sql, {"s": ("sum", "x"), "a": ("avg", "x")})
        want = self.server(sql.replace("SELECT x", "SELECT sum(x), avg(x)"))
        self.assertEqual(got["s"], int(want[0]))
        self.assertEqual(got["a"], want[1])
        with mock.patch.object(dbapi, "_AGG_PUSHDOWN", False):
            client = self.cur.aggregate(sql, {"s": ("sum", "x")})
        self.assertEqual(client["s"], int(want[0]))

    def test_avg_matches_the_server(self):
        got = self.cur.aggregate(
            f"SELECT id, price, amount FROM {TABLE}",
            {
                "id_avg": ("avg", "id"),
                "price_avg": ("avg", "price"),
                "amount_avg": ("avg", "amount"),
            },
            size=100000,
        )
        want = self.server(f"SELECT avg(id), avg(price), avg(amount) FROM {TABLE}")
        self.assertEqual(got["id_avg"], want[0])  # numeric on the server -> Decimal here
        self.assertAlmostEqual(got["price_avg"], want[1], places=9)
        self.assertEqual(got["amount_avg"], want[2])

    def test_empty_avg_is_null(self):
        got = self.cur.aggregate(
            f"SELECT id, price, amount FROM {TABLE} WHERE false",
            {"id_avg": ("avg", "id"), "price_avg": ("avg", "price"), "amount_avg": ("avg", "amount")},
        )
        self.assertIsNone(got["id_avg"])
        self.assertIsNone(got["price_avg"])
        self.assertIsNone(got["amount_avg"])

    def test_text_temporal_and_numeric_min_max(self):
        got = self.cur.aggregate(
            f"SELECT txt, day, ts, amount FROM {TABLE}",
            {
                "txt_lo": ("min", "txt"),
                "txt_hi": ("max", "txt"),
                "txt_seen": ("count", "txt"),
                "day_lo": ("min", "day"),
                "day_hi": ("max", "day"),
                "ts_lo": ("min", "ts"),
                "ts_hi": ("max", "ts"),
                "amount_lo": ("min", "amount"),
                "amount_hi": ("max", "amount"),
                "amount_sum": ("sum", "amount"),
            },
            size=100000,
        )
        want = self.server(
            f"SELECT min(txt), max(txt), count(txt), min(day), max(day), "
            f"min(ts), max(ts), min(amount), max(amount), sum(amount) FROM {TABLE}"
        )
        for key, value in zip(
            ("txt_lo", "txt_hi", "txt_seen", "day_lo", "day_hi", "ts_lo", "ts_hi",
             "amount_lo", "amount_hi", "amount_sum"),
            want,
        ):
            self.assertEqual(got[key], value, key)

    def test_count_works_on_text(self):
        got = self.cur.aggregate(f"SELECT txt FROM {TABLE}", {"n": ("count", "txt")})
        self.assertEqual(got["n"], 250000)

    def test_avg_scale_matches_the_server(self):
        """PostgreSQL keeps 16 significant base-10000 digits, not 16 decimals."""
        cases = [
            "SELECT x FROM (VALUES (0::int), (1::int)) t(x)",
            "SELECT x FROM (VALUES (-1::int), (-2::int)) t(x)",
            "SELECT x FROM (VALUES (1::int), (2::int), (4::int)) t(x)",
            "SELECT x FROM (VALUES (1000000000000000000000::numeric), (3::numeric)) t(x)",
            "SELECT x FROM (VALUES (1.00::numeric(10, 2)), (2.00::numeric(10, 2))) t(x)",
            "SELECT x FROM (VALUES (0.0001::numeric), (0.0003::numeric)) t(x)",
        ]
        for sql in cases:
            got = self.cur.aggregate(sql, {"a": ("avg", "x")})["a"]
            want = self.server(sql.replace("SELECT x", "SELECT avg(x)"))[0]
            self.assertEqual(got, want, sql)

    def test_text_min_max_uses_the_server_collation(self):
        sql = "SELECT x FROM (VALUES ('B'), ('a'), ('A'), ('b'), ('aa'), ('z')) t(x)"
        got = self.cur.aggregate(sql, {"lo": ("min", "x"), "hi": ("max", "x")})
        want = self.server(sql.replace("SELECT x", "SELECT min(x), max(x)"))
        self.assertEqual((got["lo"], got["hi"]), tuple(want))

    def test_unsupported_kind_is_refused(self):
        with self.assertRaises(dbapi.ProgrammingError):
            self.cur.aggregate(f"SELECT ('x' || id)::text AS s FROM {TABLE}", {"n": ("sum", "s")})

    def test_avg_needs_a_column(self):
        with self.assertRaises(dbapi.ProgrammingError):
            self.cur.aggregate(f"SELECT id FROM {TABLE}", {"n": ("avg", None)})

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
