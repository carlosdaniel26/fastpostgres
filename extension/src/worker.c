/*
 * worker.c — a dynamically registered background worker that services the
 * request/response ring in /dev/shm, so the client's shm path needs no socket.
 *
 * Bootstrap: the client creates /dev/shm/<seg> (initialising the ring's
 * semaphores), calls fastpostgres_ring_start(seg) once over libpq, then talks to this
 * worker only through the ring.
 *
 * There are two worker shapes. fastpostgres_ring_main is the original 1:1
 * shape: one worker, one semaphore ring. fastpostgres_pool_main is the eventfd
 * pool: one worker polls several rings and runs their requests one at a time.
 * The SQL bootstrap picks by the ring's transport (section "the eventfd pool"
 * below), giving a ring its own worker while max_worker_processes has a slot
 * and multiplexing it onto an existing one only when the budget is spent.
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
#include <poll.h>
#include <semaphore.h>
#include <signal.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
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

#include "fastpostgres/fastpostgres_shm.h"

#include "columnar.h"
#include "fastpostgres/ring.h"
#include "rows.h"
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

/* Copy the pending error into the ring (must be called inside PG_CATCH). */
static void
ring_set_error(FastpostgresRing *r)
{
    ErrorData *edata = CopyErrorData();

    r->status = 1;
    strlcpy(r->error, edata->message ? edata->message : "error", sizeof(r->error));
    FlushErrorState();
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

    /* never let a batch clobber the ring itself: it is published into the segment
     * the NEXT names */
    if (r->op == FASTPOSTGRES_OP_NEXT || r->op == FASTPOSTGRES_OP_NEXT_ROWS)
        data = a2;
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
                n         = fastpostgres_stream_next(a1, a2, (int64)r->batch, ring_data_capacity(r));
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
                n         = fastpostgres_stream_next_rows(a1, a2, (int64)r->batch, ring_data_capacity(r));
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

        default:
            r->status = 1;
            strlcpy(r->error, "unknown op", sizeof(r->error));
            break;
    }
    return true;
}

/* ---- the eventfd pool: many rings, few workers ---------------------------
 *
 * One ring used to mean one background worker, so the server's
 * max_worker_processes (8 by default, one held back) capped concurrent
 * shared-memory reads at seven rings. A worker is a full backend, but serving
 * a ring is mostly waiting for a doorbell; with eventfds one worker can poll
 * every ring it serves and run the requests one at a time, which is enough
 * because the executor's turn is ~25% of a read (bench/NOTES.md).
 *
 * The registry is a small /dev/shm file per database (a worker is connected to
 * one database). The SQL bootstrap appends a ring to a worker slot, or creates
 * a slot and registers a worker; the worker maps every ring in its slot,
 * receives the client's eventfds over SCM_RIGHTS, and publishes worker_ready.
 * It is a rendezvous, not a data path: the request slot, the counters and the
 * data segment are the same ring they always were.
 *
 * The cost of the decoupling is honest and measured in DESIGN.md: one backend
 * is one OS thread, so the rings a worker serves serialize their executor
 * turns, and they share one transaction and one snapshot (a read-only stream
 * already lived in one transaction for its whole life).
 */
#define FP_POOL_MAGIC         "FPPOOL01"
#define FP_POOL_VERSION       1u
#define FP_POOL_MAX_WORKERS   8  /* slots in the registry */
#define FP_POOL_MAX_RINGS     32 /* rings one worker will serve */
#define FP_POOL_DEFAULT_RINGS 8  /* FASTPOSTGRES_RINGS_PER_WORKER */
#define FP_POOL_EXIT_MS       2000
#define FP_POOL_START_GRACE_MS 30000

typedef struct FpPoolSlot
{
    int32_t  pid;      /* 0 while starting, then the worker's pid */
    uint32_t state;    /* 0 free, 1 starting, 2 running */
    uint32_t capacity; /* rings this worker will serve */
    uint32_t used;     /* rings listed in segs[] */
    int64_t  started_ms;
    char     segs[FP_POOL_MAX_RINGS][64];
} FpPoolSlot;

typedef struct FpPoolRegistry
{
    char       magic[8];
    uint32_t   version;
    uint32_t   pad;
    FpPoolSlot slots[FP_POOL_MAX_WORKERS];
} FpPoolRegistry;

