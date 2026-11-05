# Sealed memfd data segments ("Seal It")

Status: implemented, opt-in with `FASTPOSTGRES_MEMFD=1`, default unchanged.
Branch: `team/formats`.

## What was wrong

The data segment was a named POSIX shared-memory object, `/dev/shm/<seg>`,
created by the worker with `shm_open(..., 0666)` and then widened with
`fchmod(fd, 0666)` so that a client running as a different user (or a process
that simply knew the name) could open it. The segment is an SQL-execution
endpoint: whoever can open it can read results and, in the ring's model, drive
the worker. The name plus a world-writable inode was the trust boundary.

It was also re-created on every batch: `fastpostgres_shm_map_rw()` did
`shm_open` + `fchmod` + `fstat` + `mmap` + `madvise`, and every publisher did
`munmap`, once per `next()`/`next_rows()`. The client already cached its
read-only mapping, but the writer paid a full map/unmap of a 1 GiB virtual
region per batch. That is the cost the small-batch result below removes.

## The replacement

1. **Worker creates an anonymous memfd** per data-segment name with
   `memfd_create("fastpostgres-seg", MFD_CLOEXEC | MFD_ALLOW_SEALING)`.
2. It `ftruncate`s the memfd to the ring's requested capacity **once**.
3. It `mmap`s it `PROT_READ|PROT_WRITE, MAP_SHARED`, calls
   `madvise(MADV_HUGEPAGE)` before the first write, and only then adds seals.
4. It seals `F_SEAL_SHRINK | F_SEAL_GROW` (see "Sealing choice").
5. It **sends the descriptor to the client once** with `SCM_RIGHTS`.
6. The client `mmap`s it `PROT_READ, MAP_SHARED` once and caches it by segment
   name for the process lifetime. On a later batch it revalidates the segment
   header in place; no re-open, no re-map.
7. Both sides keep the mapping for the duration of the worker/process, so the
   batch hot path is: write into the cached mapping, publish the header, post
   the semaphore; read the cached mapping, decode.

The `FPCOLUMN` and `FPROWS` frames, the ring ops (`OPEN`, `NEXT`,
`NEXT_ROWS`, `CLOSE`, `STOP`), the batch field and the publish barriers are
byte-for-byte unchanged. This slice changes only the segment's backing store
and when its mapping is created.

## The fd protocol

The client is the listener, because it is the side that already owns the
bootstrap step (`fastpostgres_ring_start()` over libpq):

1. Before it bootstraps the worker, `Ring_start` creates an
   `AF_UNIX`/`SOCK_SEQPACKET` socket, binds it in the **abstract namespace**
   as `\0fpq-<pid>-<serial>`, and calls `listen(4)`. There is no filesystem
   name to leak or unlink.
2. It writes the abstract address into the ring's appended handoff fields:
   `fd_handoff = 1`, `fd_pathlen`, `fd_path[108]` (`sockaddr_un.sun_path`).
   The fields sit after `FastpostgresRingTally`, so every offset an older
   reader parses (including the tally at 4488) is unchanged; `FPRING05` and the
   ring's control-segment protocol stay as they were.
3. The worker, at startup and before it publishes `worker_ready`, connects to
   that address. The connection is therefore already queued when the client
   `accept`s after `wait_ready`.
4. On the first request that names a data segment, the worker creates that
   segment's memfd and sends one `SOCK_SEQPACKET` record:
   `{name_len, has_fd, size}` + the name bytes + the descriptor as an
   `SCM_RIGHTS` control message. No descriptor (`has_fd = 0`, implicit fd =
   -1) means "this segment uses the /dev/shm fallback".
5. `read()`/`read_rows()` look the name up in a process-wide handoff registry
   (populated by `Ring_start`, drained by the read path), map the received fd,
   close it (the mapping holds the file reference) and cache the result.
   Subsequent reads take one non-blocking poll (to notice a replacement) and
   return the cached mapping.

The record is sent **before** the worker posts its response, so by the time
the client's `wait()` returns, the descriptor is already in the socket buffer.
The read path's first-use wait is bounded (1 s) so an old server or a broken
handoff degrades to `/dev/shm` instead of hanging.

Why a socket rather than `/proc/<pid>/fd/<n>`? The worker is a PostgreSQL
background worker; `/proc` access depends on the server's dumpable/ptrace
policy, and a descriptor passed over a socket the client created is a real
capability transfer rather than a path lookup that a third process could
race. The abstract socket is reachable only within the same network namespace;
the connection is established from the worker whose pid the client then sees
in `tally.worker_pid`.

## Sealing choice

`F_SEAL_SHRINK | F_SEAL_GROW` are applied after the single `ftruncate`:

