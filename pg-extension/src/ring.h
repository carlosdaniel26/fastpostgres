/*
 * ring.h — request/response ring shared between the client and the
 * background worker (no socket). The client creates /dev/shm/<seg>, zeroes it
 * and initialises the semaphores before bootstrapping the worker, so there is
 * no init race; the worker only maps it.
 *
 * Columnar mode: one OPEN/NEXT/CLOSE/WRITE request at a time, data lands in a
 * separate columnar segment.
 *
 * Push mode: the worker runs the query with a DestReceiver and writes each row
 * into a circular slot queue as the executor produces it; the client turns each
 * row into a JS object immediately. Slot layout:
 *   [u8 status][u8 nulls[ceil(ncols/8)]][values...]
 * where status 0 = row, 1 = done, 2 = error (message in push_error).
 */
#ifndef FASTPOSTGRES_RING_H
#define FASTPOSTGRES_RING_H

#include <semaphore.h>
#include <stdint.h>

#define FASTPOSTGRES_RING_MAGIC   "FPRING01"
#define FASTPOSTGRES_RING_VERSION 1u
#define FASTPOSTGRES_RING_BYTES   (8u << 20) /* 8 MiB segment */
#define FASTPOSTGRES_RING_ARG     4096
#define FASTPOSTGRES_RING_ERR     256

#define FASTPOSTGRES_PUSH_OFFSET  8192u  /* slot area starts here */
#define FASTPOSTGRES_PUSH_SLOTS   64u    /* ring depth */
#define FASTPOSTGRES_PUSH_SLOT    65536u /* bytes per row frame */
#define FASTPOSTGRES_PUSH_MAXCOLS 64
#define FASTPOSTGRES_PUSH_NAME    32

enum FastpostgresRingOp
{
    FASTPOSTGRES_OP_NONE  = 0,
    FASTPOSTGRES_OP_OPEN  = 1,
    FASTPOSTGRES_OP_NEXT  = 2,
    FASTPOSTGRES_OP_CLOSE = 3,
    FASTPOSTGRES_OP_WRITE = 4,
    FASTPOSTGRES_OP_STOP  = 5,
    FASTPOSTGRES_OP_PUSH  = 6
};

/* push slot status */
enum
{
    FASTPOSTGRES_PUSH_ROW   = 0,
    FASTPOSTGRES_PUSH_DONE  = 1,
    FASTPOSTGRES_PUSH_ERROR = 2
};

typedef struct FastpostgresRing {
    char     magic[8];     /* "FPRING01" */
    uint32_t version;
    uint32_t worker_ready; /* release-store once the worker has mapped it */
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

    /* push (row-by-row) mode */
    sem_t    push_free;   /* free slots: client -> worker */
    sem_t    push_ready;  /* filled slots: worker -> client */
    uint32_t push_ncols;
    uint32_t push_status; /* 1 = the stream failed */
    char     push_error[FASTPOSTGRES_RING_ERR];
    struct {
        char     name[FASTPOSTGRES_PUSH_NAME];
        uint8_t  kind;
        uint32_t pgtype;
    } push_cols[FASTPOSTGRES_PUSH_MAXCOLS];
} FastpostgresRing;

#endif
