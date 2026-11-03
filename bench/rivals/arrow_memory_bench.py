"""fastpostgres against ADBC + Arrow, under one memory budget.

The read benchmark (`just bench`) asks which stack delivers the same rows faster.
This one also asks what each side holds while it delivers, because the two
transports are not bounded the same way:

    ring         stream()'s columnar batches, summed in place with numpy. The
                 transfer buffer is the two data segments the ring worker maps,
                 fixed by FASTPOSTGRES_SEGMENT_BYTES and identical for every row
                 count - 512 MiB here, 256 MiB per segment.
    arrow-batch  ADBC's RecordBatchReader, consumed and summed one Arrow record
                 batch at a time. What it holds is one batch, and the batch size
                 is the driver's to choose.
    arrow-table  ADBC's fetch_arrow_table(), summed with pyarrow.compute. What it
                 holds is the whole result, in Arrow buffers.

All three return the same number - the sum of the same column over the same rows
- and the harness compares them, so a faster path that returned something else
would be caught. `ring` cannot fall back to a socket: stream() raises when the
ring is unavailable, so a successful read there is a ring read, and the segment
sizes it reports are the worker's own files: that is how the budget is proven
rather than assumed.

One process per measurement, as everywhere here: run inside the dev image by
bench/harness/arrow_memory_bench.sh.

    arrow_memory_bench.py setup <n>
    arrow_memory_bench.py read <player> <n> <reps>

Output, two lines: `median_ms min_ms max_ms rows sum`, then the player's evidence
tagged with the player's own name (what it carried and its peak RSS).
"""
import os
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # bench/

import adbc_driver_postgresql.dbapi as adbc  # noqa: E402
import numpy as np  # noqa: E402
import pyarrow.compute as pc  # noqa: E402
from fastpostgres import dbapi  # noqa: E402

from lib.harness import rss_bytes  # noqa: E402

from sqlalchemy_bench import CORE_SQL, SHAPE, SHAPES, setup

DATABASE_URL = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")
BASE = os.environ.get("SQLALCHEMY_DB", "postgres@/postgres?host=/tmp")
ARROW_URI = f"postgresql://{BASE}"
# FASTPOSTGRES_RING_BYTES: the control channel's own segment, fixed and not part
# of the transfer buffer the budget is spent on.
CONTROL_RING_BYTES = 8 << 20
SUM_COLUMN = 3  # price: the column both stacks sum in place
BATCH = 100000  # rows per stream() pull, the client's own _SHM_BATCH


def now():
    return time.perf_counter()


def read_ring(conn):
    """stream()'s zero-copy column views, summed in place with numpy."""
    cur = conn.cursor()
    t0 = now()
    rows = 0
    total = 0.0
    batch_rows = batch_bytes = 0
    for batch in cur.stream(CORE_SQL, size=BATCH):
        rows += batch.row_count
        if batch.row_count:
            total += float(np.frombuffer(batch.columns[SUM_COLUMN].data, dtype="<f8").sum())
            batch_rows = max(batch_rows, batch.row_count)
            batch_bytes = max(batch_bytes, frame_bytes(batch))
    ms = (now() - t0) * 1000
    cur.close()
    return ms, rows, total, {"max_batch_rows": batch_rows, "max_batch_bytes": batch_bytes}


def frame_bytes(batch):
    """What one columnar batch carried: each column's bytes, plus its null bitmap.

    `data` is the whole column region - for text that is the offset table followed
    by the text itself - and `bytes`/`offsets` are views *into* it, so they must
    not be added on top or the text is counted twice. That is what the first
    version of this benchmark did, and it made the ring look ~50% fatter per row
    than Arrow while carrying the same bytes.
    """
    def n(view):
        return len(view) if view is not None else 0

    return sum(n(c.data) + n(c.nulls) for c in batch.columns)


