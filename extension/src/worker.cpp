/*
 * worker.cpp — a dynamically registered background worker that services the
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
 * open /dev/shm/<seg> can execute SQL through it. The client creates the control
 * segment 0600. Data segments default to sealed memfds handed over SCM_RIGHTS on
 * a socket whose peer is checked with SO_PEERCRED (shm.c); the /dev/shm fallback
 * is still widened to 0666 so a client running as a different user can map it.
 * Keep the segment name out of shared trees and treat the segment itself as the
 * boundary. Not for production.
 */
#include "pgxx.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <poll.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include "access/xact.h"
#include "commands/dbcommands.h"
#include "fmgr.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"

#include "fastpostgres/fastpostgres_shm.h"

#include "fastpostgres/ring.h"
#include "transport/transport.h"
}
#include "carry.h"
#include "columnar.h"
#include "rows.h"
#include "shm.h"
#include "stream.h"

namespace
{

constexpr int kRingWaitMs = 100; /* only so the worker notices SIGTERM */

volatile sig_atomic_t got_sigterm   = false;
bool                  have_txn      = false;
bool                  have_stream   = false;
bool                  have_snapshot = false;

void
ring_sigterm(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sigterm    = true;
    errno          = save_errno;
}

FastpostgresRing *
ring_map_rw(const char *seg, size_t *size_out)
{
    char        path[256];
    int         fd;
    struct stat st;
    void       *map;

    std::snprintf(path, sizeof(path), "/%s", seg);
    fd = shm_open(path, O_RDWR, 0);
    if (fd < 0) return nullptr;
    if (fstat(fd, &st) != 0)
    {
        close(fd);
        return nullptr;
    }
    map = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return nullptr;
    if (static_cast<size_t>(st.st_size) < sizeof(FastpostgresRing) ||
        std::memcmp(static_cast<FastpostgresRing *>(map)->magic, FASTPOSTGRES_RING_MAGIC, 8) != 0)
    {
        munmap(map, static_cast<size_t>(st.st_size));
        return nullptr;
    }
    *size_out = static_cast<size_t>(st.st_size);
    return static_cast<FastpostgresRing *>(map);
}

void
abort_txn(void)
{
    if (have_txn || IsTransactionState()) AbortCurrentTransaction();
    /* AbortCurrentTransaction resets the snapshot stack */
    have_txn      = false;
    have_stream   = false;
    have_snapshot = false;
}

void
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
void
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
void
ring_set_error(FastpostgresRing *r)
{
    MemoryContext oldcontext = MemoryContextSwitchTo(TopMemoryContext);
    ErrorData    *edata      = CopyErrorData();

    r->status = 1;
    fp::copy_bounded(r->error, sizeof(r->error), edata->message ? edata->message : "error");
    FlushErrorState();
    MemoryContextSwitchTo(oldcontext);
    FreeErrorData(edata);
}

/* How big the client asked for a data segment to be, clamped to something that
 * can hold a frame. Read from the ring per request: the client writes it before
 * starting the worker. */
size_t
ring_data_capacity(const FastpostgresRing *r)
{
    uint64_t want = r->data_bytes;

    if (want == 0) return FASTPOSTGRES_SEGMENT_BYTES;
    if (want < FASTPOSTGRES_SEGMENT_MIN) return FASTPOSTGRES_SEGMENT_MIN;
    if (want > FASTPOSTGRES_SEGMENT_BYTES) return FASTPOSTGRES_SEGMENT_BYTES;
    return static_cast<size_t>(want);
}

/* Returns false when the worker should stop. */
bool
ring_handle(FastpostgresRing *r, const char *ring_name)
{
    const char *a1 = fastpostgres_ring_arg1(r->arg);
    const char *a2 = fastpostgres_ring_arg2(r->arg);
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
        data = nullptr;

    if (data != nullptr && std::strcmp(data, ring_name) == 0)
    {
        r->status = 1;
        fp::copy_bounded(r->error, sizeof(r->error), "data segment must differ from the ring");
        return true;
    }
    if (data != nullptr && data[0] == 0)
    {
        r->status = 1;
        fp::copy_bounded(r->error, sizeof(r->error), "next needs a segment");
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
                fp::stream_open(a1, a2);
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
                fp::copy_bounded(r->error, sizeof(r->error), "no open stream");
                break;
            }
            PG_TRY();
            {
                n         = fp::stream_next(a1, a2, static_cast<int64>(r->batch), ring_data_capacity(r));
                r->status = 0;
                r->nrows  = static_cast<int64>(n);
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
                fp::copy_bounded(r->error, sizeof(r->error), "no open stream");
                break;
            }
            PG_TRY();
            {
                n         = fp::stream_next_rows(a1, a2, static_cast<int64>(r->batch), ring_data_capacity(r));
                r->status = 0;
                r->nrows  = static_cast<int64>(n);
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
                fp::copy_bounded(r->error, sizeof(r->error), "no open stream");
                break;
            }
            PG_TRY();
            {
                /* a1 is both the stream and the data segment (the pool names them
                 * alike); a2 is the plan. */
                n = fp::stream_next_reduce(a1, a1, static_cast<int64>(r->batch), ring_data_capacity(r), a2);
                r->status = 0;
                r->nrows  = static_cast<int64>(n);
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
                fp::stream_close(a1);
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
            fp::copy_bounded(r->error, sizeof(r->error), "unknown op");
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
constexpr char     kPoolMagic[]      = "FPPOOL01";
constexpr uint32_t kPoolVersion      = 2u; /* v2 adds the load hint */
constexpr int      kPoolMaxWorkers   = 8;  /* slots in the registry */
constexpr int      kPoolMaxRings     = 32; /* rings one worker will serve */
constexpr int      kPoolDefaultRings = 8;  /* FASTPOSTGRES_RINGS_PER_WORKER */
constexpr int64_t  kPoolExitMs       = 2000;
constexpr int64_t  kPoolStartGraceMs = 30000;
constexpr int64_t  kPoolIdleMs       = 25; /* quiet this long means "not in a read" */

struct FpPoolSlot {
    int32_t  pid;      /* 0 while starting, then the worker's pid */
    uint32_t state;    /* 0 free, 1 starting, 2 running */
    uint32_t capacity; /* rings this worker will serve */
    uint32_t used;     /* rings listed in segs[] */

    /* Published by the worker (atomics, no lock) so the attach policy can put
     * a new ring on a quiet worker before a busy one. `busy` is "a ring has an
     * unanswered request"; `last_service_ms` keeps a worker that has just
     * answered one looking busy for a moment, since a read is many batches. */
    uint32_t busy;
    uint32_t pad;
    int64_t  last_service_ms;

    int64_t started_ms;
    char    segs[kPoolMaxRings][64];
};

struct FpPoolRegistry {
    char       magic[8];
    uint32_t   version;
    uint32_t   pad;
    FpPoolSlot slots[kPoolMaxWorkers];
};

struct PoolRing {
    FastpostgresRing *r;
    size_t            rsize;
    char              seg[64];
    char              stream[64]; /* the open stream's name, if any */
    bool              have_stream;
};

struct PoolSet {
    PoolRing rings[kPoolMaxRings];
    int      n;
};

struct PoolTxn {
    bool started;
};

/* The worker's own view of the registry, kept mapped for its lifetime so the
 * load hint is an atomic store instead of a shm_open per request. */
int             pool_self_fd   = -1;
FpPoolRegistry *pool_self_reg  = nullptr;
int             pool_self_slot = -1;

int64_t
pool_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

/* /dev/shm/fp_pool_<dboid>: a worker serves one database, so the budget is per
 * database. Called from a backend and from the worker after it has connected. */
int
pool_registry_path(char *buf, size_t n)
{
    int len = std::snprintf(buf, n, "/fp_pool_%u", MyDatabaseId);

    return (len > 0 && static_cast<size_t>(len) < n) ? 0 : -1;
}

int
pool_open(bool create, int *fd_out, FpPoolRegistry **reg_out)
{
    char        path[96];
    int         fd;
    struct stat st;
    void       *map;

    if (pool_registry_path(path, sizeof(path)) != 0) return -1;
    fd = shm_open(path, O_RDWR | (create ? O_CREAT : 0), 0600);
    if (fd < 0) return -1;
    /* map the whole struct even if an older file is short: touching past EOF on
     * a shared mapping is SIGBUS, not a short read */
    if (fstat(fd, &st) != 0 || (st.st_size < static_cast<off_t>(sizeof(FpPoolRegistry)) &&
                                ftruncate(fd, static_cast<off_t>(sizeof(FpPoolRegistry))) != 0))
    {
        close(fd);
        return -1;
    }
    map = mmap(nullptr, sizeof(FpPoolRegistry), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED)
    {
        close(fd);
        return -1;
    }
    *fd_out  = fd;
    *reg_out = static_cast<FpPoolRegistry *>(map);
    return 0;
}

void
pool_close(int fd, FpPoolRegistry *reg)
{
    munmap(reg, sizeof(FpPoolRegistry));
    close(fd);
}

/* Caller holds the flock. A registry made by an older layout is re-made: it
 * only ever holds names and pids, so losing it costs a worker, not data. */
void
pool_init(FpPoolRegistry *reg)
{
    if (std::memcmp(reg->magic, kPoolMagic, sizeof(reg->magic)) == 0 && reg->version == kPoolVersion) return;

    std::memset(reg, 0, sizeof(*reg));
    std::memcpy(reg->magic, kPoolMagic, sizeof(reg->magic));
    reg->version = kPoolVersion;
}

void
pool_slot_reset(FpPoolSlot *s)
{
    std::memset(s, 0, sizeof(*s));
}

uint32_t
pool_capacity(void)
{
    const char *env = std::getenv("FASTPOSTGRES_RINGS_PER_WORKER");
    long        n   = env && env[0] ? std::strtol(env, nullptr, 10) : kPoolDefaultRings;

    if (n < 1) n = 1;
    if (n > kPoolMaxRings) n = kPoolMaxRings;
    return static_cast<uint32_t>(n);
}

/* Is this slot's worker still alive? A starting slot has no pid yet: it is
 * kept for a grace period so a slow fork is not mistaken for a failure. */
bool
pool_slot_dead(const FpPoolSlot *s)
{
    if (s->state == 0) return true;
    if (s->pid == 0) return pool_now_ms() - s->started_ms > kPoolStartGraceMs;
    if (kill(static_cast<pid_t>(s->pid), 0) == 0) return false;
    return errno == ESRCH;
}

/* Caller holds the lock. A worker is idle when it has no outstanding request
 * and has not served one for a few milliseconds. The hint is advisory: it is
 * written by the worker without the lock, and a stale value only changes which
 * worker is preferred, never which rings exist. */
bool
pool_slot_idle(const FpPoolSlot *s, int64_t now)
{
    int64_t last = __atomic_load_n(&s->last_service_ms, __ATOMIC_ACQUIRE);

    if (__atomic_load_n(&s->busy, __ATOMIC_ACQUIRE) != 0) return false;
    return last == 0 || now - last > kPoolIdleMs;
}

/* Find the live worker that should take one more ring when a new worker cannot
 * be registered. Prefer, in order: an idle worker, one that already has spare
 * capacity granted (so the pool does not grow a second worker's capacity), and
 * the least-loaded worker below the per-worker cap. Caller holds the lock. */
FpPoolSlot *
pool_overflow_slot(FpPoolRegistry *reg)
{
    FpPoolSlot *best       = nullptr;
    int64_t     now        = pool_now_ms();
    bool        best_idle  = false;
    int         best_group = 0;
    uint32_t    best_used  = 0;

    for (int i = 0; i < kPoolMaxWorkers; i++)
    {
        FpPoolSlot *s = &reg->slots[i];
        bool        spare;
        bool        idle;
        int         group;

        if (s->state == 0 || pool_slot_dead(s)) continue;
        spare = s->used < s->capacity;
        if (!spare && s->capacity >= pool_capacity()) continue;
        /* group 0 has capacity already granted; group 1 must grow it. */
        group = spare ? 0 : 1;
        idle  = pool_slot_idle(s, now);

        if (best == nullptr || (idle && !best_idle) ||
            (idle == best_idle && (group < best_group || (group == best_group && s->used < best_used))))
        {
            best       = s;
            best_idle  = idle;
            best_group = group;
            best_used  = s->used;
        }
    }
    return best;
}

/* Backend side: give `seg` its own worker while the server has a slot; when the
 * budget is exhausted, put it on a worker that already has one. That policy is
 * the point of the pool: one ring per worker (the 1:1 shape, and its throughput)
 * until max_worker_processes says no, then more rings per worker instead of a
 * silent fallback to libpq. */
bool
pool_ring_attach(const char *seg, const char *db, const char *transport)
{
    int                     fd;
    FpPoolRegistry         *reg;
    FpPoolSlot             *fresh = nullptr;
    BackgroundWorker        w;
    BackgroundWorkerHandle *handle = nullptr;
    char                    extra[BGW_EXTRALEN];
    int                     slot = -1;
    bool                    ok   = false;

    if (pool_open(true, &fd, &reg) != 0) return false;
    flock(fd, LOCK_EX);
    pool_init(reg);

    for (int i = 0; i < kPoolMaxWorkers; i++)
    {
        FpPoolSlot *s = &reg->slots[i];

        if (s->state != 0 && pool_slot_dead(s)) pool_slot_reset(s);
        if (s->state == 0 && fresh == nullptr) fresh = s;
    }

    /* First choice: a worker of its own, at capacity one. */
    if (fresh != nullptr)
    {
        fresh->pid        = 0;
        fresh->state      = 1;
        fresh->capacity   = 1;
        fresh->used       = 1;
        fresh->started_ms = pool_now_ms();
        strlcpy(fresh->segs[0], seg, sizeof(fresh->segs[0]));
        slot = static_cast<int>(fresh - reg->slots);
    }
    flock(fd, LOCK_UN);

    if (slot >= 0)
    {
        std::memset(&w, 0, sizeof(w));
        std::snprintf(w.bgw_name, BGW_MAXLEN, "fastpostgres ring");
        std::snprintf(w.bgw_type, BGW_MAXLEN, "fastpostgres ring worker");
        w.bgw_flags        = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
        w.bgw_start_time   = BgWorkerStart_RecoveryFinished;
        w.bgw_restart_time = BGW_NEVER_RESTART;
        std::snprintf(w.bgw_library_name, BGW_MAXLEN, "fastpostgres");
        std::snprintf(w.bgw_function_name, BGW_MAXLEN, "fastpostgres_pool_main");
        w.bgw_notify_pid = MyProcPid;

        std::memset(w.bgw_extra, 0, BGW_EXTRALEN);
        std::snprintf(extra, sizeof(extra), "%d", slot);
        std::memcpy(w.bgw_extra, extra, std::strlen(extra) + 1);
        if (std::strlen(extra) + std::strlen(db) + std::strlen(transport) + 3 > BGW_EXTRALEN ||
            std::strlen(seg) >= 64)
        {
            flock(fd, LOCK_EX);
            pool_slot_reset(&reg->slots[slot]);
            flock(fd, LOCK_UN);
            pool_close(fd, reg);
            return false;
        }
        std::memcpy(w.bgw_extra + std::strlen(extra) + 1, db, std::strlen(db) + 1);
        std::memcpy(w.bgw_extra + std::strlen(extra) + std::strlen(db) + 2, transport,
                    std::strlen(transport) + 1);

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

        if (best == nullptr)
        {
            flock(fd, LOCK_UN);
            pool_close(fd, reg);
            return false;
        }
        if (best->used >= best->capacity)
        {
            best->capacity++;
            elog(LOG,
                 "fastpostgres: no free background worker; ring \"%s\" joins worker %d "
                 "(capacity %u)",
                 seg, static_cast<int>(best->pid), best->capacity);
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
void
pool_unregister_all(void)
{
    int             fd;
    FpPoolRegistry *reg;
    pid_t           me = getpid();

    if (pool_open(false, &fd, &reg) != 0) return;
    flock(fd, LOCK_EX);
    for (int i = 0; i < kPoolMaxWorkers; i++)
        if (reg->slots[i].pid == me) pool_slot_reset(&reg->slots[i]);
    flock(fd, LOCK_UN);
    pool_close(fd, reg);
}

/* Remove one ring from our slot, so its capacity is reusable when the client
 * stops the ring and creates another. Compaction is a swap with the last. */
void
pool_unregister_ring(const char *seg)
{
    int             fd;
    FpPoolRegistry *reg;
    pid_t           me = getpid();

    if (pool_open(false, &fd, &reg) != 0) return;
    flock(fd, LOCK_EX);
    for (int i = 0; i < kPoolMaxWorkers; i++)
    {
        FpPoolSlot *s = &reg->slots[i];

        if (s->pid != me) continue;
        for (uint32_t j = 0; j < s->used; j++)
        {
            if (std::strcmp(s->segs[j], seg) != 0) continue;
            s->used--;
            if (j != s->used) std::memcpy(s->segs[j], s->segs[s->used], sizeof(s->segs[0]));
            s->segs[s->used][0] = 0;
            break;
        }
        break;
    }
    flock(fd, LOCK_UN);
    pool_close(fd, reg);
}

/* ---- the pool worker itself --------------------------------------------- */

void
pool_begin(PoolTxn *tx)
{
    if (tx->started) return;
    StartTransactionCommand();
    PushActiveSnapshot(GetTransactionSnapshot());
    tx->started = true;
}

void
pool_abort(PoolSet *set, PoolTxn *tx)
{
    if (tx->started || IsTransactionState()) AbortCurrentTransaction();
    tx->started = false;
    for (int i = 0; i < set->n; i++)
        set->rings[i].have_stream = false;
    fp::carry_reset();
}

void
pool_commit([[maybe_unused]] PoolSet *set, PoolTxn *tx)
{
    if (!tx->started) return;
    PopActiveSnapshot();
    CommitTransactionCommand();
    tx->started = false;
}

bool
pool_any_stream(const PoolSet *set)
{
    for (int i = 0; i < set->n; i++)
        if (set->rings[i].have_stream) return true;
    return false;
}

/* Publish whether this worker has work outstanding. Called once per poll loop:
 * the set is tiny and the fields are plain atomics in a mapping the worker
 * already owns, so the hot path gains two cached stores, not a syscall. */
void
pool_publish_load(PoolSet *set)
{
    FpPoolSlot *s;
    bool        busy = false;

    if (pool_self_reg == nullptr || pool_self_slot < 0) return;
    s = &pool_self_reg->slots[pool_self_slot];

    for (int i = 0; i < set->n; i++)
    {
        FastpostgresRing *r = set->rings[i].r;

        if (__atomic_load_n(&r->req_seq, __ATOMIC_ACQUIRE) != __atomic_load_n(&r->resp_seq, __ATOMIC_ACQUIRE))
        {
            busy = true;
            break;
        }
    }
    if (busy) __atomic_store_n(&s->last_service_ms, pool_now_ms(), __ATOMIC_RELEASE);
    __atomic_store_n(&s->busy, busy ? 1u : 0u, __ATOMIC_RELEASE);
}

int
pool_attach_one(PoolSet *set, const char *seg, const FastpostgresTransport *t)
{
    PoolRing *pr;

    if (set->n >= kPoolMaxRings) return -1;
    pr = &set->rings[set->n];
    std::memset(pr, 0, sizeof(*pr));

    pr->r = ring_map_rw(seg, &pr->rsize);
    if (pr->r == nullptr) return -1;
    if (fastpostgres_transport_check(pr->r, t) != 0 || t->worker_init(pr->r) < 0 ||
        fastpostgres_eventfd_attach(pr->r) < 0)
    {
        munmap(pr->r, pr->rsize);
        std::memset(pr, 0, sizeof(*pr));
        return -1;
    }
    strlcpy(pr->seg, seg, sizeof(pr->seg));
    pr->have_stream = false;
    __atomic_store_n(&pr->r->tally.worker_pid, static_cast<uint32_t>(getpid()), __ATOMIC_RELEASE);
    __atomic_store_n(&pr->r->worker_ready, 1, __ATOMIC_RELEASE);
    set->n++;
    return 0;
}

/* Close one ring's stream without taking the worker's other streams with it.
 * Used on teardown (STOP, dead client): a close that errors aborts its own
 * savepoint and drops only this stream's carry. */
void
pool_close_stream(PoolRing *pr, PoolTxn *tx)
{
    if (!pr->have_stream) return;

    if (tx->started)
    {
        MemoryContext oldcontext = CurrentMemoryContext;
        ResourceOwner oldowner   = CurrentResourceOwner;

        BeginInternalSubTransaction(nullptr);
        PG_TRY();
        {
            fp::stream_close(pr->stream);
            ReleaseCurrentSubTransaction();
            MemoryContextSwitchTo(oldcontext);
            CurrentResourceOwner = oldowner;
        }
        PG_CATCH();
        {
            MemoryContextSwitchTo(oldcontext);
            FlushErrorState();
            RollbackAndReleaseCurrentSubTransaction();
            MemoryContextSwitchTo(oldcontext);
            CurrentResourceOwner = oldowner;
            fp::stream_forget(pr->stream);
        }
        PG_END_TRY();
    }
    else
        fp::stream_forget(pr->stream);

    pr->have_stream = false;
}

/* Unmap a ring and free its slot. `tx` is committed when it was the last open
 * stream, because a transaction cannot be kept for nothing. */
void
pool_drop_ring(PoolSet *set, int i, PoolTxn *tx)
{
    PoolRing *pr = &set->rings[i];

    pool_close_stream(pr, tx);
    if (tx->started && !pool_any_stream(set)) pool_commit(set, tx);

    __atomic_store_n(&pr->r->worker_ready, 0, __ATOMIC_RELEASE);
    fastpostgres_eventfd_detach(pr->r);
    pool_unregister_ring(pr->seg);
    munmap(pr->r, pr->rsize);

    if (i != set->n - 1) set->rings[i] = set->rings[set->n - 1];
    std::memset(&set->rings[set->n - 1], 0, sizeof(set->rings[0]));
    set->n--;
}

/* Attach every ring the backend has queued since the last look. Called on the
 * 100 ms idle tick, never per request: attaching is not on the hot path. */
void
pool_refresh(PoolSet *set, const FastpostgresTransport *t)
{
    int             fd;
    FpPoolRegistry *reg;
    FpPoolSlot     *mine = nullptr;
    char            segs[kPoolMaxRings][64];
    uint32_t        used = 0;
    pid_t           me   = getpid();

    if (pool_open(false, &fd, &reg) != 0) return;
    flock(fd, LOCK_EX);
    for (int i = 0; i < kPoolMaxWorkers; i++)
        if (reg->slots[i].pid == me && reg->slots[i].state == 2) mine = &reg->slots[i];
    if (mine != nullptr)
    {
        used = mine->used > static_cast<uint32_t>(kPoolMaxRings) ? static_cast<uint32_t>(kPoolMaxRings)
                                                                 : mine->used;
        std::memcpy(segs, mine->segs, sizeof(segs));
    }
    flock(fd, LOCK_UN);
    pool_close(fd, reg);

    if (mine == nullptr) return;
    for (int i = 0; i < static_cast<int>(used); i++)
    {
        bool have = false;

        if (segs[i][0] == 0) continue;
        for (int j = 0; j < set->n; j++)
            if (std::strcmp(set->rings[j].seg, segs[i]) == 0) have = true;
        if (!have) pool_attach_one(set, segs[i], t);
    }
}

/* The op-specific body. It may ereport: the caller runs it in a subtransaction
 * so a failure rolls back this request, and only this stream, rather than the
 * transaction that every ring on the worker is sharing. */
void
pool_serve_op(PoolRing *pr)
{
    FastpostgresRing *r  = pr->r;
    const char       *a1 = fastpostgres_ring_arg1(r->arg);
    const char       *a2 = fastpostgres_ring_arg2(r->arg);
    const char       *data;

    if (r->op == FASTPOSTGRES_OP_NEXT || r->op == FASTPOSTGRES_OP_NEXT_ROWS)
        data = a2;
    else if (r->op == FASTPOSTGRES_OP_REDUCE)
        data = a1; /* the reduce answer goes back into the stream's own segment */
    else
        data = nullptr;
    if (data != nullptr && std::strcmp(data, pr->seg) == 0)
    {
        r->status = 1;
        fp::copy_bounded(r->error, sizeof(r->error), "data segment must differ from the ring");
        return;
    }
    if (data != nullptr && data[0] == 0)
    {
        r->status = 1;
        fp::copy_bounded(r->error, sizeof(r->error), "next needs a segment");
        return;
    }

    switch (r->op)
    {
        case FASTPOSTGRES_OP_OPEN:
            if (pr->have_stream)
            {
                fp::stream_close(pr->stream);
                pr->have_stream = false;
            }
            fp::stream_open(a1, a2);
            pr->have_stream = true;
            strlcpy(pr->stream, a1, sizeof(pr->stream));
            r->status = 0;
            break;

        case FASTPOSTGRES_OP_NEXT:
            if (!pr->have_stream)
            {
                r->status = 1;
                fp::copy_bounded(r->error, sizeof(r->error), "no open stream");
                break;
            }
            {
                uint64 n = fp::stream_next(a1, a2, static_cast<int64>(r->batch), ring_data_capacity(r));

                r->status = 0;
                r->nrows  = static_cast<int64>(n);
            }
            break;

        case FASTPOSTGRES_OP_NEXT_ROWS:
            if (!pr->have_stream)
            {
                r->status = 1;
                fp::copy_bounded(r->error, sizeof(r->error), "no open stream");
                break;
            }
            {
                uint64 n = fp::stream_next_rows(a1, a2, static_cast<int64>(r->batch), ring_data_capacity(r));

                r->status = 0;
                r->nrows  = static_cast<int64>(n);
            }
            break;

        case FASTPOSTGRES_OP_REDUCE:
            if (!pr->have_stream)
            {
                r->status = 1;
                fp::copy_bounded(r->error, sizeof(r->error), "no open stream");
                break;
            }
            {
                /* a1 is both the stream and the data segment; a2 is the plan. */
                uint64 n =
                    fp::stream_next_reduce(a1, a1, static_cast<int64>(r->batch), ring_data_capacity(r), a2);

                r->status = 0;
                r->nrows  = static_cast<int64>(n);
            }
            break;

        case FASTPOSTGRES_OP_CLOSE:
            if (pr->have_stream)
            {
                fp::stream_close(pr->stream);
                pr->have_stream = false;
            }
            r->status = 0;
            break;

        default:
            r->status = 1;
            fp::copy_bounded(r->error, sizeof(r->error), "unknown op");
            break;
    }
}

/* Run one request for one ring inside a savepoint. A PG error aborts that
 * savepoint, not the worker's transaction: the other rings keep their streams
 * and their snapshot, and this ring is left with no stream (the portal died
 * with the savepoint) so its next OPEN starts clean. Returns false only for
 * STOP, which means the caller should drop the ring. */
bool
pool_handle(PoolRing *pr, PoolSet *set, PoolTxn *tx)
{
    FastpostgresRing *r          = pr->r;
    MemoryContext     oldcontext = CurrentMemoryContext;
    ResourceOwner     oldowner   = CurrentResourceOwner;

    if (r->op == FASTPOSTGRES_OP_STOP) return false;

    pool_begin(tx); /* the shared transaction, outside the savepoint */

    BeginInternalSubTransaction(nullptr);
    PG_TRY();
    {
        pool_serve_op(pr);
        ReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(oldcontext);
        CurrentResourceOwner = oldowner;
    }
    PG_CATCH();
    {
        ErrorData *edata;

        MemoryContextSwitchTo(oldcontext);
        edata = CopyErrorData();
        FlushErrorState();
        RollbackAndReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(oldcontext);
        CurrentResourceOwner = oldowner;

        /* The portal, if there was one, went with the savepoint. The carried
         * row is keyed by cursor name precisely so this stream can drop its
         * own and leave the other rings' carries alone. */
        if (pr->stream[0] != 0) fp::stream_forget(pr->stream);
        pr->have_stream = false;

        r->status = 1;
        fp::copy_bounded(r->error, sizeof(r->error),
                         (edata != nullptr && edata->message != nullptr) ? edata->message : "error");
        if (edata != nullptr) FreeErrorData(edata);
    }
    PG_END_TRY();

    if (tx->started && !pool_any_stream(set)) pool_commit(set, tx);
    return true;
}

} // namespace

/* The pool worker entry point (looked up by name, so it must be exported). */
extern "C" {
PGDLLEXPORT void fastpostgres_pool_main(Datum main_arg);
}

extern "C" PGDLLEXPORT void
fastpostgres_pool_main([[maybe_unused]] Datum main_arg)
{
    BackgroundWorker            *bw     = MyBgworkerEntry;
    const char                  *slot_s = bw->bgw_extra;
    const char                  *db     = slot_s + std::strlen(slot_s) + 1;
    const char                  *tname  = db + std::strlen(db) + 1;
    const FastpostgresTransport *t;
    PoolSet                      set;
    PoolTxn                      tx          = {false};
    int64_t                      empty_since = 0;
    int64_t                      last_tick;
    int                          slot;
    int                          next_ring = 0;

    pqsignal(SIGTERM, ring_sigterm);
    BackgroundWorkerUnblockSignals();

    if (slot_s[0] < '0' || slot_s[0] > '9') proc_exit(1);
    slot = std::atoi(slot_s);
    if (slot < 0 || slot >= kPoolMaxWorkers) proc_exit(1);

    t = fastpostgres_transport_by_name(tname[0] ? tname : "eventfd");
    if (t == nullptr || t->id != FASTPOSTGRES_TRANSPORT_EVENTFD)
    {
        elog(LOG, "fastpostgres_pool_main: unknown transport \"%s\"", tname);
        proc_exit(1);
    }

    BackgroundWorkerInitializeConnection(db[0] ? db : "postgres", nullptr, 0);

    /* Publish ourselves as the slot's owner. The backend queued the rings
     * before registering us, so they are already in segs[]. The registry stays
     * mapped for the worker's lifetime: the load hint below is an atomic store
     * in it, not a shm_open per request. */
    {
        int             fd;
        FpPoolRegistry *reg;

        if (pool_open(false, &fd, &reg) != 0) proc_exit(1);
        flock(fd, LOCK_EX);
        if (std::memcmp(reg->magic, kPoolMagic, sizeof(reg->magic)) != 0 || reg->version != kPoolVersion ||
            reg->slots[slot].state == 0)
        {
            /* the backend gave up on us (or another worker claimed it) */
            flock(fd, LOCK_UN);
            pool_close(fd, reg);
            proc_exit(1);
        }
        reg->slots[slot].pid   = static_cast<int32_t>(getpid());
        reg->slots[slot].state = 2;
        __atomic_store_n(&reg->slots[slot].busy, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&reg->slots[slot].last_service_ms, 0, __ATOMIC_RELEASE);
        flock(fd, LOCK_UN);

        pool_self_fd   = fd;
        pool_self_reg  = reg;
        pool_self_slot = slot;
    }

    std::memset(&set, 0, sizeof(set));
    pool_refresh(&set, t);
    last_tick = pool_now_ms();

    for (;;)
    {
        struct pollfd pfds[kPoolMaxRings];
        int           rc;

        if (got_sigterm) break;

        /* A periodic tick, not only on an idle poll returns: a worker under
         * sustained load would otherwise never look at the registry (and a new
         * ring queued on it would wait for a quiet moment that may not come),
         * nor notice that a client died. */
        if (pool_now_ms() - last_tick >= kRingWaitMs)
        {
            last_tick = pool_now_ms();
            pool_refresh(&set, t);
            for (int i = 0; i < set.n;)
            {
                FastpostgresRing *r = set.rings[i].r;

                if (r->op == FASTPOSTGRES_OP_STOP ||
                    (r->client_pid != 0 && kill(static_cast<pid_t>(r->client_pid), 0) != 0 && errno == ESRCH))
                {
                    pool_drop_ring(&set, i, &tx);
                    continue;
                }
                i++;
            }
        }

        if (set.n == 0)
        {
            if (empty_since == 0)
                empty_since = pool_now_ms();
            else if (pool_now_ms() - empty_since > kPoolExitMs)
                break;
        }
        else
            empty_since = 0;

        pool_publish_load(&set);

        for (int i = 0; i < set.n; i++)
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
            pg_usleep(static_cast<long>(kRingWaitMs) * 1000L);
            rc = 0;
        }
        else
            rc = poll(pfds, static_cast<nfds_t>(set.n), kRingWaitMs);

        if (rc < 0)
        {
            if (errno == EINTR) continue;
            elog(LOG, "fastpostgres_pool_main: poll failed: %m");
            break;
        }
        if (rc == 0) continue; /* the tick at the top of the loop does the chores */

        /* Serve every ring that is ready once per turn, starting after the last
         * one served. One request is one batch, and a batch is the executor's
         * unit of work, so that is the finest fair share a single backend can
         * offer; the round-robin start keeps the lowest index from always
         * winning the head of the loop. */
        {
            int  start   = (set.n > 0) ? (next_ring % set.n) : 0;
            bool dropped = false;
            bool fatal   = false;

            for (int i = 0; i < set.n; i++)
            {
                int               which = (start + i) % set.n;
                PoolRing         *pr;
                FastpostgresRing *r;
                uint64_t          asked;
                uint64_t          answered;
                uint64_t          drained;
                ssize_t           got;

                if (!(pfds[which].revents & (POLLIN | POLLERR | POLLHUP))) continue;

                pr = &set.rings[which];
                r  = pr->r;

                /* Drain the bell; one request is in flight per ring. A read
                 * that would block is a stale wake, and req_seq == resp_seq
                 * below confirms it. */
                do
                    got = read(r->worker_req_efd, &drained, sizeof(drained));
                while (got < 0 && errno == EINTR);
                if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                {
                    elog(LOG, "fastpostgres_pool_main: eventfd read failed: %m");
                    fatal = true;
                    break;
                }

                asked    = __atomic_load_n(&r->req_seq, __ATOMIC_ACQUIRE);
                answered = __atomic_load_n(&r->resp_seq, __ATOMIC_ACQUIRE);
                if (asked == answered) continue;

                r->status   = 0;
                r->nrows    = 0;
                r->error[0] = 0;

                if (!pool_handle(pr, &set, &tx))
                {
                    /* STOP: dropping swaps the last ring into `which`, so end
                     * this scan and let the next poll rebuild the set. */
                    next_ring = 0;
                    pool_drop_ring(&set, which, &tx);
                    dropped = true;
                    break;
                }
                if (t->post_response(r, asked) < 0)
                {
                    elog(LOG, "fastpostgres_pool_main: post_response failed: %m");
                    fatal = true;
                    break;
                }
            }
            if (fatal) break;
            if (dropped) continue;
            next_ring = (set.n > 0) ? (start + 1) % set.n : 0;
        }
    }

    pool_abort(&set, &tx);
    while (set.n > 0)
        pool_drop_ring(&set, 0, &tx);
    pool_unregister_all();
    if (pool_self_reg != nullptr)
    {
        pool_close(pool_self_fd, pool_self_reg);
        pool_self_reg = nullptr;
        pool_self_fd  = -1;
    }
    proc_exit(0);
}

/* The worker entry point (looked up by name, so it must be exported). */
extern "C" {
PGDLLEXPORT void fastpostgres_ring_main(Datum main_arg);
}

extern "C" PGDLLEXPORT void
fastpostgres_ring_main([[maybe_unused]] Datum main_arg)
{
    BackgroundWorker            *bw  = MyBgworkerEntry;
    const char                  *seg = bw->bgw_extra; /* "ring\0dbname\0transport\0" */
    const char                  *db  = seg + std::strlen(seg) + 1;
    const char                  *tname;
    const FastpostgresTransport *t;
    FastpostgresRing            *r;
    size_t                       rsize;

    pqsignal(SIGTERM, ring_sigterm);
    BackgroundWorkerUnblockSignals();

    /* The third NUL-separated field of bgw_extra names the engine. A ring
     * registered by a server that predates the field carries none, and that
     * is the shm ring it always was. */
    tname = db + std::strlen(db) + 1;
    if (tname >= bw->bgw_extra + BGW_EXTRALEN) tname = "";
    t = fastpostgres_transport_by_name(tname[0] ? tname : "shm");
    if (t == nullptr)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("fastpostgres_ring_main: unknown transport \"%s\"", tname)));

    BackgroundWorkerInitializeConnection(db[0] ? db : "postgres", nullptr, 0);

    r = ring_map_rw(seg, &rsize);
    if (r == nullptr)
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

    /* The data segment's backing store. When the client is listening for a
     * memfd handoff this connects to it before worker_ready, so the client's
     * accept after wait_ready finds the connection queued. */
    fp::shm_worker_begin(r);

    if (t->worker_init(r) < 0)
    {
        elog(LOG, "fastpostgres_ring_main: %s worker_init failed: %m", t->name);
        fp::shm_worker_end();
        munmap(r, rsize);
        proc_exit(1);
    }
    /* The pid first, then ready, both release: a reader that sees ready can
     * name the process serving this ring from the ring itself, which is how the
     * hosts outside read this side's CPU (/proc/<pid>/stat). */
    __atomic_store_n(&r->tally.worker_pid, static_cast<uint32_t>(getpid()), __ATOMIC_RELEASE);
    __atomic_store_n(&r->worker_ready, 1, __ATOMIC_RELEASE);

    for (;;)
    {
        uint64_t seq;
        int      rc;

        if (got_sigterm) break;

        rc = t->wait_request(r, &seq, kRingWaitMs);
        if (rc == 0)
        {
            /* Timeout: re-check SIGTERM, the client that owns this ring (gone
             * means there is nobody to serve), and the ring's op. The op check
             * makes a STOP whose doorbell post was missed visible here: the
             * stop path only posts the semaphore. */
            if (r->op == FASTPOSTGRES_OP_STOP) break;
            if (r->client_pid != 0 && kill(static_cast<pid_t>(r->client_pid), 0) != 0 && errno == ESRCH)
                break;
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
    fp::shm_worker_end();
    munmap(r, rsize);
    proc_exit(0);
}

/* ---- SQL: start / stop --------------------------------------------------- */

extern "C" {
PG_FUNCTION_INFO_V1(fastpostgres_ring_start);
}

extern "C" Datum
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
        if (t == nullptr)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("fastpostgres_ring_start: unknown transport \"%s\"", name)));
    }

