"""The default sealed-memfd data segment (FASTPOSTGRES_MEMFD).

The worker creates an anonymous memfd, seals it against shrink/grow and hands
the descriptor to the client once over the ring's AF_UNIX handoff. A functional
read alone cannot tell that apart from the named /dev/shm fallback, so this
test checks the three properties the default exists for:

  * there is no /dev/shm/<seg> file,
  * the object is sealed against both shrinking and growing,
  * the client maps the descriptor the worker handed over.

The named path still exists for a client that runs as a different user than the
server; set FASTPOSTGRES_MEMFD=0 and this test skips.
"""
import errno
import fcntl
import os
import unittest

import fastpostgres as fp
from fastpostgres import _client

CONNINFO = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")


def _memfd_enabled():
    value = os.environ.get("FASTPOSTGRES_MEMFD")
    if value is None or value == "":
        return True
    return value.lower() not in ("0", "off", "false", "no")


@unittest.skipUnless(_memfd_enabled(), "sealed memfd is disabled by FASTPOSTGRES_MEMFD")
class MemfdSegmentTest(unittest.TestCase):
    def test_sealed_memfd_replaces_the_named_segment(self):
        pid = os.getpid()
        seg = f"fp_memfd_seg_{pid}"
        conn = fp.Connection(CONNINFO)
        ring = fp.Ring(f"fp_memfd_ring_{pid}", 256 * 1024)
        try:
            ring.start(conn)
            ring.open(
                seg,
                "SELECT g::int AS id, ('x' || g)::text AS s FROM generate_series(1, 5000) g",
            )
            self.assertEqual(ring.next_rows(5000), 5000)

            # Map it on the client side: this is the handoff record being used.
            rows = _client.read_rows(seg).rows
            self.assertEqual(len(rows), 5000)
            self.assertEqual(rows[0], (1, "x1"))
            self.assertEqual(rows[-1], (5000, "x5000"))

            path = os.path.join("/dev/shm", seg)
            self.assertFalse(
                os.path.exists(path),
                f"{path} exists: the default sealed-memfd path left a named segment",
            )

            fd = _client.segment_fd(seg)
            self.assertIsNotNone(fd, "the data segment was mapped from /dev/shm, not a memfd")
            try:
                seals = fcntl.fcntl(fd, fcntl.F_GET_SEALS)
                self.assertTrue(seals & fcntl.F_SEAL_SHRINK, "the memfd is not sealed against shrink")
                self.assertTrue(seals & fcntl.F_SEAL_GROW, "the memfd is not sealed against grow")

                for size in (64 * 1024, 1 << 20):
                    with self.assertRaises(OSError) as caught:
                        os.ftruncate(fd, size)
                    self.assertEqual(
                        caught.exception.errno,
                        errno.EPERM,
                        f"ftruncate({size}) on a sealed memfd was not refused",
                    )
            finally:
                os.close(fd)

            ring.close()
        finally:
            ring.stop()
            conn.close()


if __name__ == "__main__":
    unittest.main()
