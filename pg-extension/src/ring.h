/*
 * ring.h — request/response ring shared between the client and the
 * background worker (no socket). The client creates /dev/shm/<seg>, zeroes it
 * and initialises both semaphores before bootstrapping the worker, so there is
 * no init race; the worker only maps it.
 */
#ifndef NP_RING_H
#define NP_RING_H

#include <semaphore.h>
#include <stdint.h>

#define NP_RING_MAGIC "NPRING01"
#define NP_RING_VERSION 1u
#define NP_RING_BYTES (1u << 20) /* 1 MiB */
#define NP_RING_ARG 4096
#define NP_RING_ERR 256

enum NpRingOp {
  NP_OP_NONE = 0,
  NP_OP_OPEN = 1,
  NP_OP_NEXT = 2,
  NP_OP_CLOSE = 3,
  NP_OP_WRITE = 4,
  NP_OP_STOP = 5
};

typedef struct NpRing {
  char magic[8];        /* "NPRING01" */
  uint32_t version;
  uint32_t worker_ready;/* release-store once the worker has mapped it */
  uint64_t req_seq;     /* bumped by the client per request */
  uint64_t resp_seq;    /* bumped by the worker per response */
  sem_t req_ready;      /* client -> worker */
  sem_t resp_ready;     /* worker -> client */
  uint32_t op;          /* NpRingOp */
  uint32_t status;      /* 0 = ok */
  int64_t nrows;
  uint32_t batch;
  char error[NP_RING_ERR];
  char arg[NP_RING_ARG];/* "<seg>\0<query|table>\0" */
} NpRing;

#endif
