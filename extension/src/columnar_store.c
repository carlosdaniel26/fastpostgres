/* columnar_store.c — bulk-load a read-only SELECT into a sealed memfd.
 *
 * This is the storage half of H9's surviving branch.  PostgreSQL's shared
 * buffers are anonymous mappings with no descriptor, so a lease of heap pages is
 * impossible; what *is* possible is a store that is born columnar.  A query is
 * run once, every value is written into its column's run in the FPCOLUMN layout,
 * and the result is a sealed, immutable memfd.  The client maps that memfd
 * read-only and walks the columns where they lie: no server round trip, no
 * transpose, no per-row copy, no scratch.
 *
 * This is deliberately not a table AM.  The object has no catalog entry, no
 * relfilenode, no MVCC metadata and no tuple headers; it is a snapshot produced
 * by one query, held by the backend that produced it.  DESIGN.md records what
 * the missing pieces would cost.
 *
 * The descriptor is returned to the caller as
 *
 *     FPQCOL1 <backend pid> <fd> <bytes> <rows>
 *
 * That string is unchanged: it still names the backend and the descriptor in
 * that backend's table, so a client that cannot take a live handoff can still
 * open /proc/<pid>/fd/<fd>.  The preferred delivery is an AF_UNIX control
 * socket: the optional third argument is the client's listening address, and
 * the descriptor travels over it with SCM_RIGHTS.  The client then holds its
 * own reference to the sealed object and never needs /proc, the backend's UID,
 * or ptrace-read permission.  The backend keeps its fd in a small
 * process-local table too (the descriptor above stays honest), but the client's
 * capability is the received descriptor, not a lookup by pid.
 *
 * The fd stays open in a small process-local table for the backend's lifetime:
 * PostgreSQL closes it on exit, and until then every attached client mapping
 * points at the same immutable pages.  The mapping itself keeps the object
 * alive even after the backend goes away.
 */
#include "postgres.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "executor/spi.h"
#include "miscadmin.h"
#include "utils/builtins.h"

#include "fastpostgres/fastpostgres_shm.h"

#include "columnar.h"

/* How many loaded tables one backend keeps open.  The fds are the only handle
 * the client has, so they live until the process exits; a small fixed table is
 * enough for a prototype and makes the leak visible instead of unbounded. */
#define FASTPOSTGRES_COLUMNAR_MAX_LOADS 64

/* A bigger-than-1-GiB snapshot is possible, but it is a deliberate ask: the
 * caller passes the capacity, and we refuse absurd sizes rather than size a
 * virtual mapping that cannot be backed. */
#define FASTPOSTGRES_COLUMNAR_MAX_BYTES ((uint64)16 * 1024 * 1024 * 1024)

typedef struct LoadedColumnar
{
    int    fd;
    uint64 bytes;
    uint64 rows;
} LoadedColumnar;

static LoadedColumnar loaded[FASTPOSTGRES_COLUMNAR_MAX_LOADS];
static int            nloaded;

/* One control-socket record: the descriptor rides in the SCM_RIGHTS ancillary
 * message, these fields say which object it is.  SOCK_SEQPACKET preserves the
 * record boundary, so one recvmsg on the client sees exactly this send. */
typedef struct ColumnarHandoff
{
    uint32_t magic; /* FASTPOSTGRES_COLUMNAR_HANDOFF_MAGIC */
    int32_t  pid;   /* the backend that owns the memfd (SO_PEERCRED agrees) */
    uint64_t bytes;
    uint64_t rows;
} ColumnarHandoff;

#define FASTPOSTGRES_COLUMNAR_HANDOFF_MAGIC 0x46514348u /* "FQCH" */

/*
 * Deliver the sealed memfd over the client's control socket.  `path` is the
 * abstract address the client published (leading '@' stands for the sun_path
 * NUL) or a filesystem socket path.  A failure is never fatal: the descriptor
 * string still works through /proc for a same-UID client, so the load succeeds
 * and only the capability transfer fell back.
 */