- the object can never change size under a mapped reader, and
- it can never be grown into a different shape (the reader's `capacity` and
  the descriptor's size agree forever).

`F_SEAL_WRITE` is **not** used, deliberately. The ring recycles one segment per
batch: the worker rewrites the same mapping for every `next()`. A write seal
would make the already-writable shared mapping fault on the next publish (and
`F_ADD_SEALS(F_SEAL_WRITE)` fails while a writable shared mapping exists), so
write sealing and batch recycling are mutually exclusive in this design. The
readers are protected by the size seals and by the existing publish/sequence
protocol, not by copy-on-write.

Consequence: a segment name's capacity is fixed for the life of the worker.
`ring_data_capacity()` already clamps it per ring, so this is the behavior
callers ask for. If a larger ask ever arrives, the worker replaces the memfd
and sends a fresh descriptor; the client notices the new generation on its
next read and remaps.

## Lifetime

- **memfd descriptor (worker)**: created on first use of a segment name, held
  in the worker's segment table (up to 8 names), closed at `worker_fini` after
  the final unmap.
- **memfd mapping (worker)**: held for the worker's lifetime; no per-batch
  `munmap`. Unmapped in `fastpostgres_shm_worker_end()`.
- **client descriptor**: received once, mapped, then closed; the `mmap`
  reference keeps the memfd alive even if the worker exits.
- **client mapping**: process-lifetime cache, keyed by segment name. It is
  replaced if a new descriptor/generation arrives for the same name (only a
  capacity replacement in practice).
- **socket**: listener closed at the first `accept`; accepted connection owned
  by the `Ring` and closed on `Ring.stop()`/deallocation.
- **ring segment** (the control channel) is still `/dev/shm/<ring>` at 0600 as
  before; this slice concerns the data segment only.

## Security delta

| | /dev/shm path (default, fallback) | memfd path (`FASTPOSTGRES_MEMFD=1`) |
|---|---|---|
| name on a shared filesystem | yes (`/dev/shm/<seg>`) | no |
| inode permission | widened to `0666` so another user can open it | anonymous; no path, no mode |
| who can reach the payload | any local process that can open the name | any process that can connect to the client's abstract listener in the same netns — the worker connects before `worker_ready`, and the fd is sent to that one connection |
| resize under a reader | possible via `ftruncate` by anyone who opens the file | impossible (`F_SEAL_SHRINK|F_SEAL_GROW`) |
| segment identity | name | descriptor |

The `fchmod(0666)` call is gone from the memfd path entirely. The fallback
path keeps it, because a client running as a different user than the server
still has to reach a named object; that is why memfd is opt-in rather than the
default. The control ring remains 0600 as it already was.

Remaining gap (documented, not fixed here): the abstract listener is open to
any process in the same network namespace while `Ring_start` is in flight.
A hostile local process could connect first and receive a descriptor instead
of the worker (denial of service, and a copy of one result segment). Closing
that would mean authenticating the peer (`SO_PEERCRED` against the server's
uid/pids) or using a filesystem socket under a private 0700 directory; both
are small follow-ups.

## Fallback and compatibility

- No `FASTPOSTGRES_MEMFD` (default) or an explicit `0`: the client does not
  listen, `fd_handoff = 0`, and the worker takes exactly the old `/dev/shm`
  path (including `fchmod`). Existing tests that inspect
  `/dev/shm/<seg>` pass unchanged.
- `memfd_create` fails (`ENOSYS`, fd limit): the worker sends `has_fd = 0` and
  creates the named segment; the client maps `/dev/shm/<seg>`.
- Socket connect/accept fails, or an old server ignores the handoff fields:
  the worker uses `/dev/shm`; the client's bounded first-use wait expires and
  it maps by name.
- The shm fallback also benefits from the mapping cache (one `mmap` per
  segment name per worker instead of one per batch).

## Measurement

`bench/results/team-formats-ab.log`. Same command on both images, under
`/tmp/fpq-bench.lock`, with `FASTPOSTGRES_MEMFD=1` on both (the baseline
ignores it). Five alternating runs per size; medians:

- 3K `stream()`: 6.05M -> 6.76M rows/s (+11.7%)
- 3K PATHS `stream()`: 6.11M -> 6.52M (+6.7%)
- 3K DBAPI: 7.34M -> 7.79M rows/s (+6.1%)
- 3K core: 4.91M -> 6.03M (+22.8%)
- 100K `stream()`: 8.38M -> 8.71M rows/s (+3.9%)
- 100K DBAPI: 3.64M -> 3.64M rows/s (0.0%)

The small-batch read (where a per-request map/unmap was a large share of the
work) is faster in every fastpostgres path; the 100K read is unchanged within
the benchmark's process-to-process spread.

## What is left

- The default is still the named segment (tests and cross-user clients depend
  on it); flipping the default needs the `/dev/shm`-inspecting tests to opt
  out explicitly and a decision that same-user client/server is the supported
  deployment.
- Peer authentication on the handoff socket (`SO_PEERCRED`) as noted above.
- A dedicated regression test that asserts the memfd path is used (no data
  file under `/dev/shm`) and that shrinking/growing is refused; the current
  suite runs the fallback path for that check.
