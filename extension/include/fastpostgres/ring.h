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
 * Two transports share this block. `transport` says which one created it:
 *
 *   SHM     the doorbells are the two sem_t below (futex under the hood)
 *   REGIPC  the request travels through the regipc kernel module: the client
 *           calls, ring 0 writes the nine carriers into `regipc.leaf` (a page
 *           the worker pinned), and both sides poll the counters in this
 *           segment before falling back to the semaphores when idle
 *
 * The payload never travels through either doorbell: it is the data segment,
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
    "FPRING05" /* 05: the ring carries a tally per side; an old segment is re-made rather than misread */
#define FASTPOSTGRES_RING_VERSION 5u
#define FASTPOSTGRES_RING_BYTES   (8u << 20) /* 8 MiB control segment */
#define FASTPOSTGRES_RING_ARG     4096
#define FASTPOSTGRES_RING_ERR     256

/* Where the two tally rows sit in the segment (below). Appended after `arg`, so
 * every offset the readers already parse - the header, `batch`, the regipc
 * leaf's page - is unchanged; the assert in transport.c holds this literal to
 * the struct, and bench/ names it in its own reader. */
#define FASTPOSTGRES_RING_TALLY_OFFSET 4488u

/* The regipc state sits on its own page: the module pins pages, and a 88-byte
 * leaf straddling two of them would be rejected by pin_user_pages(). */
#define FASTPOSTGRES_RING_REGIPC_OFFSET 4096u

enum FastpostgresRingOp
{
    FASTPOSTGRES_OP_NONE      = 0,
    FASTPOSTGRES_OP_OPEN      = 1,
    FASTPOSTGRES_OP_NEXT      = 2, /* the columnar frame */
    FASTPOSTGRES_OP_CLOSE     = 3,
    FASTPOSTGRES_OP_STOP      = 5,
    FASTPOSTGRES_OP_NEXT_ROWS = 6 /* the row frame (falls back to columnar) */
};

/* The module's leaf, as laid out by struct regipc_leaf_result (regipc.h). It is
 * mirrored here so this header stays free of the kernel ABI; engine_regipc.c
 * includes both and asserts the two layouts agree. */
typedef struct FastpostgresRingLeaf
{
    uint32_t magic;    /* REGIPC_LEAF_MAGIC */
    uint32_t calls;    /* incremented by the module, in ring 0, once per call */
    uint64_t checksum; /* FNV-1a over the low 32 bits of every carrier */
    uint64_t values[9]; /* x0..x8 of the most recent call */
} FastpostgresRingLeaf;

/* What the two sides keep around the leaf: the worker's cursor over `calls`,
 * the response counter it publishes, and the pids the pairing needs. */
typedef struct FastpostgresRingRegipc
{
    FastpostgresRingLeaf leaf;
    uint64_t             worker_seen; /* the worker's cursor over leaf.calls */
    uint64_t             resp_seq;    /* the last response the worker published */
    uint32_t             worker_pid;  /* the callee: the client pairs with it */
    uint32_t             client_pid;  /* the caller */
    uint32_t             ready;       /* the worker registered with the module */
    uint32_t             pad;
} FastpostgresRingRegipc;

/* One side's handoff tally: the client's row counts what it posted, the
 * worker's what it answered. Only that side writes its own row, so the record
 * is two snapshots of this segment around a run — no shared counter, no lock.
 * The events are counted, never timed by an extra clock: `spin_us` comes from
 * the poll loops' own clock reads (see engine_regipc.c). */
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
    uint32_t transport;    /* FASTPOSTGRES_TRANSPORT_SHM | _REGIPC */
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

/* The regipc block, at a fixed page inside the same segment. */
static inline FastpostgresRingRegipc *
fastpostgres_ring_regipc(FastpostgresRing *r)
{
    return (FastpostgresRingRegipc *)((char *)r + FASTPOSTGRES_RING_REGIPC_OFFSET);
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
