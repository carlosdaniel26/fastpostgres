#!/usr/bin/env python3
"""Fetch the ClickBench `hits` data once, into a git-ignored directory.

ClickBench publishes one file — `hits.tsv.gz`, 16.3 GB — and no official
small sample at that path. Holding all of it is out of the question on a
benchmark host, so this streams the gzip and keeps a **bounded prefix**: the
header and the first `--rows` data rows, which is a deterministic slice of the
same file rather than a different one.

Be precise about what that is: these are upstream's first N rows, not
ClickBench's "small" dataset, and the q/s measured on them is comparable to
another run of this harness, not to the published 100M-row table. The README
says the same thing next to the pin.

    python3 fetch.py                 # default: 1,000,000 rows
    python3 fetch.py --rows 100000   # a cheaper slice for a smoke run
    python3 fetch.py --force         # re-fetch even if the file is there

It is a no-op when `data/hits.tsv` already exists and its recorded checksum
matches, so a benchmark run can call it unconditionally.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
DATA = HERE / "data"
TSV = DATA / "hits.tsv"
META = DATA / "source.json"

URL = "https://datasets.clickhouse.com/hits_compatible/hits.tsv.gz"
DEFAULT_ROWS = 1_000_000


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def recorded() -> dict:
    if META.exists():
        try:
            return json.loads(META.read_text())
        except ValueError:
            pass
    return {}


def fetch(rows: int, force: bool) -> int:
    DATA.mkdir(exist_ok=True)
    meta = recorded()

    if not force and TSV.exists() and meta.get("rows") == rows and meta.get("sha256") == sha256(TSV):
        print(f"fetch: {TSV} is current ({rows} rows, {meta['sha256'][:12]}…)")
        return None  # current is a success with nothing to do

    part = TSV.with_suffix(".tsv.part")
    print(f"fetch: streaming {URL} -> first {rows} rows", flush=True)

    proc, src = _curl()
    try:
        with src, part.open("wb") as out:
            header = src.readline()
            out.write(header)
            for i in range(rows):
                line = src.readline()
                if not line:  # upstream shorter than asked: keep what exists
                    rows = i
                    break
                out.write(line)
    finally:
        # We stop reading as soon as the prefix is written, so curl exits 23
        # ("failure writing output") by design. That is this command's happy path.
        try:
            proc.terminate()
            proc.wait(timeout=10)
        except Exception:
            pass

    digest = sha256(part)
    part.replace(TSV)
    META.write_text(
        json.dumps(
            {
                "url": URL,
                "rows": rows,
                "sha256": digest,
                "fetched": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                "note": "upstream's first N data rows, not ClickBench's small sample",
            },
            indent=2,
        )
        + "\n"
    )
    print(f"fetch: {TSV} {rows} rows, sha256 {digest}")
    return rows


def _curl():
    """A streaming gunzip of upstream's file: the 16 GB is never written to disk,
    and closing early is what makes the bounded prefix cheap. curl rather than
    urllib because the dataset host refuses a bare urllib request (403)."""
    import subprocess

    proc = subprocess.Popen(["curl", "-sS", "--fail", "-L", URL], stdout=subprocess.PIPE)
    return proc, gzip.GzipFile(fileobj=proc.stdout, mode="rb")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--rows", type=int, default=DEFAULT_ROWS, help="data rows to keep (default: 1,000,000)")
    ap.add_argument("--force", action="store_true", help="re-fetch even when the file is current")
    args = ap.parse_args()
    if args.rows <= 0:
        raise SystemExit("fetch: --rows must be positive")
    rows = fetch(args.rows, args.force)
    return 0 if rows is None or rows > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
