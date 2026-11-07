"""Per-query parallel aggregate: the ctid-range fan-out must equal the serial path.

`Cursor.aggregate(..., parallel=N)` splits a simple single-table SELECT into N
ctid page ranges, drives each range on its own ring/worker on native threads,
and combines the per-partition scalars exactly. These tests prove the answer is
identical to the single-ring path and that more than one ring was actually used.
"""
import os
import unittest
from unittest import mock

import fastpostgres as fp
from fastpostgres import _client, dbapi

CONNINFO = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")
TABLE = "fp_par_t"


def setUpModule():
    conn = fp.Connection(CONNINFO)
    try:
        conn.exec(f"DROP TABLE IF EXISTS {TABLE}")
        conn.exec(
            f"CREATE TABLE {TABLE} AS SELECT g::int4 AS id, (g % 100)::int4 AS grp, "
            f"((g * 0.5) - 1000)::float8 AS price "
            f"FROM generate_series(1, 200000) g"
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


OPS = {
    "rows": ("count_all", None),
    "total": ("sum", "id"),
    "low": ("min", "id"),
    "high": ("max", "id"),
    "avg_price": ("avg", "price"),
}


class ParallelAggregateTest(unittest.TestCase):
    def setUp(self):
        if not hasattr(_client, "aggregate_frames"):
            self.skipTest("this client has no per-query fan-out")
        if dbapi.pool_size() < 2:
            self.skipTest("the ring pool cannot provide two partitions")
        self.conn = dbapi.connect(CONNINFO)
        self.cur = self.conn.cursor()

    def tearDown(self):
        self.conn.close()

    def _spy(self):
        calls = []
        real = _client.aggregate_frames

        def spy(*args, **kwargs):
            calls.append(len(args[0]))
            return real(*args, **kwargs)

        return calls, mock.patch.object(_client, "aggregate_frames", side_effect=spy)

    def test_parallel_matches_serial(self):
        sql = f"SELECT id, grp, price FROM {TABLE}"
        serial = self.cur.aggregate(sql, OPS)

        calls, patched = self._spy()
        with patched:
            parallel = self.cur.aggregate(sql, OPS, parallel=4)

        self.assertEqual(serial, parallel)
        self.assertTrue(calls and calls[0] >= 2, f"fan-out did not run: {calls}")

    def test_parallel_where_matches_serial(self):
        sql = f"SELECT id, grp, price FROM {TABLE} WHERE grp < 50"
        serial = self.cur.aggregate(sql, OPS)

        calls, patched = self._spy()
        with patched:
            parallel = self.cur.aggregate(sql, OPS, parallel=4)

        self.assertEqual(serial, parallel)
        self.assertTrue(calls and calls[0] >= 2, f"fan-out did not run: {calls}")

    def test_fetchall_does_not_concat_an_aggregate(self):
        # A row-concatenating fan-out would return one partial count per
        # partition; an aggregate must stay on the single-ring path and come
        # back as the one total. (Before the concat gate: PARALLEL=4 returned
        # _RANGE_MULT * 4 partial rows.)
        sql = f"SELECT count(*) FROM {TABLE}"
        with mock.patch.dict(os.environ, {dbapi._PARALLEL_ENV: "4"}):
            self.cur.execute(sql)
            rows = self.cur.fetchall()
        self.assertEqual(len(rows), 1, "the aggregate was split and concatenated")
        self.assertEqual(rows[0][0], 200000)

    def test_non_splittable_query_stays_serial(self):
        # GROUP BY cannot be split by ctid; the fan-out must not be attempted.
        sql = f"SELECT grp FROM {TABLE} GROUP BY grp"
        with mock.patch.object(
            _client, "aggregate_frames", side_effect=AssertionError("must not fan out")
        ):
            got = self.cur.aggregate(sql, {"rows": ("count_all", None)}, parallel=4)
        self.assertEqual(got["rows"], 100)


if __name__ == "__main__":
    unittest.main()
