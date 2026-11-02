"""The workload scenarios: what an application does, one spec per shape.

Every scenario says what it is (name, class, kind), the SQL it runs, how its
parameters are built - seeded, so both players issue the same statements - and
how a result is checked. The check is the repo's rule at a smaller grain: each
statement's rows are verified against its parameters and folded into a digest the
runner compares across players, so a faster path that returned something else
fails the run.

The SQL is deliberately ordinary: money is `numeric`, the page has an ORDER BY,
the join has a filter, and nothing is a single-table `generate_series` scan.
"""
from dataclasses import dataclass

from schema import RESERVED_ORDER_ID

LOOKUP_SQL = "SELECT id, name, country FROM customers WHERE id = %s"

PAGED_SQL = """SELECT id, status, total, created_at
               FROM orders
               WHERE customer_id = %s
               ORDER BY created_at DESC, id DESC
               LIMIT 100"""

SCAN_SQL = """SELECT o.id, c.name, o.status, o.total, o.created_at
              FROM orders o JOIN customers c ON c.id = o.customer_id"""

JOIN_SQL = """SELECT oi.order_id, oi.qty, oi.unit_price, p.sku, p.category
              FROM order_items oi JOIN products p ON p.id = oi.product_id
              WHERE oi.order_id <= %s"""

AGGREGATE_SQL = """SELECT c.country, count(*) AS orders, sum(o.total) AS total
                   FROM orders o JOIN customers c ON c.id = o.customer_id
                   GROUP BY c.country
                   ORDER BY c.country"""

INSERT_SQL = """INSERT INTO orders (id, customer_id, status, total, created_at)
                VALUES (%s, %s, 'new', %s, now())"""


@dataclass(frozen=True)
class Scenario:
    name: str
    klass: str  # app | analytics | concurrency | mixed
    kind: str  # statements | bulk | duration
    note: str
    sql: str
    param: object  # (i, counts, rng) -> tuple of statement parameters
    digest: object  # (rows, param) -> (ok, count, accumulator_a, accumulator_b)
    statements: int = 200  # per rep, when kind is "statements"
    write_sql: str = ""  # the mixed scenario's write side


def customer_param(i, counts, rng):
    return (1 + rng.randrange(counts["customers"]),)


def orders_cap_param(i, counts, rng):
    return (int(counts["orders"] * 0.9),)


def no_param(i, counts, rng):
    return ()


def write_param(i, counts, rng):
    return (RESERVED_ORDER_ID + 1 + i, 1 + rng.randrange(counts["customers"]),
            round(5 + (i % 20000) / 100, 2))


def digest_lookup(rows, param):
    ok = len(rows) == 1 and rows[0][0] == param[0]
    return ok, len(rows), sum(r[0] for r in rows), sum(len(r[1]) for r in rows)


def digest_paged(rows, param):
    ok = len(rows) <= 100 and all(r[2] >= 0 for r in rows)
    return ok, len(rows), sum(r[0] for r in rows), sum(float(r[2]) for r in rows)


def digest_scan(rows, param):
    return True, len(rows), sum(r[0] for r in rows), sum(float(r[3]) for r in rows)


def digest_join(rows, param):
    return True, len(rows), sum(r[0] for r in rows), sum(r[1] for r in rows)


def digest_aggregate(rows, param):
    return True, len(rows), sum(r[1] for r in rows), sum(float(r[2]) for r in rows)


SCENARIOS = (
    Scenario("lookup", "app", "statements", "primary-key lookups, one row out",
             LOOKUP_SQL, customer_param, digest_lookup),
    Scenario("paged", "app", "statements", "filtered, ordered page of 100 rows",
             PAGED_SQL, customer_param, digest_paged),
    Scenario("scan", "analytics", "bulk", "orders x customers, every row out",
             SCAN_SQL, no_param, digest_scan),
    Scenario("join", "analytics", "bulk", "order_items x products, 90% of the rows",
             JOIN_SQL, orders_cap_param, digest_join),
    Scenario("aggregate", "analytics", "bulk", "group by country over the join",
             AGGREGATE_SQL, no_param, digest_aggregate),
    Scenario("concurrent", "concurrency", "duration", "clients on the paged query",
             PAGED_SQL, customer_param, digest_paged),
    Scenario("mixed", "mixed", "duration", "90/10 paged reads and order inserts",
             PAGED_SQL, customer_param, digest_paged, write_sql=INSERT_SQL),
)

BY_NAME = {scenario.name: scenario for scenario in SCENARIOS}