def read_arrow_batch(conn):
    """ADBC's record-batch reader: one batch in Arrow buffers at a time."""
    cur = conn.cursor()
    cur.execute(CORE_SQL)
    t0 = now()
    rows = 0
    batches = 0
    total = 0.0
    batch_rows = batch_bytes = 0
    for batch in cur.fetch_record_batch():
        rows += batch.num_rows
        batches += 1
        total += float(pc.sum(batch.column(SUM_COLUMN)).as_py())
        batch_rows = max(batch_rows, batch.num_rows)
        batch_bytes = max(batch_bytes, batch.nbytes)
    cur.close()
    conn.commit()  # the driver does not autocommit; the read's transaction ends here
    ms = (now() - t0) * 1000
    return ms, rows, total, {"batches": batches, "max_batch_rows": batch_rows,
                             "max_batch_bytes": batch_bytes}


def read_arrow_table(conn):
    """ADBC's Arrow table: the whole result in Arrow buffers, summed in place."""
    cur = conn.cursor()
    cur.execute(CORE_SQL)
    t0 = now()
    table = cur.fetch_arrow_table()
    total = float(pc.sum(table["price"]).as_py())
    cur.close()
    conn.commit()
    ms = (now() - t0) * 1000
    return ms, table.num_rows, total, {"result_bytes": table.nbytes,
                                       "chunks": table.column(0).num_chunks}


def open_ring():
    return dbapi.connect(DATABASE_URL)


def open_arrow():
    return adbc.connect(ARROW_URI)


PLAYERS = {
    "ring": (open_ring, read_ring),
    "arrow-batch": (open_arrow, read_arrow_batch),
    "arrow-table": (open_arrow, read_arrow_table),
}


def ring_evidence(extra):
    """The worker's own data segments, the batch it carried, and this process's peak."""
    stem = f"fp_dbapi_{os.getpid()}_"
    sizes = [
        os.stat(os.path.join("/dev/shm", name)).st_size
        for name in sorted(os.listdir("/dev/shm"))
        # stream() fills <name>_seg; the cursor path alternates _seg_a/_seg_b
        if name.startswith(stem) and "_seg" in name
    ]
    carried = " ".join(f"{k}={v}" for k, v in extra.items())
    return (
        f"seg_bytes={max(sizes) if sizes else 0} segments={len(sizes)} "
        f"pair_bytes={sum(sizes)} ctrl_bytes={CONTROL_RING_BYTES} {carried} "
        f"rss_bytes={rss_bytes()}"
    )


def do_read(player_name, n, reps):
    open_handle, read = PLAYERS[player_name]
    handle = open_handle()
    try:
        read(handle)  # warm: connections, rings, arrow pools, plans
        times = []
        rows = 0
        total = 0.0
        extra = {}
        for _ in range(reps):
            ms, rows, total, extra = read(handle)
            times.append(ms)
        if rows != n:
            raise SystemExit(f"{player_name}: read {rows} rows, wanted {n}")
    finally:
        handle.close()
    fields = [
        f"{statistics.median(times):.3f}",
        f"{min(times):.3f}",
        f"{max(times):.3f}",
        str(rows),
        f"{total:.6f}",
    ]
    return fields, extra


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    if argv[1] == "setup":
        if SHAPE not in SHAPES:
            raise SystemExit(f"SHAPE={SHAPE!r}: choose one of {sorted(SHAPES)}")
        setup(int(argv[2]))
        return
    if argv[1] == "read":
        if len(argv) < 5:
            raise SystemExit(__doc__)
        player_name, n, reps = argv[2], int(argv[3]), int(argv[4])
        if player_name not in PLAYERS:
            raise SystemExit(f"unknown player {player_name!r}: {sorted(PLAYERS)}")
        fields, extra = do_read(player_name, n, reps)
        print(" ".join(fields))
        if player_name == "ring":
            print(f"{player_name} " + ring_evidence(extra))
        else:
            print(f"{player_name} " + " ".join(f"{k}={v}" for k, v in extra.items())
                  + f" rss_bytes={rss_bytes()}")
        return
    raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
