/*
 * worker.c — a dynamically registered background worker that services the
 * request/response ring in /dev/shm, so the client's shm path needs no socket.
 *
 * Bootstrap: the client creates /dev/shm/<seg> (initialising the ring's
 * semaphores), calls fastpostgres_ring_start(seg) once over libpq, then talks to this
 * worker only through the ring.
 *
 * SECURITY: the ring is an SQL-execution endpoint — any local process that can
 * open /dev/shm/<seg> can execute SQL through it. The client creates the segment
 * 0600; fastpostgres_shm_map_rw widens it to 0666 (shm.c) so a client running as
 * a different user can map it. Keep the segment name out of shared trees and
 * treat the segment itself as the boundary. Not for production.
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
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "fastpostgres/fastpostgres_shm.h"

#include "columnar.h"
#include "fastpostgres/ring.h"
#include "shm.h"
#include "stream.h"
#include "transport/transport.h"

#define FASTPOSTGRES_RING_WAIT_MS 100 /* only so the worker notices SIGTERM */

static volatile sig_atomic_t got_sigterm   = false;
static bool                  have_txn      = false;
static bool                  have_stream   = false;
static bool                  have_snapshot = false;

static void
ring_sigterm(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sigterm    = true;
    errno          = save_errno;
}

static FastpostgresRing *
ring_map_rw(const char *seg, size_t *size_out)
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

static void
abort_txn(void)
{
    if (have_txn || IsTransactionState()) AbortCurrentTransaction();
    /* AbortCurrentTransaction resets the snapshot stack */
    have_txn      = false;
    have_stream   = false;
    have_snapshot = false;
}

static void
commit_txn(void)
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
static void
begin_txn(void)
{
    abort_txn();
    StartTransactionCommand();
    have_txn = true;
    PushActiveSnapshot(GetTransactionSnapshot());
    have_snapshot = true;
}

/* Copy the pending error into the ring (must be called inside PG_CATCH).
 *
 * errfinish leaves CurrentMemoryContext in ErrorContext, and CopyErrorData
 * allocates in whatever context is current; FlushErrorState then resets
 * ErrorContext. Copying there would hand FreeErrorData a dangling pointer, so
 * the copy is made in TopMemoryContext and freed before the handler resumes. */
static void
ring_set_error(FastpostgresRing *r)
{
    MemoryContext oldcontext = MemoryContextSwitchTo(TopMemoryContext);
    ErrorData  *edata        = CopyErrorData();

    r->status = 1;
    strlcpy(r->error, edata->message ? edata->message : "error", sizeof(r->error));
    FlushErrorState();
    MemoryContextSwitchTo(oldcontext);
    FreeErrorData(edata);
}

/* How big the client asked for a data segment to be, clamped to something that
 * can hold a frame. Read from the ring per request: the client writes it before
 * starting the worker. */
static size_t
ring_data_capacity(const FastpostgresRing *r)
{
    uint64_t want = r->data_bytes;

    if (want == 0) return FASTPOSTGRES_SEGMENT_BYTES;
    if (want < FASTPOSTGRES_SEGMENT_MIN) return FASTPOSTGRES_SEGMENT_MIN;
    if (want > FASTPOSTGRES_SEGMENT_BYTES) return FASTPOSTGRES_SEGMENT_BYTES;
    return (size_t)want;
}