typedef struct PoolRing
{
    FastpostgresRing *r;
    size_t            rsize;
    char              seg[64];
    char              stream[64]; /* the open stream's name, if any */
    bool              have_stream;
} PoolRing;

typedef struct PoolSet
{
    PoolRing rings[FP_POOL_MAX_RINGS];
    int      n;
} PoolSet;

typedef struct PoolTxn
{
    bool started;
} PoolTxn;

static int64_t
pool_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* /dev/shm/fp_pool_<dboid>: a worker serves one database, so the budget is per
 * database. Called from a backend and from the worker after it has connected. */
static int
pool_registry_path(char *buf, size_t n)
{
    int len = snprintf(buf, n, "/fp_pool_%u", MyDatabaseId);

    return (len > 0 && (size_t)len < n) ? 0 : -1;
}

static int
pool_open(bool create, int *fd_out, FpPoolRegistry **reg_out)
{
    char           path[96];
    int            fd;
    struct stat    st;
    void          *map;

    if (pool_registry_path(path, sizeof(path)) != 0) return -1;
    fd = shm_open(path, O_RDWR | (create ? O_CREAT : 0), 0600);
    if (fd < 0) return -1;
    /* map the whole struct even if an older file is short: touching past EOF on
     * a shared mapping is SIGBUS, not a short read */
    if (fstat(fd, &st) != 0 ||
        (st.st_size < (off_t)sizeof(FpPoolRegistry) &&
         ftruncate(fd, (off_t)sizeof(FpPoolRegistry)) != 0))
    {
        close(fd);
        return -1;
    }
    map = mmap(NULL, sizeof(FpPoolRegistry), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED)
    {
        close(fd);
        return -1;
    }
    *fd_out  = fd;
    *reg_out = (FpPoolRegistry *)map;
    return 0;
}

static void
pool_close(int fd, FpPoolRegistry *reg)
{
    munmap(reg, sizeof(FpPoolRegistry));
    close(fd);
}

/* Caller holds the flock. A registry made by an older layout is re-made: it
 * only ever holds names and pids, so losing it costs a worker, not data. */
static void
pool_init(FpPoolRegistry *reg)
{
    if (memcmp(reg->magic, FP_POOL_MAGIC, 8) == 0 && reg->version == FP_POOL_VERSION) return;

    memset(reg, 0, sizeof(*reg));
    memcpy(reg->magic, FP_POOL_MAGIC, 8);
    reg->version = FP_POOL_VERSION;
}

static void
pool_slot_reset(FpPoolSlot *s)
{
    memset(s, 0, sizeof(*s));
}

static uint32_t
pool_capacity(void)
{
    const char *env = getenv("FASTPOSTGRES_RINGS_PER_WORKER");
    long        n   = env && env[0] ? strtol(env, NULL, 10) : FP_POOL_DEFAULT_RINGS;

    if (n < 1) n = 1;
    if (n > FP_POOL_MAX_RINGS) n = FP_POOL_MAX_RINGS;
    return (uint32_t)n;
}

/* Is this slot's worker still alive? A starting slot has no pid yet: it is
 * kept for a grace period so a slow fork is not mistaken for a failure. */
static bool
pool_slot_dead(const FpPoolSlot *s)
{
    if (s->state == 0) return true;
    if (s->pid == 0) return pool_now_ms() - s->started_ms > FP_POOL_START_GRACE_MS;
    if (kill((pid_t)s->pid, 0) == 0) return false;
    return errno == ESRCH;
}

/* Find the live worker that should take one more ring when a new worker cannot
 * be registered. Prefer spare capacity already granted (a previous bump); else
 * the least-loaded worker below the per-worker cap. Caller holds the lock. */
static FpPoolSlot *
pool_overflow_slot(FpPoolRegistry *reg)
{
    FpPoolSlot *best = NULL;
    int         i;

    for (i = 0; i < FP_POOL_MAX_WORKERS; i++)
    {
        FpPoolSlot *s = &reg->slots[i];

        if (s->state == 0 || pool_slot_dead(s)) continue;
        if (s->used < s->capacity)
        {
            if (best == NULL || s->used < best->used) best = s;
            continue;
        }
        if (s->capacity < pool_capacity() &&
            (best == NULL || s->used < best->used || (best->used >= best->capacity)))
            best = s;
    }
    return best;
}

