"""The aggregate partitioner: which SELECTs split, and the SQL each slice runs.

No server and no native module: `partition.py` is pure stdlib, so it is loaded
straight from its path. What is asserted is the shape decision (`table_of`) and
the exact text `partition_sql` produces, because that text is what each ring
runs — a wrong predicate would silently change an aggregate's answer rather than
fail.

    python3 python/tests/test_partition_query.py
"""
import importlib.util
import os
import unittest

_MODULE = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "src",
    "fastpostgres",
    "partition.py",
)
_spec = importlib.util.spec_from_file_location("fastpostgres_partition", _MODULE)
partition = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(partition)


def where(parts):
    """The page-range predicates `parts` (a partition_sql result) appends."""
    return [sql[sql.index("ctid >= ") :] for sql in parts]


class TableOfTest(unittest.TestCase):
    """The shape decision: a single base relation, or None."""

    def test_simple_select(self):
        self.assertEqual(partition.table_of("SELECT * FROM t"), "t")
        self.assertEqual(partition.table_of("select * from t"), "t")
        self.assertEqual(partition.table_of("  SELECT 1 FROM t;  "), "t")

    def test_select_list_is_anything(self):
        # Columns, functions, casts and aliases in the select list do not change
        # which rows the scan reads.
        self.assertEqual(
            partition.table_of("SELECT count(*), max(t.a) AS m, t.b::int FROM t"), "t"
        )

    def test_with_where(self):
        self.assertEqual(partition.table_of("SELECT * FROM t WHERE a = 1"), "t")
        self.assertEqual(
            partition.table_of("SELECT * FROM t WHERE a = 1 AND (b = 2 OR c = 3)"), "t"
        )

    def test_where_may_contain_a_subquery(self):
        # The inner SELECT's FROM and LIMIT sit inside parentheses: not top level.
        self.assertEqual(
            partition.table_of("SELECT * FROM t WHERE a IN (SELECT a FROM u LIMIT 3)"), "t"
        )

    def test_schema_qualified_and_quoted(self):
        self.assertEqual(partition.table_of("SELECT * FROM public.t"), "public.t")
        self.assertEqual(partition.table_of('SELECT * FROM "My Table"'), '"My Table"')
        self.assertEqual(partition.table_of('SELECT * FROM "public"."t"'), '"public"."t"')
        self.assertEqual(partition.table_of("SELECT * FROM db.public.t"), "db.public.t")

    def test_alias(self):
        self.assertEqual(partition.table_of("SELECT * FROM t AS x"), "t")
        self.assertEqual(partition.table_of("SELECT * FROM t x WHERE x.a > 1"), "t")
        self.assertEqual(partition.table_of('SELECT * FROM t AS "x y"'), "t")
        self.assertEqual(partition.table_of('SELECT * FROM "t" "x"'), '"t"')

    def test_keywords_inside_literals_and_comments_are_data(self):
        sqls = [
            "SELECT * FROM t WHERE label = 'from t group by (join)'",
            "SELECT * FROM t WHERE s = 'a''b'",
            "SELECT * FROM t WHERE s = 'a;b'",
            "SELECT * FROM t -- from u join v\nWHERE a = 1",
            "SELECT * FROM t /* join u */ WHERE a = 1",
            "SELECT * FROM t /* a /* nested */ join */ WHERE a = 1",
            "SELECT * FROM t WHERE s = $tag$ from join $tag$",
            "SELECT * FROM/*x*/t",
        ]
        for sql in sqls:
            with self.subTest(sql=sql):
                self.assertEqual(partition.table_of(sql), "t")

    def test_rejects_more_than_one_relation(self):
        sqls = [
            "SELECT * FROM t JOIN u ON t.id = u.id",
            "SELECT * FROM t LEFT JOIN u ON t.id = u.id",
            "SELECT * FROM t, u",
            "SELECT * FROM t a, u b",
            "SELECT * FROM t UNION SELECT * FROM u",
            "SELECT * FROM t INTERSECT SELECT * FROM u",
            "SELECT * FROM t EXCEPT SELECT * FROM u",
        ]
        for sql in sqls:
            with self.subTest(sql=sql):
                self.assertIsNone(partition.table_of(sql))

    def test_rejects_result_shaping_clauses(self):
        sqls = [
            "SELECT * FROM t GROUP BY a",
            "SELECT * FROM t HAVING count(*) > 1",
            "SELECT * FROM t ORDER BY a",
            "SELECT * FROM t LIMIT 1",
            "SELECT * FROM t OFFSET 1",
            "SELECT * FROM t FETCH FIRST 3 ROWS ONLY",
            "SELECT * FROM t FETCH NEXT 3 ROWS ONLY",
            "SELECT DISTINCT a FROM t",
            "SELECT count(*) OVER () FROM t",
            "SELECT * FROM t WINDOW w AS ()",
            "SELECT * FROM t FOR UPDATE",
            "SELECT * FROM t FOR SHARE",
            "WITH x AS (SELECT 1) SELECT * FROM x",
            "SELECT * FROM t; SELECT * FROM u",
        ]
        for sql in sqls:
            with self.subTest(sql=sql):
                self.assertIsNone(partition.table_of(sql))

    def test_rejects_from_items_that_are_not_a_base_relation(self):
        sqls = [
            "SELECT * FROM (SELECT * FROM t) s",
            "SELECT * FROM LATERAL (SELECT 1) s",
            "SELECT * FROM generate_series(1, 10)",
            "SELECT * FROM unnest(ARRAY[1, 2]) AS u",
            "SELECT * FROM ONLY t",
            "SELECT * FROM t TABLESAMPLE SYSTEM (1)",
            "SELECT * FROM t AS x(a, b)",
        ]
        for sql in sqls:
            with self.subTest(sql=sql):
                self.assertIsNone(partition.table_of(sql))

    def test_rejects_what_it_cannot_delimit(self):
        sqls = [
            "",
            "   ",
            None,
            "SELECT 1",  # no FROM at all
            "UPDATE t SET a = 1",
            "TABLE t",
            "SELECT * FROM t WHERE (a = 1",  # unbalanced
            "SELECT * FROM t WHERE s = 'unterminated",
            "SELECT * FROM t /* unterminated",
            "SELECT * FROM t WHERE s = $tag$ unterminated",
        ]
        for sql in sqls:
            with self.subTest(sql=sql):
                self.assertIsNone(partition.table_of(sql))


