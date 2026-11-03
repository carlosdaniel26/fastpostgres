"""The workload schema: a small commerce database with the shape a real one has.

Four tables with primary and foreign keys and the indexes an application would
create, at a scale given as a multiplier: 1 means 10k customers, 10k products,
1M orders and 3M order items (about 2 GB with the indexes). Money is `numeric`
on purpose - it is what money columns are, and it is not native to the row frame,
so the realistic reads exercise the columnar frame the client falls back to
instead of pretending every column is a `float8`.

`setup()` drops and recreates everything, loads it from `generate_series` and
then creates the indexes: a fresh, deterministic database for every run, so a
scenario's output is the same in both players and in every repetition.
"""
import os

import psycopg

DATABASE_URL = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")

# scale 1: the row counts an application of this shape might actually have
BASE = {"customers": 10_000, "products": 10_000, "orders": 1_000_000, "items_per_order": 3}

# Ids above this are the mixed scenario's inserts; the runner deletes them again
# between players, so both start from the same table.
RESERVED_ORDER_ID = 50_000_000

DDL = (
    "DROP TABLE IF EXISTS order_items CASCADE",
    "DROP TABLE IF EXISTS orders CASCADE",
    "DROP TABLE IF EXISTS products CASCADE",
    "DROP TABLE IF EXISTS customers CASCADE",
    """CREATE TABLE customers (
           id integer PRIMARY KEY,
           name text NOT NULL,
           email text NOT NULL,
           country text NOT NULL,
           created_at timestamptz NOT NULL)""",
    """CREATE TABLE products (
           id integer PRIMARY KEY,
           sku text NOT NULL,
           name text NOT NULL,
           category text NOT NULL,
           price numeric(10,2) NOT NULL)""",
    """CREATE TABLE orders (
           id integer PRIMARY KEY,
           customer_id integer NOT NULL REFERENCES customers(id),
           status text NOT NULL,
           total numeric(12,2) NOT NULL,
           created_at timestamptz NOT NULL)""",
    """CREATE TABLE order_items (
           order_id integer NOT NULL REFERENCES orders(id),
           product_id integer NOT NULL REFERENCES products(id),
           qty integer NOT NULL,
           unit_price numeric(10,2) NOT NULL,
           PRIMARY KEY (order_id, product_id))""",
)

INDEXES = (
    "CREATE INDEX orders_customer_created ON orders (customer_id, created_at DESC)",
    "CREATE INDEX orders_status_created ON orders (status, created_at DESC)",
    "CREATE INDEX order_items_product ON order_items (product_id)",
    "CREATE INDEX products_category ON products (category)",
)

TABLES = ("customers", "products", "orders", "order_items")


def counts(scale):
    return {k: max(1, int(round(v * scale))) for k, v in BASE.items()}


def setup(scale=1.0):
    """Recreate and load the schema; returns the row counts it built.

    The numbers are interpolated rather than bound: they are this module's own
    integers, and the generators use `%` as the modulo operator, which a
    parameterized statement would have to escape.
    """
    n = counts(scale)
    statements = [
        *DDL,
        f"""INSERT INTO customers (id, name, email, country, created_at)
            SELECT g, 'Customer ' || g, 'customer' || g || '@example.com',
                   (ARRAY['BR','US','DE','IN','JP','GB','FR','CA'])[1 + g % 8],
                   timestamptz '2024-01-01 00:00:00+00' + (g % 730) * interval '1 day'
            FROM generate_series(1, {n['customers']}) g""",
        f"""INSERT INTO products (id, sku, name, category, price)
            SELECT g, 'SKU-' || lpad(g::text, 6, '0'), 'Product ' || g,
                   (ARRAY['books','electronics','garden','kitchen','toys','sports'])[1 + g % 6],
                   round((g % 40000 + 999)::numeric / 100, 2)
            FROM generate_series(1, {n['products']}) g""",
        f"""INSERT INTO orders (id, customer_id, status, total, created_at)
            SELECT g, 1 + (g::bigint * 7919) % {n['customers']},
                   (ARRAY['new','paid','shipped','cancelled'])[1 + g % 4],
                   round((g % 2000000 + 500)::numeric / 100, 2),
                   timestamptz '2024-01-01 00:00:00+00' + (g % 730) * interval '1 day'
                     + (g % 86400) * interval '1 second'
            FROM generate_series(1, {n['orders']}) g""",
        f"""INSERT INTO order_items (order_id, product_id, qty, unit_price)
            SELECT o, 1 + (o * 31 + k * 7919) % {n['products']}, 1 + (o + k) % 5,
                   round(((1 + (o * 31 + k * 7919) % {n['products']}) % 40000 + 999)::numeric / 100, 2)
            FROM generate_series(1, {n['orders']}) o,
                 generate_series(1, {n['items_per_order']}) k""",
        *INDEXES,
        *(f"ANALYZE {table}" for table in TABLES),
    ]

    conn = psycopg.connect(DATABASE_URL)
    conn.autocommit = True
    try:
        with conn.cursor() as cur:
            for statement in statements:
                cur.execute(statement)
    finally:
        conn.close()
    return n


def reset_reserved_orders():
    """Empty the id range the mixed scenario writes into."""
    conn = psycopg.connect(DATABASE_URL)
    conn.autocommit = True
    try:
        with conn.cursor() as cur:
            cur.execute(f"DELETE FROM orders WHERE id > {RESERVED_ORDER_ID}")
    finally:
        conn.close()