/* Backend side: give `seg` its own worker while the server has a slot; when the
 * budget is exhausted, put it on a worker that already has one. That policy is
 * the point of the pool: one ring per worker (the 1:1 shape, and its throughput)
 * until max_worker_processes says no, then more rings per worker instead of a
 * silent fallback to libpq. */
static bool
pool_ring_attach(const char *seg, const char *db, const char *transport)
{
    int             fd;
    FpPoolRegistry *reg;
    FpPoolSlot     *fresh = NULL;
    BackgroundWorker w;
    BackgroundWorkerHandle *handle;
    char             extra[BGW_EXTRALEN];
    int              i;
    int              slot = -1;
    bool             ok;

    if (pool_open(true, &fd, &reg) != 0) return false;
    flock(fd, LOCK_EX);
    pool_init(reg);

    for (i = 0; i < FP_POOL_MAX_WORKERS; i++)
    {
        FpPoolSlot *s = &reg->slots[i];

        if (s->state != 0 && pool_slot_dead(s)) pool_slot_reset(s);
        if (s->state == 0 && fresh == NULL) fresh = s;
    }

    /* First choice: a worker of its own, at capacity one. */
    if (fresh != NULL)
    {
        fresh->pid        = 0;
        fresh->state      = 1;
        fresh->capacity   = 1;
        fresh->used       = 1;
        fresh->started_ms = pool_now_ms();
        strlcpy(fresh->segs[0], seg, sizeof(fresh->segs[0]));
        slot = (int)(fresh - reg->slots);
    }
    flock(fd, LOCK_UN);

    if (slot >= 0)
    {
        memset(&w, 0, sizeof(w));
        snprintf(w.bgw_name, BGW_MAXLEN, "fastpostgres ring");
        snprintf(w.bgw_type, BGW_MAXLEN, "fastpostgres ring worker");
        w.bgw_flags        = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
        w.bgw_start_time   = BgWorkerStart_RecoveryFinished;
        w.bgw_restart_time = BGW_NEVER_RESTART;
        snprintf(w.bgw_library_name, BGW_MAXLEN, "fastpostgres");
        snprintf(w.bgw_function_name, BGW_MAXLEN, "fastpostgres_pool_main");
        w.bgw_notify_pid = MyProcPid;

        memset(w.bgw_extra, 0, BGW_EXTRALEN);
        snprintf(extra, sizeof(extra), "%d", slot);
        memcpy(w.bgw_extra, extra, strlen(extra) + 1);
        if (strlen(extra) + strlen(db) + strlen(transport) + 3 > BGW_EXTRALEN || strlen(seg) >= 64)
        {
            flock(fd, LOCK_EX);
            pool_slot_reset(&reg->slots[slot]);
            flock(fd, LOCK_UN);
            pool_close(fd, reg);
            return false;
        }
        memcpy(w.bgw_extra + strlen(extra) + 1, db, strlen(db) + 1);
        memcpy(w.bgw_extra + strlen(extra) + strlen(db) + 2, transport, strlen(transport) + 1);

        if (RegisterDynamicBackgroundWorker(&w, &handle))
        {
            pool_close(fd, reg);
            return true;
        }

        /* No slot: the server is at max_worker_processes. Take this ring back
         * out of the failed slot and multiplex it below. */
        flock(fd, LOCK_EX);
        pool_slot_reset(&reg->slots[slot]);
    }
    else
        flock(fd, LOCK_EX);

    {
        FpPoolSlot *best = pool_overflow_slot(reg);

        if (best == NULL)
        {
            flock(fd, LOCK_UN);
            pool_close(fd, reg);
            return false;
        }
        if (best->used >= best->capacity)
        {
            best->capacity++;
            elog(LOG, "fastpostgres: no free background worker; ring \"%s\" joins worker %d "
                      "(capacity %u)", seg, (int)best->pid, best->capacity);
        }
        strlcpy(best->segs[best->used], seg, sizeof(best->segs[0]));
        best->used++;
        ok = true;
    }
    flock(fd, LOCK_UN);
    pool_close(fd, reg);
    return ok;
}

