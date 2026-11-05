"""The eventfd engine: many rings on fewer workers than rings.

Opt-in (``FASTPOSTGRES_TRANSPORT=eventfd``), because the semaphore engine's
one-worker-per-ring still fills the server's worker budget in the default run:
the pool needs at least one free slot to start, and with the budget already
spent by 1:1 workers there is nowhere to put the first pool worker. Run this
module with::

    FASTPOSTGRES_TRANSPORT=eventfd python3 -m unittest tests.test_eventfd

It proves the two properties the decoupling exists for: eight rings is more
rings than ``max_worker_processes`` has slots (eight, one held back), and every
one of them is served correctly anyway, on fewer workers.
"""
import os
import threading
import time
import unittest

import fastpostgres as fp
from fastpostgres import dbapi

CONNINFO = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")
EVENTFD = os.environ.get("FASTPOSTGRES_TRANSPORT") == "eventfd"
RING_BACKEND = "fastpostgres ring worker"
N = 8


def ring_workers():
    conn = dbapi.connect(CONNINFO, shm=False)
    try:
        return int(
            conn._conn.exec(
                f"SELECT count(*) FROM pg_stat_activity WHERE backend_type = '{RING_BACKEND}'"
            ).rows[0][0]
        )
    finally:
        conn.close()


@unittest.skipUnless(EVENTFD, "the eventfd engine is opt-in (FASTPOSTGRES_TRANSPORT=eventfd)")
class EventfdPoolTest(unittest.TestCase):
    def test_more_rings_than_workers(self):
        conn = fp.Connection(CONNINFO)
        before = ring_workers()
        rings = [fp.Ring(f"fp_efd_{i}", 0, "eventfd") for i in range(N)]
        done = threading.Barrier(N + 1)
        errors = []
        counts = [0] * N

        def work(i):
            try:
                rings[i].open(f"fp_efd_seg_{i}", "SELECT g::int AS id FROM generate_series(1, 2000) g")
                done.wait(timeout=30)  # every ring holds its stream at once
                counts[i] = rings[i].next(1000) + rings[i].next(1000)
                rings[i].close()
            except BaseException as exc:
                errors.append(f"{type(exc).__name__}: {exc}")
                try:
                    done.abort()
                except Exception:
                    pass

        try:
            for ring in rings:
                ring.start(conn)
            self.assertGreaterEqual(N, 8)
            workers = ring_workers()
            self.assertLess(workers, N, "eight rings should not need eight workers")
            self.assertGreaterEqual(workers, 1)

            threads = [threading.Thread(target=work, args=(i,)) for i in range(N)]
            for t in threads:
                t.start()
            done.wait(timeout=30)
            for t in threads:
                t.join(timeout=60)

            self.assertEqual(errors, [])
            self.assertEqual(counts, [2000] * N)
        finally:
            for ring in rings:
                try:
                    ring.stop()
                except Exception:
                    pass
            conn.close()

            # the workers our rings started exit once their rings are gone; give
            # the suite's next test its slots back
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and ring_workers() > before:
                time.sleep(0.1)


if __name__ == "__main__":
    unittest.main()
