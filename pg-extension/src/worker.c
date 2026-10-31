/*
 * worker.c — a dynamically registered background worker that services the
 * request/response ring in /dev/shm, so the client's shm path needs no socket.
 *
 * Bootstrap: the client creates /dev/shm/<seg> (initialising the ring's
 * semaphores), calls fastpostgres_ring_start(seg) once over libpq, then talks to this
 * worker only through the ring.
 *
 * SECURITY: the ring is an SQL-execution endpoint. Keep /dev/shm/<seg> mode
 * 0600 owned by the postgres user; any process running as that user can execute
 * SQL through it. Not for production.
 */
#include "postgres.h"

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "access/xact.h"
#include "commands/dbcommands.h"
#include "fmgr.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"

#include "fastpostgres_shm.h"

#include "columnar.h"
#include "ring.h"
#include "stream.h"
#include "write.h"

#define FASTPOSTGRES_RING_WAIT_MS 100 /* only so the worker notices SIGTERM */

static volatile sig_atomic_t got_sigterm   = false;
static bool                  have_txn      = false;
static bool                  have_stream   = false;
static bool                  have_snapshot = false;

static void ring_sigterm(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sigterm    = true;
    errno          = save_errno;
}

static FastpostgresRing *ring_map_rw(const char *seg, size_t *size_out)
{
    char        path[256];
    int         fd;
    struct stat st;
    void       *map;

    snprintf(path, sizeof(path), "/%s", seg);
    fd = shm_open(path, O_RDWR, 0);
    if (fd < 0) return NULL;
    if (fstat(fd, &st) != 0)
    {
        close(fd);
        return NULL;
    }
    map = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return NULL;
    if ((size_t)st.st_size < sizeof(FastpostgresRing) ||
        memcmp(((FastpostgresRing *)map)->magic, FASTPOSTGRES_RING_MAGIC, 8) != 0)
    {
        munmap(map, (size_t)st.st_size);
        return NULL;
    }
    *size_out = (size_t)st.st_size;
    return (FastpostgresRing *)map;
}

static void abort_txn(void)
{
    if (have_txn || IsTransactionState()) AbortCurrentTransaction();
    /* AbortCurrentTransaction resets the snapshot stack */
    have_txn      = false;
    have_stream   = false;
    have_snapshot = false;
}

static void commit_txn(void)
{
    if (have_snapshot)
    {
        PopActiveSnapshot();
        have_snapshot = false;
    }
    if (have_txn) CommitTransactionCommand();
    have_txn    = false;
    have_stream = false;
}

/* A bgworker has no active snapshot; SPI_cursor_open (read-only) needs one. */
static void begin_txn(void)
{
    abort_txn();
    StartTransactionCommand();
    have_txn = true;
    PushActiveSnapshot(GetTransactionSnapshot());
    have_snapshot = true;
}

/* Copy the pending error into the ring (must be called inside PG_CATCH). */
static void ring_set_error(FastpostgresRing *r)
{
    ErrorData *edata = CopyErrorData();

    r->status = 1;
    strlcpy(r->error, edata->message ? edata->message : "error", sizeof(r->error));
    FlushErrorState();
    FreeErrorData(edata);
}

/* Returns false when the worker should stop. */
static bool ring_handle(FastpostgresRing *r, const char *ring_name)
{
    char  *a1 = (char *)fastpostgres_ring_arg1(r->arg);
    char  *a2 = (char *)fastpostgres_ring_arg2(r->arg);
    uint64 n  = 0;

    /* never let a data write clobber the ring itself */
    if ((r->op == FASTPOSTGRES_OP_OPEN || r->op == FASTPOSTGRES_OP_NEXT || r->op == FASTPOSTGRES_OP_NEXT_ROWS ||
         r->op == FASTPOSTGRES_OP_CLOSE || r->op == FASTPOSTGRES_OP_WRITE) &&
        strcmp(a1, ring_name) == 0)
    {
        r->status = 1;
        strlcpy(r->error, "data segment must differ from the ring", sizeof(r->error));
        return true;
    }

    switch (r->op)
    {
        case FASTPOSTGRES_OP_STOP:
            return false;

        case FASTPOSTGRES_OP_OPEN:
            begin_txn(); /* drop any previous stream and open a new transaction */
            PG_TRY();
            {
                fastpostgres_stream_open(a1, a2);
                have_stream = true;
                r->status   = 0;
            }
            PG_CATCH();
            {
                ring_set_error(r);
                abort_txn();
            }
            PG_END_TRY();
            break;

        case FASTPOSTGRES_OP_NEXT:
            if (!have_stream)
            {
                r->status = 1;
                strlcpy(r->error, "no open stream", sizeof(r->error));
                break;
            }
            PG_TRY();
            {
                n         = fastpostgres_stream_next(a1, (int64)r->batch);
                r->status = 0;
                r->nrows  = (int64)n;
            }
            PG_CATCH();
            {
                ring_set_error(r);
                abort_txn();
            }
            PG_END_TRY();
            break;

        case FASTPOSTGRES_OP_NEXT_ROWS:
            if (!have_stream)
            {
                r->status = 1;
                strlcpy(r->error, "no open stream", sizeof(r->error));
                break;
            }
            PG_TRY();
            {
                n         = fastpostgres_stream_next_rows(a1, (int64)r->batch);
                r->status = 0;
                r->nrows  = (int64)n;
            }
            PG_CATCH();
            {
                ring_set_error(r);
                abort_txn();
            }
            PG_END_TRY();
            break;

        case FASTPOSTGRES_OP_CLOSE:
            PG_TRY();
            {
                fastpostgres_stream_close(a1);
                r->status = 0;
                commit_txn();
            }
            PG_CATCH();
            {
                ring_set_error(r);
                abort_txn();
            }
            PG_END_TRY();
            break;

        case FASTPOSTGRES_OP_WRITE:
            begin_txn();
            PG_TRY();
            {
                n         = fastpostgres_write_segment(a1, a2);
                r->status = 0;
                r->nrows  = (int64)n;
                commit_txn();
            }
            PG_CATCH();
            {
                ring_set_error(r);
                abort_txn();
            }
            PG_END_TRY();
            break;

        default:
            r->status = 1;
            strlcpy(r->error, "unknown op", sizeof(r->error));
            break;
    }
    return true;
}