/* Worker side: drop this process's slot and every ring name in it. */
static void
pool_unregister_all(void)
{
    int             fd;
    FpPoolRegistry *reg;
    int             i;
    pid_t           me = getpid();

    if (pool_open(false, &fd, &reg) != 0) return;
    flock(fd, LOCK_EX);
    for (i = 0; i < FP_POOL_MAX_WORKERS; i++)
        if (reg->slots[i].pid == me) pool_slot_reset(&reg->slots[i]);
    flock(fd, LOCK_UN);
    pool_close(fd, reg);
}

/* Remove one ring from our slot, so its capacity is reusable when the client
 * stops the ring and creates another. Compaction is a swap with the last. */
static void
pool_unregister_ring(const char *seg)
{
    int             fd;
    FpPoolRegistry *reg;
    int             i;
    pid_t           me = getpid();

    if (pool_open(false, &fd, &reg) != 0) return;
    flock(fd, LOCK_EX);
    for (i = 0; i < FP_POOL_MAX_WORKERS; i++)
    {
        FpPoolSlot *s = &reg->slots[i];
        uint32_t    j;

        if (s->pid != me) continue;
        for (j = 0; j < s->used; j++)
        {
            if (strcmp(s->segs[j], seg) != 0) continue;
            s->used--;
            if (j != s->used) memcpy(s->segs[j], s->segs[s->used], sizeof(s->segs[0]));
            s->segs[s->used][0] = 0;
            break;
        }
        break;
    }
    flock(fd, LOCK_UN);
    pool_close(fd, reg);
}

/* ---- the pool worker itself --------------------------------------------- */

static void
pool_begin(PoolTxn *tx)
{
    if (tx->started) return;
    StartTransactionCommand();
    PushActiveSnapshot(GetTransactionSnapshot());
    tx->started = true;
}

static void
pool_abort(PoolSet *set, PoolTxn *tx)
{
    if (tx->started || IsTransactionState()) AbortCurrentTransaction();
    tx->started = false;
    for (int i = 0; i < set->n; i++) set->rings[i].have_stream = false;
    fastpostgres_rows_carry_reset();
}

static void
pool_commit(PoolSet *set, PoolTxn *tx)
{
    (void)set;
    if (!tx->started) return;
    PopActiveSnapshot();
    CommitTransactionCommand();
    tx->started = false;
}

static bool
pool_any_stream(const PoolSet *set)
{
    for (int i = 0; i < set->n; i++)
        if (set->rings[i].have_stream) return true;
    return false;
}

static int
pool_attach_one(PoolSet *set, const char *seg, const FastpostgresTransport *t)
{
    PoolRing *pr;

    if (set->n >= FP_POOL_MAX_RINGS) return -1;
    pr = &set->rings[set->n];
    memset(pr, 0, sizeof(*pr));

    pr->r = ring_map_rw(seg, &pr->rsize);
    if (pr->r == NULL) return -1;
    if (fastpostgres_transport_check(pr->r, t) != 0 ||
        t->worker_init(pr->r) < 0 ||
        fastpostgres_eventfd_attach(pr->r) < 0)
    {
        munmap(pr->r, pr->rsize);
        memset(pr, 0, sizeof(*pr));
        return -1;
    }
    strlcpy(pr->seg, seg, sizeof(pr->seg));
    pr->have_stream = false;
    __atomic_store_n(&pr->r->tally.worker_pid, (uint32_t)getpid(), __ATOMIC_RELEASE);
    __atomic_store_n(&pr->r->worker_ready, 1, __ATOMIC_RELEASE);
    set->n++;
    return 0;
}

/* Unmap a ring and free its slot. `tx` is committed when it was the last open
 * stream, because a transaction cannot be kept for nothing. */
static void
pool_drop_ring(PoolSet *set, int i, PoolTxn *tx)
{
    PoolRing *pr = &set->rings[i];

    if (pr->have_stream)
    {
        PG_TRY();
        {
            fastpostgres_stream_close(pr->stream);
        }
        PG_CATCH();
        {
            FlushErrorState();
            pool_abort(set, tx);
        }
        PG_END_TRY();
        pr->have_stream = false;
    }
    if (tx->started && !pool_any_stream(set)) pool_commit(set, tx);

    __atomic_store_n(&pr->r->worker_ready, 0, __ATOMIC_RELEASE);
    fastpostgres_eventfd_detach(pr->r);
    pool_unregister_ring(pr->seg);
    munmap(pr->r, pr->rsize);

    if (i != set->n - 1) set->rings[i] = set->rings[set->n - 1];
    memset(&set->rings[set->n - 1], 0, sizeof(set->rings[0]));
    set->n--;
}