static void
columnar_handoff_send(const char *path, int fd, uint64 bytes, uint64 rows)
{
    struct sockaddr_un addr;
    socklen_t          addrlen;
    struct msghdr      msg;
    struct iovec       iov;
    char               control[CMSG_SPACE(sizeof(int))];
    ColumnarHandoff    hdr;
    ssize_t            n;
    int                s;

    if (path == NULL || path[0] == 0) return;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (path[0] == '@')
    {
        size_t len = strlen(path + 1);

        if (len == 0 || len >= sizeof(addr.sun_path) - 1)
        {
            elog(WARNING, "fastpostgres: columnar handoff address is too long");
            return;
        }
        /* Linux abstract namespace: the first sun_path byte is NUL and the
         * name occupies the rest; there is no filesystem object to clean up. */
        addr.sun_path[0] = '\0';
        memcpy(addr.sun_path + 1, path + 1, len);
        addrlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + len);
    }
    else
    {
        if (strlen(path) >= sizeof(addr.sun_path))
        {
            elog(WARNING, "fastpostgres: columnar handoff address is too long");
            return;
        }
        memcpy(addr.sun_path, path, strlen(path) + 1);
        addrlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1);
    }

    s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (s < 0)
    {
        elog(WARNING, "fastpostgres: columnar handoff socket failed: %m");
        return;
    }
    if (connect(s, (struct sockaddr *)&addr, addrlen) != 0)
    {
        int save_errno = errno;

        close(s);
        errno = save_errno;
        elog(WARNING, "fastpostgres: columnar handoff connect failed: %m");
        return;
    }

    hdr.magic = FASTPOSTGRES_COLUMNAR_HANDOFF_MAGIC;
    hdr.pid   = (int32_t)MyProcPid;
    hdr.bytes = bytes;
    hdr.rows  = rows;

    memset(&msg, 0, sizeof(msg));
    memset(control, 0, sizeof(control));
    iov.iov_base    = &hdr;
    iov.iov_len     = sizeof(hdr);
    msg.msg_iov     = &iov;
    msg.msg_iovlen  = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    {
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);

        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type  = SCM_RIGHTS;
        c->cmsg_len   = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }

    /* MSG_NOSIGNAL: a client that gave up must not SIGPIPE the backend. */
    do
        n = sendmsg(s, &msg, MSG_NOSIGNAL);
    while (n < 0 && errno == EINTR);

    if (n < 0)
        elog(WARNING, "fastpostgres: columnar handoff send failed: %m");
    close(s);
}

static void
remember_fd(int fd, uint64 bytes, uint64 rows)
{
    if (nloaded >= FASTPOSTGRES_COLUMNAR_MAX_LOADS)
    {
        close(fd);
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("fastpostgres: more than %d loaded columnar tables in this backend",
                        FASTPOSTGRES_COLUMNAR_MAX_LOADS)));
    }
    loaded[nloaded].fd    = fd;
    loaded[nloaded].bytes = bytes;
    loaded[nloaded].rows  = rows;
    nloaded++;
}

/*
 * fastpostgres_columnar_load(query text, capacity bigint DEFAULT 0,
 *                            handoff text DEFAULT NULL) -> text
 *
 * Runs `query` once, read-only, and materialises its rows into a fresh memfd in
 * the FPCOLUMN layout.  Returns the descriptor above.  `capacity` is the
 * virtual size of the memfd (0 = the 1 GiB segment default); it is truncated to
 * the exact payload before the seals go on, so only the used bytes survive.
 * `handoff` names the client's control socket; when it is given the sealed
 * descriptor is delivered over it with SCM_RIGHTS.  Omitting it is the original
 * /proc path, byte for byte.
 */