/* The worker entry point (looked up by name, so it must be exported). */
PGDLLEXPORT void fastpostgres_ring_main(Datum main_arg);

PGDLLEXPORT void fastpostgres_ring_main(Datum main_arg)
{
    BackgroundWorker *bw  = MyBgworkerEntry;
    const char       *seg = bw->bgw_extra; /* "ring\0dbname\0" */
    const char       *db  = seg + strlen(seg) + 1;
    FastpostgresRing *r;
    size_t            rsize;
    uint64_t          handled = 0;

    (void)main_arg;

    pqsignal(SIGTERM, ring_sigterm);
    BackgroundWorkerUnblockSignals();

    BackgroundWorkerInitializeConnection(db[0] ? db : "postgres", NULL, 0);

    r = ring_map_rw(seg, &rsize);
    if (r == NULL)
    {
        elog(LOG, "fastpostgres_ring_main: cannot map ring \"%s\"", seg);
        proc_exit(1);
    }
    __atomic_store_n(&r->worker_ready, 1, __ATOMIC_RELEASE);

    for (;;)
    {
        struct timespec ts;
        int             rc;
        uint64_t        seq;

        if (got_sigterm) break;

        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += FASTPOSTGRES_RING_WAIT_MS * 1000000L;
        if (ts.tv_nsec >= 1000000000L)
        {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }

        rc = sem_timedwait(&r->req_ready, &ts);
        if (rc != 0)
        {
            /* timeout or EINTR: re-check SIGTERM, and whether the client that
             * owns this ring is gone (then there is nobody to serve) */
            if (r->client_pid != 0 && kill((pid_t)r->client_pid, 0) != 0 && errno == ESRCH) break;
            continue;
        }

        seq = __atomic_load_n(&r->req_seq, __ATOMIC_ACQUIRE);
        if (seq == handled) continue;
        handled = seq;

        r->status   = 0;
        r->nrows    = 0;
        r->error[0] = 0;

        if (!ring_handle(r, seg)) break;
        __atomic_store_n(&r->resp_seq, handled, __ATOMIC_RELEASE);
        sem_post(&r->resp_ready);
    }

    abort_txn();
    __atomic_store_n(&r->worker_ready, 0, __ATOMIC_RELEASE);
    munmap(r, rsize);
    proc_exit(0);
}

/* ---- SQL: start / stop --------------------------------------------------- */

PG_FUNCTION_INFO_V1(fastpostgres_ring_start);
Datum fastpostgres_ring_start(PG_FUNCTION_ARGS)
{
    char                   *seg = text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char             *db  = get_database_name(MyDatabaseId);
    BackgroundWorker        w;
    BackgroundWorkerHandle *handle;
    size_t                  slen;
    char                    summary[256];

    memset(&w, 0, sizeof(w));
    snprintf(w.bgw_name, BGW_MAXLEN, "fastpostgres ring");
    snprintf(w.bgw_type, BGW_MAXLEN, "fastpostgres ring worker");
    w.bgw_flags        = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    w.bgw_start_time   = BgWorkerStart_RecoveryFinished;
    w.bgw_restart_time = BGW_NEVER_RESTART;
    snprintf(w.bgw_library_name, BGW_MAXLEN, "fastpostgres");
    snprintf(w.bgw_function_name, BGW_MAXLEN, "fastpostgres_ring_main");
    w.bgw_notify_pid = MyProcPid;

    memset(w.bgw_extra, 0, BGW_EXTRALEN);
    slen = strlen(seg);
    if (slen + strlen(db) + 2 > BGW_EXTRALEN) elog(ERROR, "fastpostgres_ring_start: ring name too long");
    memcpy(w.bgw_extra, seg, slen + 1);                 /* ring name */
    memcpy(w.bgw_extra + slen + 1, db, strlen(db) + 1); /* database name */

    if (!RegisterDynamicBackgroundWorker(&w, &handle))
        elog(ERROR, "fastpostgres_ring_start: could not register background worker");

    snprintf(summary, sizeof(summary), "ring=%s started", seg);
    PG_RETURN_TEXT_P(cstring_to_text(summary));
}

PG_FUNCTION_INFO_V1(fastpostgres_ring_stop);
Datum fastpostgres_ring_stop(PG_FUNCTION_ARGS)
{
    char             *seg = text_to_cstring(PG_GETARG_TEXT_PP(0));
    size_t            rsize;
    FastpostgresRing *r = ring_map_rw(seg, &rsize);
    char              summary[256];

    if (r != NULL)
    {
        r->op = FASTPOSTGRES_OP_STOP;
        __atomic_add_fetch(&r->req_seq, 1, __ATOMIC_RELEASE);
        sem_post(&r->req_ready);
        munmap(r, rsize);
    }

    snprintf(summary, sizeof(summary), "ring=%s stopping", seg);
    PG_RETURN_TEXT_P(cstring_to_text(summary));
}
