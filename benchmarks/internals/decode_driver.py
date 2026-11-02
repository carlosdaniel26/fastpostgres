"""The row decode on a fixed shape, for the offline meters.

Runs inside the image. `_client.read_rows` is the exact call the SQLAlchemy
benchmark's shm path makes (dbapi.py:572); this driver makes it directly, with
no SQLAlchemy in the way, so callgrind's instruction counts and llvm-mca's
region belong to the decode and not to the ORM. One shape per process: the
object arena is append-only and process-scoped (benchmarks/jobs/benchmarks.sh:9-20).

  bench   the benchmark's own row shape (id, sku, name, price, stock, active),
          same expressions as its INSERT, so cycles/row and Ir/row are
          comparable to the numbers already in docs/DECODE-NOTES.md
  mixed   the same six columns with NULLs on ~10% of the values and text
          lengths on both sides of the 126-byte varlena header split, plus a
          non-ASCII value every eighth row: the null test, the alignment walk
          and the fp_birth_text bail-out actually run, and mispredict

A small warmup batch runs first and is decoded, so the measured batches are
steady-state (the rdtsc totals are per process and cannot exclude it; at
min(rows, 1000) it is well under 1% of the total).

Usage: python3 benchmarks/internals/decode_driver.py [--shape bench|mixed] [rows] [batches]
       (defaults: bench 100000 3)

The extension's report prints at process exit on stderr:

  objpool: immortal cap=... objects=... fallbacks=...
  read_rows: <cycles/row> over <rows> rows (decode <cycles/row>, frame+desc+list <cycles/row>)

`just decode-cycles` runs this and parses those lines; `just decode-ir` runs it
again under callgrind with collection toggled on for `fp_decode_tuples`, for the
dynamic instruction count and the miss profile of the same scope.
"""
import argparse
import os
import sys

import fastpostgres as fp
from fastpostgres import _client

CONNINFO = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")

# One ring per process, like the dbapi pool (dbapi._prefix): a fixed name would
# collide when drop-in runs share a container.
STEM = f"fp_decode_{os.getpid()}"
SEG = f"{STEM}_seg"

SHAPES = {
    "bench": (
        "SELECT g::int4 AS id, ('SKU-' || g)::text AS sku, ('Product ' || g)::text AS name, "
        "(g * 0.5)::float8 AS price, (g % 500)::int4 AS stock, (g % 2 = 0)::bool AS active "
        "FROM generate_series(1, {n}) g"
    ),
    "mixed": (
        "SELECT g::int4 AS id, "
        "CASE WHEN g % 10 = 0 THEN NULL "
        "     WHEN g % 8 = 0 THEN repeat('é', 1 + g % 12) "
        "     ELSE 'SKU-' || g END AS sku, "
        "CASE WHEN g % 7 = 0 THEN repeat('x', 130 + g % 70) "
        "     WHEN g % 10 = 0 THEN NULL "
        "     ELSE repeat('y', 1 + g % 54) END AS name, "
        "(g * 0.5)::float8 AS price, "
        "CASE WHEN g % 10 = 0 THEN NULL ELSE (g % 500)::int4 END AS stock, "
        "(g % 2 = 0)::bool AS active "
        "FROM generate_series(1, {n}) g"
    ),
}


def positive(text):
    n = int(text)
    if n <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return n


def pull(ring, rows):
    """One next_rows() + read_rows(): the decode this tooling measures."""
    got = ring.next_rows(rows)
    if got != rows:
        raise SystemExit(f"decode_driver: asked for {rows} rows, the worker sent {got}")
    res = _client.read_rows(SEG)
    if len(res.rows) != rows:
        raise SystemExit(f"decode_driver: decode produced {len(res.rows)} rows, expected {rows}")
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--shape", choices=sorted(SHAPES), default="bench")
    ap.add_argument("rows", nargs="?", type=positive, default=100000,
                    help="rows per measured batch (default: %(default)s)")
    ap.add_argument("batches", nargs="?", type=positive, default=3,
                    help="measured batches (default: %(default)s)")
    args = ap.parse_args()

    warmup = min(args.rows, 1000)
    total = warmup + args.rows * args.batches
    query = SHAPES[args.shape].format(n=total)

    conn = fp.Connection(CONNINFO)
    ring = fp.Ring(STEM)
    try:
        ring.start(conn)
        ring.open(SEG, query)

        res = pull(ring, warmup)
        print("### DECODE")
        print(f"shape: {args.shape}")
        print(f"rows: {args.rows} batches: {args.batches} warmup: {warmup}")
        print(f"arena: {_client.objpool}")  # off/exhausted/immortal, decided at import
        print(f"fields: {', '.join(res.fields)}")
        print(f"oids: {', '.join(str(t) for t in res.field_types)}")
        print(f"query: {' '.join(query.split())}")
        print("### END DECODE")

        decoded = 0
        for _ in range(args.batches):
            decoded += len(pull(ring, args.rows).rows)
        print(f"decode_driver: shape={args.shape} rows={args.rows} batches={args.batches} decoded={decoded} ok")
    finally:
        ring.close()
        ring.stop()
        conn.close()


if __name__ == "__main__":
    sys.exit(main())
