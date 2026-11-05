"""The asyncio face: one tier thread per connection, partitions leased per stream.

Needs a live server with the extension (DATABASE_URL, as the rest of the suite).
"""
import asyncio
import os
import unittest

import fastpostgres as fp
from fastpostgres import aio, dbapi

CONNINFO = os.environ.get("DATABASE_URL", "host=/tmp dbname=postgres user=postgres")

QUERY = (
    "SELECT g::int8 AS n, ('x' || g)::text AS s FROM generate_series(1, 300000) g"
)


class AioTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.conn = await aio.connect(CONNINFO)

    async def asyncTearDown(self):
        await self.conn.close()

    async def test_fetchall_matches_libpq(self):
        sql = "SELECT g::int4 AS n, ('x' || g)::text AS s FROM generate_series(1, 5) g"
        rows = await self.conn.fetchall(sql)
        plain = fp.Connection(CONNINFO).exec(sql).rows
        self.assertEqual(rows, plain)
        self.assertEqual(self.conn.stats()["shm_reads"], 1)

    async def test_execute_and_fetch_parity(self):
        cur = self.conn.cursor()
        await cur.execute("SELECT %s::int + %s::int", (2, 3))
        self.assertEqual(await cur.fetchone(), (5,))
        self.assertEqual(cur.rowcount, 1)
        self.assertEqual(cur.description[0][1], 23)  # int4

        await cur.execute("SELECT g::int AS n FROM generate_series(1, 3) g")
        self.assertEqual(await cur.fetchall(), [(1,), (2,), (3,)])

    async def test_first(self):
        self.assertEqual(await self.conn.first("SELECT g::int AS n FROM generate_series(7, 9) g"), (7,))
        self.assertIsNone(await self.conn.first("SELECT 1::int AS n WHERE false"))

    async def test_stream_is_zero_copy_and_reuses_its_ring(self):
        total = 0
        async with self.conn.stream(QUERY, size=40000) as batches:
            async for batch in batches:
                total += batch.row_count
                self.assertGreater(len(batch.columns), 0)
        self.assertEqual(total, 300000)

        created = dbapi.pool_created()
        rows = await self.conn.fetchall("SELECT 1::int AS n")
        self.assertEqual(rows, [(1,)])
        self.assertEqual(dbapi.pool_created(), created, "the stream's ring was not reused")

    async def test_loop_keeps_running_during_a_read(self):
        ticks = 0

        async def heartbeat():
            nonlocal ticks
            while True:
                await asyncio.sleep(0.001)
                ticks += 1

        beat = asyncio.create_task(heartbeat())
        try:
            rows = await self.conn.fetchall(
                "SELECT g::int8 AS n FROM generate_series(1, 1000000) g"
            )
        finally:
            beat.cancel()
        self.assertEqual(len(rows), 1000000)
        self.assertGreater(ticks, 10, "the event loop stalled while the tier read")

    async def test_concurrent_streams_on_one_loop(self):
        async def total(conn):
            n = 0
            async with conn.stream(QUERY, size=50000) as batches:
                async for batch in batches:
                    n += batch.row_count
            return n

        others = [await aio.connect(CONNINFO) for _ in range(2)]
        try:
            totals = await asyncio.gather(total(self.conn), total(others[0]), total(others[1]))
            self.assertEqual(totals, [300000, 300000, 300000])
        finally:
            for conn in others:
                await conn.close()

    async def test_second_concurrent_pull_is_refused(self):
        stream = self.conn.stream(QUERY, size=1000)
        await stream.__anext__()
        second = asyncio.create_task(stream.__anext__())
        await asyncio.sleep(0)  # let the second pull set its flag
        with self.assertRaises(RuntimeError):
            await stream.__anext__()
        await second
        await stream.aclose()

    async def test_cancelled_pull_returns_the_ring(self):
        stream = self.conn.stream(QUERY, size=1000)
        await stream.__anext__()
        created = dbapi.pool_created()

        pull = asyncio.create_task(stream.__anext__())
        await asyncio.sleep(0)
        pull.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await pull

        await stream.aclose()
        self.assertEqual(await self.conn.fetchall("SELECT 1::int AS n"), [(1,)])
        self.assertEqual(dbapi.pool_created(), created, "the cancelled stream leaked its ring")

    async def test_close_ends_a_live_stream(self):
        await self.conn.stream(QUERY, size=1000).__anext__()
        created = dbapi.pool_created()
        await self.conn.close()  # the live stream must not keep its ring

        other = await aio.connect(CONNINFO)
        try:
            self.assertEqual(await other.fetchall("SELECT 1::int AS n"), [(1,)])
            self.assertEqual(dbapi.pool_created(), created)
        finally:
            await other.close()

    async def test_errors(self):
        cur = self.conn.cursor()
        with self.assertRaises(dbapi.DatabaseError):
            await cur.execute("SELECT 1/0")

        broken = await aio.connect(CONNINFO, shm=False)
        try:
            with self.assertRaises(dbapi.NotSupportedError):
                broken.stream(QUERY)
            # shm=False still reads, over libpq
            self.assertEqual(await broken.fetchall("SELECT 1::int AS n"), [(1,)])
        finally:
            await broken.close()

    async def test_use_after_close(self):
        await self.conn.close()
        with self.assertRaises(dbapi.InterfaceError):
            await self.conn.fetchall("SELECT 1")
        await self.conn.close()  # idempotent

    async def test_forked_connection_is_refused(self):
        """The child must not drive the parent's PGconn or its tier."""
        conn = await aio.connect(CONNINFO)
        r, w = os.pipe()
        pid = os.fork()
        if pid == 0:
            try:
                os.close(r)
                asyncio.run(conn.fetchall("SELECT 1::int AS n"))
                os.write(w, b"drove-it")
            except BaseException as exc:
                os.write(w, f"{type(exc).__name__}".encode())
            finally:
                os._exit(0)
        os.close(w)
        message = os.read(r, 64).decode()
        os.close(r)
        os.waitpid(pid, 0)

        self.assertEqual(message, "InterfaceError", "the forked child drove the inherited connection")
        # the parent's connection is untouched
        self.assertEqual(await conn.fetchall("SELECT 1::int AS n"), [(1,)])
        await conn.close()


if __name__ == "__main__":
    unittest.main()
