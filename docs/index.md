# FastPostgres

PostgreSQL query results delivered through POSIX shared memory as typed arrays,
instead of the socket's text protocol. A background worker runs each read with a
portal and publishes every batch into `/dev/shm`; the client maps the segment and
reads it in place. Writes, DDL and transactions keep using libpq.

These pages are the arguments and the records behind the project:

| start here | it answers |
|---|---|
| [Benchmarks](BENCHMARKS.md) | what is measured, in which class, and how to run it |
| [Records](RESULTS.md) | every committed measurement, and which claim it backs |
| [Method](METHODOLOGY.md) | the protocol, the statistics, and what would invalidate them |
| [Limits](LIMITATIONS.md) | what it does not cover, and what happens there |
| [Why it wins](WHY-IT-WINS.md) | the mechanism, rather than the ratio |

The README at the repository root has the measured results and the quick start;
[`DOCUMENTATION.md`](https://github.com/carlosdaniel26/fastpostgres/blob/main/DOCUMENTATION.md)
has the project layout and the full index.
