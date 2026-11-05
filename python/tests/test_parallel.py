"""N threads, N connections, N partitions: the pool's leases in parallel.

Each thread opens its own stream and holds its ring — its partition of the
shared memory — while the others do the same; the pool is the partition table.
Run in a child process so FASTPOSTGRES_SHM_POOL can be set before the pool is
resolved (it is read at import and fixed on the first read).
"""
import json
import os
import subprocess
import sys
import unittest

from fastpostgres import dbapi

CONNINFO = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")
QUERY = "SELECT g::int8 AS n, ('x' || g)::text AS s FROM generate_series(1, 20000) g"

# One thread per partition. Each thread pulls its first batch (which leases a
# ring and holds it: a stream keeps its partition until it ends), then everyone
# waits on the barrier — so all N partitions exist at once, and the lease count
# read there is the evidence, not a timing coincidence.
PROGRAM = r"""
import json, os, threading
from fastpostgres import dbapi

conninfo = os.environ["DATABASE_URL"]
query = os.environ["QUERY"]
n = int(os.environ["N"])
rows = [None] * n
leases = [None] * n
errors = []
ready = threading.Barrier(n)

def work(i):
    try:
        conn = dbapi.connect(conninfo)
        stream = conn.cursor().stream(query, size=20000)
        first = next(stream)              # this thread holds its ring from here
        ready.wait()
        leases[i] = len(dbapi.pool_stats()["leases"])
        rows[i] = first.row_count + sum(b.row_count for b in stream)
        conn.close()
    except BaseException as exc:
        errors.append(f"{type(exc).__name__}: {exc}")
        try:
            ready.abort()
        except Exception:
            pass

threads = [threading.Thread(target=work, args=(i,)) for i in range(n)]
for t in threads:
    t.start()
for t in threads:
    t.join()

prefix = f"fp_dbapi_{os.getpid()}_"
names = sorted(n for n in os.listdir("/dev/shm") if n.startswith(prefix))
# A stream reads its data segment named "<ring>_seg"; only the tuple path
# (_shm_read) alternates two, "_seg_a"/"_seg_b". This program streams, so each
# ring comes with one.
segments = [n for n in names if n.endswith(("_seg", "_seg_a", "_seg_b"))]
print(json.dumps({
    "errors": errors,
    "rows": rows,
    "leases": leases,
    "created": dbapi.pool_created(),
    "size": dbapi.pool_size(),
    "rings": len(names) - len(segments),
    "segments": len(segments),
}))
"""


class ParallelTest(unittest.TestCase):
    def _free_ring_slots(self):
        """The server's worker budget, less what this process's rings hold."""
        conn = dbapi.connect(CONNINFO)
        try:
            return dbapi._server_ring_slots(conn._conn)
        finally:
            conn.close()

    def test_threads_each_get_their_own_partition(self):
        # One ring is one background worker and the server budgets them
        # (max_worker_processes): the suite's earlier tests already hold rings of
        # their own, so the worker count asked for here is what is still free.
        slots = self._free_ring_slots()
        n = 4 if slots is None else min(4, slots)
        if n < 2:
            self.skipTest(f"the server has {slots} free ring slots; this test needs 2")
        print(f"[test_parallel] {n} partitions (slots free: {slots})")

        env = dict(os.environ, FASTPOSTGRES_SHM_POOL=str(n), N=str(n), QUERY=QUERY)
        proc = subprocess.run(
            [sys.executable, "-c", PROGRAM], env=env, capture_output=True, text=True, timeout=120
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-4000:])
        out = json.loads(proc.stdout.strip().splitlines()[-1])

        self.assertEqual(out["errors"], [])
        self.assertEqual(out["rows"], [20000] * n, "the threads disagreed on the same read")
        self.assertEqual(out["leases"], [n] * n, "every thread should hold its own partition")
        self.assertEqual(out["created"], n)
        self.assertEqual(out["size"], n)
        self.assertEqual(out["rings"], n, "one control ring per partition")
        self.assertEqual(out["segments"], n, "one data segment per streaming partition")

    def test_forked_child_does_not_inherit_leases(self):
        """A child forks while the parent holds a stream open: the child must take
        its own stem and its own rings, and the parent's stream must still work."""
        conn = dbapi.connect(CONNINFO)
        try:
            stream = conn.cursor().stream(QUERY, size=4000)
            first = next(stream)  # the parent now holds a ring lease
            self.assertGreater(first.row_count, 0)

            r, w = os.pipe()
            pid = os.fork()
            if pid == 0:  # child
                try:
                    os.close(r)
                    child = dbapi.connect(CONNINFO)
                    cur = child.cursor()
                    cur.execute("SELECT 1::int AS n")
                    rows = cur.fetchall()
                    child.close()
                    os.write(w, f"ok {rows[0][0]} {dbapi.pool_created()}".encode())
                except BaseException as exc:
                    os.write(w, f"err {type(exc).__name__}: {exc}".encode())
                finally:
                    os._exit(0)
            os.close(w)
            message = os.read(r, 4096).decode()
            os.close(r)
            _, status = os.waitpid(pid, 0)

            self.assertEqual(status, 0)
            self.assertTrue(message.startswith("ok 1"), message)
            self.assertEqual(message.split()[2], "1", "the child's pool is only its own")

            # the parent's lease survived the fork and is still the one it holds
            rest = sum(batch.row_count for batch in stream)
            self.assertEqual(rest + first.row_count, 20000)
        finally:
            conn.close()

    def test_close_ends_a_live_stream_and_returns_the_ring(self):
        conn = dbapi.connect(CONNINFO)
        stream = conn.cursor().stream(QUERY, size=4000)
        next(stream)  # a ring is leased and held now
        created = dbapi.pool_created()
        conn.close()  # must CLOSE the stream's ring, not leak it

        other = dbapi.connect(CONNINFO)
        try:
            cur = other.cursor()
            cur.execute("SELECT 1::int AS n")
            self.assertEqual(cur.fetchall(), [(1,)])
            self.assertEqual(dbapi.pool_created(), created, "no new ring was needed")
        finally:
            other.close()


class RingGuardTest(unittest.TestCase):
    def test_one_ring_two_threads_is_refused(self):
        """The C layer refuses a second driver instead of interleaving the slot."""
        conn = dbapi.connect(CONNINFO)
        try:
            info = dbapi._ring_acquire(conn._conn, block=True, timeout=5)
            self.assertIsNotNone(info, "no ring available to test with")
            ring = info[0]
            result = {}

            def second():
                try:
                    ring.open("fp_guard_seg", "SELECT 1")
                    result["outcome"] = "drove the ring"
                except Exception as exc:
                    result["outcome"] = type(exc).__name__

            try:
                ring.open("fp_guard_seg", "SELECT 1")  # this thread holds the stream now
                import threading

                t = threading.Thread(target=second)
                t.start()
                t.join()
                self.assertEqual(result["outcome"], "RuntimeError", "the second driver was not refused")
            finally:
                try:
                    ring.close()
                except Exception:
                    pass
                dbapi._ring_release(info)
        finally:
            conn.close()


if __name__ == "__main__":
    unittest.main()