/* Returns false when the worker should stop. */
static bool
ring_handle(FastpostgresRing *r, const char *ring_name)
{
    char       *a1 = (char *)fastpostgres_ring_arg1(r->arg);
    char       *a2 = (char *)fastpostgres_ring_arg2(r->arg);
    const char *data;
    uint64      n = 0;
    /* The pre-request context: errfinish leaves CurrentMemoryContext in
     * ErrorContext, and a transaction context dies with its abort, so handlers
     * resume here instead of either. */
    MemoryContext oldcontext = CurrentMemoryContext;

    /* never let a batch clobber the ring itself: it is published into the segment
     * the NEXT names. REDUCE names its data segment in a1 and its plan in a2,
     * because the plan is the request payload and the segment is the answer. */
    if (r->op == FASTPOSTGRES_OP_NEXT || r->op == FASTPOSTGRES_OP_NEXT_ROWS)
        data = a2;
    else if (r->op == FASTPOSTGRES_OP_REDUCE)
        data = a1;
    else
        data = NULL;

    if (data != NULL && strcmp(data, ring_name) == 0)
    {
        r->status = 1;
        strlcpy(r->error, "data segment must differ from the ring", sizeof(r->error));
        return true;
    }
    if (data != NULL && data[0] == 0)
    {
        r->status = 1;
        strlcpy(r->error, "next needs a segment", sizeof(r->error));
        return true;
    }

    switch (r->op)
    {
        case FASTPOSTGRES_OP_STOP:
            return false;

        case FASTPOSTGRES_OP_OPEN:
            /* the stream's data segments are named by its batches: a mapping
             * from the last stream must not outlive it */
            fastpostgres_shm_cache_fini();
            begin_txn(); /* drop any previous stream and open a new transaction */
            PG_TRY();
            {
                fastpostgres_stream_open(a1, a2);
                have_stream = true;
                r->status   = 0;
            }
            PG_CATCH();
            {
                MemoryContextSwitchTo(oldcontext);
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
                n         = fastpostgres_stream_next(a1, a2, (int64)r->batch, ring_data_capacity(r));
                r->status = 0;
                r->nrows  = (int64)n;
            }
            PG_CATCH();
            {
                MemoryContextSwitchTo(oldcontext);
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
                n         = fastpostgres_stream_next_rows(a1, a2, (int64)r->batch, ring_data_capacity(r));
                r->status = 0;
                r->nrows  = (int64)n;
            }
            PG_CATCH();
            {
                MemoryContextSwitchTo(oldcontext);
                ring_set_error(r);
                abort_txn();
            }
            PG_END_TRY();
            break;

        case FASTPOSTGRES_OP_REDUCE:
            if (!have_stream)
            {
                r->status = 1;
                strlcpy(r->error, "no open stream", sizeof(r->error));
                break;
            }
            PG_TRY();
            {
                /* a1 is both the stream and the data segment (the pool names them
                 * alike); a2 is the plan. */
                n = fastpostgres_stream_next_reduce(a1, a1, (int64)r->batch, ring_data_capacity(r), a2);
                r->status = 0;
                r->nrows  = (int64)n;
            }
            PG_CATCH();
            {
                MemoryContextSwitchTo(oldcontext);
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
                MemoryContextSwitchTo(oldcontext);
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

PGDLLEXPORT void
fastpostgres_ring_main(Datum main_arg)
{
    BackgroundWorker            *bw  = MyBgworkerEntry;
    const char                  *seg = bw->bgw_extra; /* "ring\0dbname\0transport\0" */
    const char                  *db  = seg + strlen(seg) + 1;
    const char                  *tname;
    const FastpostgresTransport *t;
    FastpostgresRing            *r;
    size_t                       rsize;

    (void)main_arg;

    pqsignal(SIGTERM, ring_sigterm);
    BackgroundWorkerUnblockSignals();

    /* The third NUL-separated field of bgw_extra names the engine. A ring
     * registered by a server that predates the field carries none, and that
     * is the shm ring it always was. */
    tname = db + strlen(db) + 1;
    if (tname >= bw->bgw_extra + BGW_EXTRALEN) tname = "";
    t = fastpostgres_transport_by_name(tname[0] ? tname : "shm");
    if (t == NULL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("fastpostgres_ring_main: unknown transport \"%s\"", tname)));

    BackgroundWorkerInitializeConnection(db[0] ? db : "postgres", NULL, 0);

    r = ring_map_rw(seg, &rsize);
    if (r == NULL)
    {
        elog(LOG, "fastpostgres_ring_main: cannot map ring \"%s\"", seg);
        proc_exit(1);
    }

    /* Before anything else reads or writes the segment: a ring another
     * version or another engine wrote is refused, never driven. */
    if (fastpostgres_transport_check(r, t) != 0)
    {
        elog(LOG, "fastpostgres_ring_main: ring \"%s\" is not a %s ring", seg, t->name);
        fastpostgres_shm_cache_fini();
        munmap(r, rsize);
        proc_exit(1);
    }

    if (t->worker_init(r) < 0)
    {
        elog(LOG, "fastpostgres_ring_main: %s worker_init failed: %m", t->name);
        fastpostgres_shm_cache_fini();
        munmap(r, rsize);
        proc_exit(1);
    }
    /* The pid first, then ready, both release: a reader that sees ready can
     * name the process serving this ring from the ring itself, which is how the
     * hosts outside read this side's CPU (/proc/<pid>/stat). */
    __atomic_store_n(&r->tally.worker_pid, (uint32_t)getpid(), __ATOMIC_RELEASE);
    __atomic_store_n(&r->worker_ready, 1, __ATOMIC_RELEASE);

    for (;;)
    {
        uint64_t seq;
        int      rc;

        if (got_sigterm) break;

        rc = t->wait_request(r, &seq, FASTPOSTGRES_RING_WAIT_MS);
        if (rc == 0)
        {
            /* Timeout: re-check SIGTERM, the client that owns this ring (gone
             * means there is nobody to serve), and the ring's op. The op check
             * makes a STOP whose doorbell post was missed visible here: the
             * stop path only posts the semaphore. */
            if (r->op == FASTPOSTGRES_OP_STOP) break;
            if (r->client_pid != 0 && kill((pid_t)r->client_pid, 0) != 0 && errno == ESRCH) break;
            continue;
        }
        if (rc < 0)
        {
            /* SIGTERM ends a sem_timedwait with EINTR; that is an exit, not an
             * engine failure worth logging. */
            if (!got_sigterm) elog(LOG, "fastpostgres_ring_main: %s wait_request failed: %m", t->name);
            break;
        }

        r->status   = 0;
        r->nrows    = 0;
        r->error[0] = 0;

        if (!ring_handle(r, seg)) break;
        if (t->post_response(r, seq) < 0)
        {
            elog(LOG, "fastpostgres_ring_main: %s post_response failed: %m", t->name);
            break;
        }
    }

    abort_txn();
    /* the data mappings belong to the segments this worker published into;
     * they must not survive it */
    fastpostgres_shm_cache_fini();
    __atomic_store_n(&r->worker_ready, 0, __ATOMIC_RELEASE);
    t->worker_fini(r);
    munmap(r, rsize);
    proc_exit(0);
}

/* ---- SQL: start / stop --------------------------------------------------- */

PG_FUNCTION_INFO_V1(fastpostgres_ring_start);
Datum
fastpostgres_ring_start(PG_FUNCTION_ARGS)
{
    char                        *seg = text_to_cstring(PG_GETARG_TEXT_PP(0));
    const FastpostgresTransport *t   = &fastpostgres_transport_shm;
    const char                  *db  = get_database_name(MyDatabaseId);
    BackgroundWorker             w;
    BackgroundWorkerHandle      *handle;
    size_t                       slen;
    size_t                       dlen;
    size_t                       tlen;
    size_t                       rsize;
    FastpostgresRing            *r;
    char                         summary[256];

    /* The one-argument form is the shm ring it always was; the two-argument
     * overload names the engine. An unknown name is an error rather than a
     * silent fallback: the caller would otherwise believe it asked for the
     * transport it got. */
    if (PG_NARGS() > 1)
    {
        char *name = text_to_cstring(PG_GETARG_TEXT_PP(1));

        t = fastpostgres_transport_by_name(name);
        if (t == NULL)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("fastpostgres_ring_start: unknown transport \"%s\"", name)));
    }

    /* The ring's transport field is the segment's provenance. A v4 ring made
     * before the field was filled in reads zero and is stamped here; one
     * already stamped for a different engine is refused, because driving it
     * with the wrong doorbell would corrupt the session it is serving. */
    r = ring_map_rw(seg, &rsize);
    if (r != NULL)
    {
        if (r->transport == 0)
            r->transport = t->id;
        else if (r->transport != t->id)
        {
            uint32_t have = r->transport;

            munmap(r, rsize);
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("fastpostgres_ring_start: ring \"%s\" was made for transport %u, not %s",
                                   seg, have, t->name)));
        }
        munmap(r, rsize);
    }

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
    dlen = strlen(db);
    tlen = strlen(t->name);
    if (slen + dlen + tlen + 3 > BGW_EXTRALEN) elog(ERROR, "fastpostgres_ring_start: ring name too long");
    memcpy(w.bgw_extra, seg, slen + 1);                       /* ring name */
    memcpy(w.bgw_extra + slen + 1, db, dlen + 1);             /* database name */
    memcpy(w.bgw_extra + slen + dlen + 2, t->name, tlen + 1); /* transport */

    if (!RegisterDynamicBackgroundWorker(&w, &handle))
        elog(ERROR, "fastpostgres_ring_start: could not register background worker");

    snprintf(summary, sizeof(summary), "ring=%s started", seg);
    PG_RETURN_TEXT_P(cstring_to_text(summary));
}

PG_FUNCTION_INFO_V1(fastpostgres_ring_stop);
Datum
fastpostgres_ring_stop(PG_FUNCTION_ARGS)
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