/* Attach every ring the backend has queued since the last look. Called on the
 * 100 ms idle tick, never per request: attaching is not on the hot path. */
static void
pool_refresh(PoolSet *set, const FastpostgresTransport *t)
{
    int             fd;
    FpPoolRegistry *reg;
    FpPoolSlot     *mine = NULL;
    char            segs[FP_POOL_MAX_RINGS][64];
    uint32_t        used = 0;
    pid_t           me   = getpid();
    int             i, j;

    if (pool_open(false, &fd, &reg) != 0) return;
    flock(fd, LOCK_EX);
    for (i = 0; i < FP_POOL_MAX_WORKERS; i++)
        if (reg->slots[i].pid == me && reg->slots[i].state == 2) mine = &reg->slots[i];
    if (mine != NULL)
    {
        used = mine->used > FP_POOL_MAX_RINGS ? FP_POOL_MAX_RINGS : mine->used;
        memcpy(segs, mine->segs, sizeof(segs));
    }
    flock(fd, LOCK_UN);
    pool_close(fd, reg);

    if (mine == NULL) return;
    for (i = 0; i < (int)used; i++)
    {
        bool have = false;

        if (segs[i][0] == 0) continue;
        for (j = 0; j < set->n; j++)
            if (strcmp(set->rings[j].seg, segs[i]) == 0) have = true;
        if (!have) pool_attach_one(set, segs[i], t);
    }
}

static bool
pool_handle(PoolRing *pr, PoolSet *set, PoolTxn *tx)
{
    FastpostgresRing *r   = pr->r;
    char             *a1  = (char *)fastpostgres_ring_arg1(r->arg);
    char             *a2  = (char *)fastpostgres_ring_arg2(r->arg);
    const char       *data;

    data = (r->op == FASTPOSTGRES_OP_NEXT || r->op == FASTPOSTGRES_OP_NEXT_ROWS) ? a2 : NULL;
    if (data != NULL && strcmp(data, pr->seg) == 0)
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
            pool_begin(tx);
            if (pr->have_stream)
            {
                PG_TRY();
                {
                    fastpostgres_stream_close(pr->stream);
                }
                PG_CATCH();
                {
                    ring_set_error(r);
                    pool_abort(set, tx);
                    break;
                }
                PG_END_TRY();
                pr->have_stream = false;
            }
            PG_TRY();
            {
                fastpostgres_stream_open(a1, a2);
                pr->have_stream = true;
                strlcpy(pr->stream, a1, sizeof(pr->stream));
                r->status = 0;
            }
            PG_CATCH();
            {
                ring_set_error(r);
                pool_abort(set, tx);
            }
            PG_END_TRY();
            break;

        case FASTPOSTGRES_OP_NEXT:
            if (!pr->have_stream)
            {
                r->status = 1;
                strlcpy(r->error, "no open stream", sizeof(r->error));
                break;
            }
            PG_TRY();
            {
                uint64 n  = fastpostgres_stream_next(a1, a2, (int64)r->batch, ring_data_capacity(r));
                r->status = 0;
                r->nrows  = (int64)n;
            }
            PG_CATCH();
            {
                ring_set_error(r);
                pool_abort(set, tx);
            }
            PG_END_TRY();
            break;

        case FASTPOSTGRES_OP_NEXT_ROWS:
            if (!pr->have_stream)
            {
                r->status = 1;
                strlcpy(r->error, "no open stream", sizeof(r->error));
                break;
            }
            PG_TRY();
            {
                uint64 n  = fastpostgres_stream_next_rows(a1, a2, (int64)r->batch, ring_data_capacity(r));
                r->status = 0;
                r->nrows  = (int64)n;
            }
            PG_CATCH();
            {
                ring_set_error(r);
                pool_abort(set, tx);
            }
            PG_END_TRY();
            break;

        case FASTPOSTGRES_OP_CLOSE:
            if (pr->have_stream)
            {
                PG_TRY();
                {
                    fastpostgres_stream_close(pr->stream);
                }
                PG_CATCH();
                {
                    ring_set_error(r);
                    pool_abort(set, tx);
                }
                PG_END_TRY();
                pr->have_stream = false;
            }
            r->status = 0;
            if (tx->started && !pool_any_stream(set)) pool_commit(set, tx);
            break;

        default:
            r->status = 1;
            strlcpy(r->error, "unknown op", sizeof(r->error));
            break;
    }
    return true;
}

