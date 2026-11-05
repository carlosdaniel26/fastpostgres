/*
 * ring.h — request/response ring shared between the client and the
 * background worker (no socket). The client creates /dev/shm/<seg>, zeroes it
 * and initialises the doorbells before bootstrapping the worker, so there is
 * no init race; the worker only maps it.
 *
 * Reads only: one OPEN/NEXT/CLOSE request at a time, the batch landing in a
 * separate segment. Writes, DDL and transactions stay on libpq.
 *
 * That data segment is the worker's to create and size, so the client says how
 * big it wants it in the ring (data_bytes) before starting the worker.
 *
 * `transport` says which engine created the block: FASTPOSTGRES_TRANSPORT_SHM,
 * whose doorbells are the two sem_t below (futex under the hood), or
 * FASTPOSTGRES_TRANSPORT_EVENTFD, whose fds are pollable so one worker can
 * serve several rings (extension/transport/engine_eventfd.c).
 *
 * The payload never travels through a doorbell: it is the data segment,
 * published by fastpostgres_shm_publish() and read in place. A transport is a
 * notification mechanism, not a data path.
 *
 * Both sides also keep a tally here (FastpostgresRingTally): the handoffs, the
 * semaphore posts, the falls back to the semaphore and the microseconds spent
 * polling. A reader takes the two rows before and after a run; that diff is
 * what a change of wait policy would have to move.
 */
#ifndef FASTPOSTGRES_RING_H
#define FASTPOSTGRES_RING_H

#include <semaphore.h>
#include <stdint.h>
#include <string.h>

#define FASTPOSTGRES_RING_MAGIC                                                                              \
    "FPRING06" /* 06: the eventfd engine's fds and the ring's own name follow the tally */
#define FASTPOSTGRES_RING_VERSION 6u
#define FASTPOSTGRES_RING_BYTES   (8u << 20) /* 8 MiB control segment */
#define FASTPOSTGRES_RING_ARG     4096
#define FASTPOSTGRES_RING_ERR     256

/* Where the two tally rows sit in the segment (below). Appended after `arg`, so
 * every offset the readers already parse - the header, `batch`, `arg` - is
 * unchanged; the assert in transport.c holds this literal to the struct, and
 * bench/ names it in its own reader. */
#define FASTPOSTGRES_RING_TALLY_OFFSET 4488u

enum FastpostgresRingOp
{
    FASTPOSTGRES_OP_NONE      = 0,
    FASTPOSTGRES_OP_OPEN      = 1,
    FASTPOSTGRES_OP_NEXT      = 2, /* the columnar frame */
    FASTPOSTGRES_OP_CLOSE     = 3,
    FASTPOSTGRES_OP_STOP      = 5,
    FASTPOSTGRES_OP_NEXT_ROWS = 6 /* the row frame (falls back to columnar) */
};

/* One side's handoff tally: the client's row counts what it posted, the
 * worker's what it answered. Only that side writes its own row, so the record
 * is two snapshots of this segment around a run — no shared counter, no lock.
 * The events are counted, never timed by an extra clock: the shm engine never
 * polls, so it leaves `spin_us` at zero. */
typedef struct FastpostgresRingSide
{
    uint64_t handoffs; /* client: requests posted; worker: requests answered */
    uint64_t posts;    /* semaphore posts made: the wake hint sent to the other side */
    uint64_t sleeps;   /* fallbacks to the semaphore: the wait a policy change would remove */
    uint64_t spin_us;  /* microseconds spent polling a counter before that fallback */
} FastpostgresRingSide;

/* What a change of wait policy has to move: both rows, and the pid whose CPU
 * /proc/<pid>/stat holds (the worker's; the client's is the reader's own). */
typedef struct FastpostgresRingTally
{
    FastpostgresRingSide client;
    FastpostgresRingSide worker;
    uint32_t             worker_pid; /* the callee, published before worker_ready */
    uint32_t             pad;
} FastpostgresRingTally;

typedef struct FastpostgresRing
{
    char     magic[8];     /* "FPRING05" */
    uint32_t version;
    uint32_t worker_ready; /* release-store once the worker has mapped it */
    uint32_t client_pid;   /* owning client: the worker exits once it is gone */
    uint32_t transport;    /* FASTPOSTGRES_TRANSPORT_SHM */
    uint64_t data_bytes;   /* capacity the client asks for a data segment (0 = FASTPOSTGRES_SEGMENT_BYTES) */
    uint64_t req_seq;      /* bumped by the client per request */
    uint64_t resp_seq;     /* bumped by the worker per response */
    sem_t    req_ready;    /* client -> worker */
    sem_t    resp_ready;   /* worker -> client */
    uint32_t op;           /* FastpostgresRingOp */
    uint32_t status;       /* 0 = ok */
    int64_t  nrows;
    uint32_t batch;
    char     error[FASTPOSTGRES_RING_ERR];
    char     arg[FASTPOSTGRES_RING_ARG]; /* "<seg>\0<query|table>\0" */
    FastpostgresRingTally tally; /* appended: FASTPOSTGRES_RING_TALLY_OFFSET */

    /* The eventfd engine's worker half, appended after the tally so the
     * offsets the readers already parse do not move. The client's own fds are
     * process-local and live beside its mapping (engine_eventfd.c); only the
     * worker's received copies are published here, and only that worker reads
     * them. */
    char     ring_name[64]; /* the client writes it before the worker starts */
    int32_t  worker_req_efd;  /* the worker's received copy of the request bell */
    int32_t  worker_resp_efd; /* the worker's received copy of the response bell */
    uint32_t srv_pid;         /* the worker that attached this ring */
} FastpostgresRing;

/* One side's costs, added to its row where the engine ran the event. Relaxed
 * adds: the row is a record read as a diff, not a way to agree on an order. */
static inline void
fastpostgres_ring_tally(FastpostgresRingSide *side, uint64_t handoffs, uint64_t posts, uint64_t sleeps,
                        uint64_t spin_us)
{
    if (handoffs != 0) __atomic_add_fetch(&side->handoffs, handoffs, __ATOMIC_RELAXED);
    if (posts != 0) __atomic_add_fetch(&side->posts, posts, __ATOMIC_RELAXED);
    if (sleeps != 0) __atomic_add_fetch(&side->sleeps, sleeps, __ATOMIC_RELAXED);
    if (spin_us != 0) __atomic_add_fetch(&side->spin_us, spin_us, __ATOMIC_RELAXED);
}

/* The request payload every op carries: "<a1>\0<a2>\0" in `arg`. The client
 * packs with these and the worker unpacks with them, so the convention has a
 * single definition instead of a matching pair of strlen dances. */
static inline int
fastpostgres_ring_arg_set(char *arg, const char *a1, const char *a2)
{
    size_t n = a1 ? strlen(a1) + 1 : 1;
    size_t m = a2 ? strlen(a2) + 1 : 1;

    if (n + m > FASTPOSTGRES_RING_ARG) return -1;
    if (a1)
        memcpy(arg, a1, n);
    else
        arg[0] = 0;
    if (a2)
        memcpy(arg + n, a2, m);
    else
        arg[n] = 0;
    return 0;
}

static inline const char *
fastpostgres_ring_arg1(const char *arg)
{
    return arg;
}

static inline const char *
fastpostgres_ring_arg2(const char *arg)
{
    return arg + strlen(arg) + 1;
}

#endif