PG_FUNCTION_INFO_V1(fastpostgres_columnar_load);
Datum
fastpostgres_columnar_load(PG_FUNCTION_ARGS)
{
    char     *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char     *handoff = NULL;
    int64     cap_arg = 0;
    uint64    cap;
    uint64    used;
    uint64    nrows;
    int       rc;
    int       fd = -1;
    void     *map = NULL;
    TupleDesc td;
    HeapTuple *tuples;
    char     *descriptor;

    if (PG_NARGS() > 1 && !PG_ARGISNULL(1)) cap_arg = PG_GETARG_INT64(1);
    if (PG_NARGS() > 2 && !PG_ARGISNULL(2)) handoff = text_to_cstring(PG_GETARG_TEXT_PP(2));

    if (cap_arg == 0)
        cap = FASTPOSTGRES_SEGMENT_BYTES;
    else if (cap_arg < (int64)FASTPOSTGRES_SEGMENT_MIN)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("fastpostgres: columnar capacity must be 0 or at least %u bytes",
                        FASTPOSTGRES_SEGMENT_MIN)));
    else if ((uint64)cap_arg > FASTPOSTGRES_COLUMNAR_MAX_BYTES)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("fastpostgres: columnar capacity is capped at %llu bytes",
                        (unsigned long long)FASTPOSTGRES_COLUMNAR_MAX_BYTES)));
    else
        cap = (uint64)cap_arg;

    /* A memfd is the object: anonymous, sealable, and not reachable by path.
     * MFD_CLOEXEC because no child of this backend has any business inheriting
     * the table. */
    fd = memfd_create("fastpostgres-columnar", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        ereport(ERROR, (errcode_for_file_access(),
                        errmsg("fastpostgres: memfd_create failed: %m")));

    if (ftruncate(fd, (off_t)cap) != 0)
    {
        int save_errno = errno;

        close(fd);
        errno = save_errno;
        ereport(ERROR, (errcode_for_file_access(),
                        errmsg("fastpostgres: ftruncate of columnar memfd failed: %m")));
    }

    map = mmap(NULL, (size_t)cap, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED)
    {
        int save_errno = errno;

        close(fd);
        errno = save_errno;
        ereport(ERROR, (errcode_for_file_access(),
                        errmsg("fastpostgres: mmap of columnar memfd failed: %m")));
    }
    (void)madvise(map, (size_t)cap, MADV_HUGEPAGE); /* before the first write */

    if (SPI_connect() != SPI_OK_CONNECT)
    {
        munmap(map, (size_t)cap);
        close(fd);
        elog(ERROR, "fastpostgres_columnar_load: SPI_connect failed");
    }

    PG_TRY();
    {
        rc = SPI_execute(query, true, 0);
        if (rc != SPI_OK_SELECT || SPI_tuptable == NULL)
            elog(ERROR, "fastpostgres_columnar_load: query is not a read-only SELECT (SPI code %d)",
                 rc);

        nrows  = SPI_processed;
        td     = SPI_tuptable->tupdesc;
        tuples = SPI_tuptable->vals;

        /* One pass, straight into the column regions: the memfd is the buffer,
         * not a copy destination for a scratch frame. */
        used = fastpostgres_columnar_build(tuples, td, nrows, (char *)map, (char *)map + cap);
    }
    PG_CATCH();
    {
        /* The transaction abort frees SPI's memory; the descriptor and the
         * mapping are ours to release, or a failing load would leak one of
         * each for the life of the backend. */
        munmap(map, (size_t)cap);
        close(fd);
        PG_RE_THROW();
    }
    PG_END_TRY();

    SPI_finish();

    /* The payload is exact now.  Shrink before sealing: a reader maps only
     * `used`, and a later ftruncate cannot move the object under it. */
    if (ftruncate(fd, (off_t)used) != 0)
    {
        int save_errno = errno;

        munmap(map, (size_t)cap);
        close(fd);
        errno = save_errno;
        ereport(ERROR, (errcode_for_file_access(),
                        errmsg("fastpostgres: ftruncate of columnar payload failed: %m")));
    }
    /* Drop the last writable mapping before sealing: F_SEAL_WRITE refuses to
     * attach while one exists.  After this the object is immutable to everyone,
     * including a same-UID process that finds the descriptor. */
    munmap(map, (size_t)cap);
    map = NULL;

    if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) != 0)
        elog(WARNING, "fastpostgres: F_ADD_SEALS on columnar memfd failed: %m");

    remember_fd(fd, used, nrows);

    /* The capability, when the client asked for one: the sealed descriptor
     * itself, over the control socket, with the identity the client checks
     * against SO_PEERCRED.  A failure only costs the /proc fallback. */
    if (handoff != NULL)
        columnar_handoff_send(handoff, fd, used, nrows);

    descriptor = psprintf("FPQCOL1 %d %d %llu %llu", (int)MyProcPid, fd,
                          (unsigned long long)used, (unsigned long long)nrows);
    PG_RETURN_TEXT_P(cstring_to_text(descriptor));
}