/* The pool worker entry point (looked up by name, so it must be exported). */
PGDLLEXPORT void fastpostgres_pool_main(Datum main_arg);

PGDLLEXPORT void
fastpostgres_pool_main(Datum main_arg)
{
    BackgroundWorker            *bw   = MyBgworkerEntry;
    const char                  *slot_s = bw->bgw_extra;
    const char                  *db     = slot_s + strlen(slot_s) + 1;
    const char                  *tname  = db + strlen(db) + 1;
    const FastpostgresTransport *t;
    PoolSet                      set;
    PoolTxn                      tx = {false};
    int64_t                      empty_since = 0;
    int64_t                      last_tick;
    int                          slot;

    (void)main_arg;

    pqsignal(SIGTERM, ring_sigterm);
    BackgroundWorkerUnblockSignals();

    if (slot_s[0] < '0' || slot_s[0] > '9') proc_exit(1);
    slot = atoi(slot_s);
    if (slot < 0 || slot >= FP_POOL_MAX_WORKERS) proc_exit(1);

    t = fastpostgres_transport_by_name(tname[0] ? tname : "eventfd");
    if (t == NULL || t->id != FASTPOSTGRES_TRANSPORT_EVENTFD)
    {
        elog(LOG, "fastpostgres_pool_main: unknown transport \"%s\"", tname);
        proc_exit(1);
    }

    BackgroundWorkerInitializeConnection(db[0] ? db : "postgres", NULL, 0);

    /* Publish ourselves as the slot's owner. The backend queued the rings
     * before registering us, so they are already in segs[]. */
    {
        int             fd;
        FpPoolRegistry *reg;

        if (pool_open(false, &fd, &reg) != 0) proc_exit(1);
        flock(fd, LOCK_EX);
        if (memcmp(reg->magic, FP_POOL_MAGIC, 8) != 0 || reg->version != FP_POOL_VERSION ||
            reg->slots[slot].state == 0)
        {
            /* the backend gave up on us (or another worker claimed it) */
            flock(fd, LOCK_UN);
            pool_close(fd, reg);
            proc_exit(1);
        }
        reg->slots[slot].pid   = (int32_t)getpid();
        reg->slots[slot].state = 2;
        flock(fd, LOCK_UN);
        pool_close(fd, reg);
    }

    memset(&set, 0, sizeof(set));
    pool_refresh(&set, t);
    last_tick = pool_now_ms();

    for (;;)
    {
        struct pollfd pfds[FP_POOL_MAX_RINGS];
        int           rc;
        int           i;

        if (got_sigterm) break;

        /* A periodic tick, not only on an idle poll returns: a worker under
         * sustained load would otherwise never look at the registry (and a new
         * ring queued on it would wait for a quiet moment that may not come),
         * nor notice that a client died. */
        if (pool_now_ms() - last_tick >= FASTPOSTGRES_RING_WAIT_MS)
        {
            last_tick = pool_now_ms();
            pool_refresh(&set, t);
            for (i = 0; i < set.n;)
            {
                FastpostgresRing *r = set.rings[i].r;

                if (r->op == FASTPOSTGRES_OP_STOP ||
                    (r->client_pid != 0 && kill((pid_t)r->client_pid, 0) != 0 &&
                     errno == ESRCH))
                {
                    pool_drop_ring(&set, i, &tx);
                    continue;
                }
                i++;
            }
        }

        if (set.n == 0)
        {
            if (empty_since == 0) empty_since = pool_now_ms();
            else if (pool_now_ms() - empty_since > FP_POOL_EXIT_MS) break;
        }
        else
            empty_since = 0;

        for (i = 0; i < set.n; i++)
        {
            pfds[i].fd      = set.rings[i].r->worker_req_efd;
            pfds[i].events  = POLLIN;
            pfds[i].revents = 0;
        }

        /* With no rings, poll(0 fds) would spin: sleep one tick instead, so a
         * worker that has just been drained waits for a new ring or the exit
         * grace. */
        if (set.n == 0)
        {
            pg_usleep((long)FASTPOSTGRES_RING_WAIT_MS * 1000L);
            rc = 0;
        }
        else
            rc = poll(pfds, (nfds_t)set.n, FASTPOSTGRES_RING_WAIT_MS);

        if (rc < 0)
        {
            if (errno == EINTR) continue;
            elog(LOG, "fastpostgres_pool_main: poll failed: %m");
            break;
        }
        if (rc == 0) continue; /* the tick at the top of the loop does the chores */

        /* Service the first ring that rang. One executor turn at a time is the
         * honest model for one backend; the next poll picks up the rest. */
        {
            int               which = -1;
            PoolRing         *pr;
            FastpostgresRing *r;
            uint64_t          asked;
            uint64_t          answered;
            uint64_t          drained;
            ssize_t           got;

            for (i = 0; i < set.n; i++)
            {
                if (pfds[i].revents & (POLLIN | POLLERR | POLLHUP))
                {
                    which = i;
                    break;
                }
            }
            if (which < 0) continue;

            pr = &set.rings[which];
            r  = pr->r;

            /* Drain the bell; one request is in flight per ring. A read that
             * would block is a stale wake, and req_seq == resp_seq below
             * confirms it. */
            do
                got = read(r->worker_req_efd, &drained, sizeof(drained));
            while (got < 0 && errno == EINTR);
            if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                elog(LOG, "fastpostgres_pool_main: eventfd read failed: %m");
                break;
            }

            asked    = __atomic_load_n(&r->req_seq, __ATOMIC_ACQUIRE);
            answered = __atomic_load_n(&r->resp_seq, __ATOMIC_ACQUIRE);
            if (asked != answered)
            {
                r->status   = 0;
                r->nrows    = 0;
                r->error[0] = 0;

                if (!pool_handle(pr, &set, &tx))
                {
                    pool_drop_ring(&set, which, &tx);
                    continue;
                }
                if (t->post_response(r, asked) < 0)
                {
                    elog(LOG, "fastpostgres_pool_main: post_response failed: %m");
                    break;
                }
            }
        }
    }

    pool_abort(&set, &tx);
    while (set.n > 0) pool_drop_ring(&set, 0, &tx);
    pool_unregister_all();
    proc_exit(0);
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
        munmap(r, rsize);
        proc_exit(1);
    }

    if (t->worker_init(r) < 0)
    {
        elog(LOG, "fastpostgres_ring_main: %s worker_init failed: %m", t->name);
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
    uint32_t                     ring_transport;
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
    ring_transport = t->id;
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
        ring_transport = r->transport;
        munmap(r, rsize);
    }

    /* A ring that speaks the eventfd engine does not get a worker of its own:
     * it is queued on one of the pool's few workers, which poll every ring they
     * serve. That is the whole point of the engine — the number of rings is
     * no longer bounded by max_worker_processes. */
    if (ring_transport == FASTPOSTGRES_TRANSPORT_EVENTFD)
    {
        if (r == NULL)
            elog(ERROR, "fastpostgres_ring_start: ring \"%s\" is not a segment", seg);
        if (!pool_ring_attach(seg, db, "eventfd"))
            elog(ERROR, "fastpostgres_ring_start: no pool worker accepted ring \"%s\"", seg);
        snprintf(summary, sizeof(summary), "ring=%s started", seg);
        PG_RETURN_TEXT_P(cstring_to_text(summary));
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
        /* The semaphore is the shm engine's doorbell. An eventfd ring's bell is
         * a process-local fd this backend does not own; the pool worker sees
         * the op on its next 100 ms tick, and the client's own stop() posts. */
        if (r->transport != FASTPOSTGRES_TRANSPORT_EVENTFD) sem_post(&r->req_ready);
        munmap(r, rsize);
    }

    snprintf(summary, sizeof(summary), "ring=%s stopping", seg);
    PG_RETURN_TEXT_P(cstring_to_text(summary));
}
