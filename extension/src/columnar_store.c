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
 * The client opens /proc/<pid>/fd/<fd> (same user) and maps it.  The fd stays
 * open in a small process-local table for the backend's lifetime: PostgreSQL
 * closes it on exit, and until then every attached client mapping points at the
 * same immutable pages.  The mapping itself keeps the object alive even after
 * the backend goes away.
 */
#include "postgres.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
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
 * fastpostgres_columnar_load(query text, capacity bigint DEFAULT 0) -> text
 *
 * Runs `query` once, read-only, and materialises its rows into a fresh memfd in
 * the FPCOLUMN layout.  Returns the descriptor above.  `capacity` is the
 * virtual size of the memfd (0 = the 1 GiB segment default); it is truncated to
 * the exact payload before the seals go on, so only the used bytes survive.
 */
PG_FUNCTION_INFO_V1(fastpostgres_columnar_load);
Datum
fastpostgres_columnar_load(PG_FUNCTION_ARGS)
{
    char     *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
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
    descriptor = psprintf("FPQCOL1 %d %d %llu %llu", (int)MyProcPid, fd,
                          (unsigned long long)used, (unsigned long long)nrows);
    PG_RETURN_TEXT_P(cstring_to_text(descriptor));
}