    /* The ring's transport field is the segment's provenance. A v4 ring made
     * before the field was filled in reads zero and is stamped here; one
     * already stamped for a different engine is refused, because driving it
     * with the wrong doorbell would corrupt the session it is serving. */
    ring_transport = t->id;
    r              = ring_map_rw(seg, &rsize);
    if (r != nullptr)
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
        if (r == nullptr) elog(ERROR, "fastpostgres_ring_start: ring \"%s\" is not a segment", seg);
        if (!pool_ring_attach(seg, db, "eventfd"))
            elog(ERROR, "fastpostgres_ring_start: no pool worker accepted ring \"%s\"", seg);
        std::snprintf(summary, sizeof(summary), "ring=%s started", seg);
        PG_RETURN_TEXT_P(cstring_to_text(summary));
    }

    std::memset(&w, 0, sizeof(w));
    std::snprintf(w.bgw_name, BGW_MAXLEN, "fastpostgres ring");
    std::snprintf(w.bgw_type, BGW_MAXLEN, "fastpostgres ring worker");
    w.bgw_flags        = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    w.bgw_start_time   = BgWorkerStart_RecoveryFinished;
    w.bgw_restart_time = BGW_NEVER_RESTART;
    std::snprintf(w.bgw_library_name, BGW_MAXLEN, "fastpostgres");
    std::snprintf(w.bgw_function_name, BGW_MAXLEN, "fastpostgres_ring_main");
    w.bgw_notify_pid = MyProcPid;

    std::memset(w.bgw_extra, 0, BGW_EXTRALEN);
    slen = std::strlen(seg);
    dlen = std::strlen(db);
    tlen = std::strlen(t->name);
    if (slen + dlen + tlen + 3 > BGW_EXTRALEN) elog(ERROR, "fastpostgres_ring_start: ring name too long");
    std::memcpy(w.bgw_extra, seg, slen + 1);                       /* ring name */
    std::memcpy(w.bgw_extra + slen + 1, db, dlen + 1);             /* database name */
    std::memcpy(w.bgw_extra + slen + dlen + 2, t->name, tlen + 1); /* transport */

    if (!RegisterDynamicBackgroundWorker(&w, &handle))
        elog(ERROR, "fastpostgres_ring_start: could not register background worker");

    std::snprintf(summary, sizeof(summary), "ring=%s started", seg);
    PG_RETURN_TEXT_P(cstring_to_text(summary));
}

extern "C" {
PG_FUNCTION_INFO_V1(fastpostgres_ring_stop);
}

extern "C" Datum
fastpostgres_ring_stop(PG_FUNCTION_ARGS)
{
    char             *seg = text_to_cstring(PG_GETARG_TEXT_PP(0));
    size_t            rsize;
    FastpostgresRing *r = ring_map_rw(seg, &rsize);
    char              summary[256];

    if (r != nullptr)
    {
        r->op = FASTPOSTGRES_OP_STOP;
        __atomic_add_fetch(&r->req_seq, 1, __ATOMIC_RELEASE);
        /* The semaphore is the shm engine's doorbell. An eventfd ring's bell is
         * a process-local fd this backend does not own; the pool worker sees
         * the op on its next 100 ms tick, and the client's own stop() posts. */
        if (r->transport != FASTPOSTGRES_TRANSPORT_EVENTFD) sem_post(&r->req_ready);
        munmap(r, rsize);
    }

    std::snprintf(summary, sizeof(summary), "ring=%s stopping", seg);
    PG_RETURN_TEXT_P(cstring_to_text(summary));
}
