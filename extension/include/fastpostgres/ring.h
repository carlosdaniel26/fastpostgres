/*
 * ring.h — request/response ring shared between the client and the
 * background worker (no socket). The client creates /dev/shm/<seg>, zeroes it
 * and initialises the semaphores before bootstrapping the worker, so there is
 * no init race; the worker only maps it.
 *
 * Reads only: one OPEN/NEXT/CLOSE request at a time, the batch landing in a
 * separate segment. Writes, DDL and transactions stay on libpq.
 *
 * That data segment is the worker's to create and size, so the client says how
 * big it wants it in the ring (data_bytes) before starting the worker.
 */
#ifndef FASTPOSTGRES_RING_H
#define FASTPOSTGRES_RING_H

#include <semaphore.h>
#include <stdint.h>
#include <string.h>

#define FASTPOSTGRES_RING_MAGIC                                                                              \
    "FPRING03" /* 03: the ring carries the data-segment capacity; an old segment is re-made */
#define FASTPOSTGRES_RING_VERSION 3u
#define FASTPOSTGRES_RING_BYTES   (8u << 20) /* 8 MiB control segment */
#define FASTPOSTGRES_RING_ARG     4096
#define FASTPOSTGRES_RING_ERR     256

enum FastpostgresRingOp
{
    FASTPOSTGRES_OP_NONE      = 0,
    FASTPOSTGRES_OP_OPEN      = 1,
    FASTPOSTGRES_OP_NEXT      = 2, /* the columnar frame */
    FASTPOSTGRES_OP_CLOSE     = 3,
    FASTPOSTGRES_OP_STOP      = 5,
    FASTPOSTGRES_OP_NEXT_ROWS = 6 /* the row frame (falls back to columnar) */
};

typedef struct FastpostgresRing {
    char     magic[8];     /* "FPRING03" */
    uint32_t version;
    uint32_t worker_ready; /* release-store once the worker has mapped it */
    uint32_t client_pid;   /* owning client: the worker exits once it is gone */
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
} FastpostgresRing;

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