class PartitionSqlTest(unittest.TestCase):
    """The text each ring runs: contiguous page ranges, one statement per part."""

    def test_no_where_adds_one(self):
        self.assertEqual(
            partition.partition_sql("SELECT * FROM t", 4, 100),
            [
                "SELECT * FROM t WHERE ctid >= '(0,0)'::tid AND ctid < '(25,0)'::tid",
                "SELECT * FROM t WHERE ctid >= '(25,0)'::tid AND ctid < '(50,0)'::tid",
                "SELECT * FROM t WHERE ctid >= '(50,0)'::tid AND ctid < '(75,0)'::tid",
                "SELECT * FROM t WHERE ctid >= '(75,0)'::tid AND ctid < '(100,0)'::tid",
            ],
        )

    def test_where_is_parenthesised_and_anded(self):
        # The caller's condition stays one condition (so `a OR b` cannot swallow
        # the range), and everything else in the statement is preserved.
        self.assertEqual(
            partition.partition_sql("SELECT * FROM t WHERE a > 1", 2, 10),
            [
                "SELECT * FROM t WHERE ( a > 1) AND "
                "ctid >= '(0,0)'::tid AND ctid < '(5,0)'::tid",
                "SELECT * FROM t WHERE ( a > 1) AND "
                "ctid >= '(5,0)'::tid AND ctid < '(10,0)'::tid",
            ],
        )
        self.assertEqual(
            partition.partition_sql("SELECT * FROM t WHERE a = 1 AND (b = 2 OR c = 3)", 2, 4),
            [
                "SELECT * FROM t WHERE ( a = 1 AND (b = 2 OR c = 3)) AND "
                "ctid >= '(0,0)'::tid AND ctid < '(2,0)'::tid",
                "SELECT * FROM t WHERE ( a = 1 AND (b = 2 OR c = 3)) AND "
                "ctid >= '(2,0)'::tid AND ctid < '(4,0)'::tid",
            ],
        )

    def test_ranges_are_contiguous_and_cover_every_page(self):
        sqls = partition.partition_sql("SELECT * FROM t WHERE a > 1", 4, 100)
        self.assertEqual(len(sqls), 4)
        for lo, hi in ((0, 25), (25, 50), (50, 75), (75, 100)):
            self.assertIn(f"ctid >= '({lo},0)'::tid AND ctid < '({hi},0)'::tid", sqls[lo // 25])

    def test_alias_and_select_list_are_preserved(self):
        self.assertEqual(
            partition.partition_sql("SELECT n FROM t x", 3, 10),
            [
                "SELECT n FROM t x WHERE ctid >= '(0,0)'::tid AND ctid < '(4,0)'::tid",
                "SELECT n FROM t x WHERE ctid >= '(4,0)'::tid AND ctid < '(8,0)'::tid",
                "SELECT n FROM t x WHERE ctid >= '(8,0)'::tid AND ctid < '(10,0)'::tid",
            ],
        )

    def test_remainder_goes_to_the_last_slice(self):
        sqls = partition.partition_sql("SELECT * FROM t", 3, 10)  # step 4
        self.assertEqual(where(sqls), [
            "ctid >= '(0,0)'::tid AND ctid < '(4,0)'::tid",
            "ctid >= '(4,0)'::tid AND ctid < '(8,0)'::tid",
            "ctid >= '(8,0)'::tid AND ctid < '(10,0)'::tid",
        ])

    def test_empty_ranges_are_skipped(self):
        # 5 pages in 4 parts: step 2 gives three ranges, the fourth would be empty.
        sqls = partition.partition_sql("SELECT * FROM t", 4, 5)
        self.assertEqual(len(sqls), 3)
        self.assertEqual(where(sqls), [
            "ctid >= '(0,0)'::tid AND ctid < '(2,0)'::tid",
            "ctid >= '(2,0)'::tid AND ctid < '(4,0)'::tid",
            "ctid >= '(4,0)'::tid AND ctid < '(5,0)'::tid",
        ])

    def test_trailing_semicolon_and_comment_stay_at_the_end(self):
        self.assertEqual(
            partition.partition_sql("SELECT * FROM t WHERE a = 1; -- done", 2, 4),
            [
                "SELECT * FROM t WHERE ( a = 1) AND "
                "ctid >= '(0,0)'::tid AND ctid < '(2,0)'::tid; -- done",
                "SELECT * FROM t WHERE ( a = 1) AND "
                "ctid >= '(2,0)'::tid AND ctid < '(4,0)'::tid; -- done",
            ],
        )

    def test_refuses_a_table_too_small_to_split(self):
        # Fewer pages than parts would leave a ring with nothing to read.
        self.assertIsNone(partition.partition_sql("SELECT * FROM t", 4, 3))
        self.assertIsNone(partition.partition_sql("SELECT * FROM t", 2, 1))
        self.assertIsNone(partition.partition_sql("SELECT * FROM t", 2, 0))

    def test_one_page_per_slice_is_the_boundary(self):
        self.assertEqual(where(partition.partition_sql("SELECT * FROM t", 3, 3)), [
            "ctid >= '(0,0)'::tid AND ctid < '(1,0)'::tid",
            "ctid >= '(1,0)'::tid AND ctid < '(2,0)'::tid",
            "ctid >= '(2,0)'::tid AND ctid < '(3,0)'::tid",
        ])

    def test_refuses_fewer_than_two_parts(self):
        for parts in (1, 0, -1):
            with self.subTest(parts=parts):
                self.assertIsNone(partition.partition_sql("SELECT * FROM t", parts, 100))

    def test_refuses_a_query_it_cannot_split(self):
        for sql in (
            "SELECT * FROM t JOIN u ON t.id = u.id",
            "SELECT * FROM t GROUP BY a",
            "SELECT * FROM t ORDER BY a",
            "SELECT * FROM t LIMIT 1",
            "WITH x AS (SELECT 1) SELECT * FROM x",
            "SELECT * FROM (SELECT * FROM t) s",
            "SELECT * FROM t, u",
            "SELECT * FROM generate_series(1, 10)",
            "SELECT 1",
        ):
            with self.subTest(sql=sql):
                self.assertIsNone(partition.partition_sql(sql, 4, 100))

    def test_refuses_a_page_count_it_cannot_use(self):
        self.assertIsNone(partition.partition_sql("SELECT * FROM t", 4, None))
        self.assertIsNone(partition.partition_sql("SELECT * FROM t", 4, "many"))


if __name__ == "__main__":
    unittest.main()
