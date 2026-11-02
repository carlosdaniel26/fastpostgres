/*
 * transport.h — the seam between the ring protocol and the mechanism that
 * notifies the two sides. Two engines implement it:
 *
 *   shm     the ring's own semaphores: sem_post / sem_timedwait, a futex
 *           round trip and a scheduler wake per handoff (futuress, no
 *           privileges, works everywhere)
 *   regipc  a cross-process call through the regipc kernel module: the client
 *           puts the request's fields in x0..x8, one ioctl, ring 0 writes them
 *           into a page the worker pinned, and the value comes back in RAX.
 *           No wake, no context switch, no syscall beyond the ioctl; both sides
 *           poll this segment's counters and only sleep when idle
 *
 * Everything else in the ring — the ops, `arg`, the batch, status/nrows/error,
 * the data segment and its publish barriers — is transport-neutral and stays
 * where it was. The payload is never carried by a doorbell.
 *
 * Each hook returns 1 on success, 0 on timeout and -1 on error, and none of
 * them may block for longer than the timeout it is given.
 */
#ifndef FASTPOSTGRES_TRANSPORT_H
#define FASTPOSTGRES_TRANSPORT_H

#include <stdint.h>

#include "fastpostgres/ring.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FASTPOSTGRES_TRANSPORT_SHM    1u
#define FASTPOSTGRES_TRANSPORT_REGIPC 2u

/* The per-process state each side owns and passes back to the hooks. Nothing in
 * here is shared: the regipc fd is per-process by nature, the spin budget is what
 * that process settled on, and the cursors are that side's own view. */
typedef struct FastpostgresRegipcCtx {
    int      fd;      /* /dev/regipc, -1 when not open */
    int      paired;  /* client: REGIPC_IOC_PAIR returned 0 */
    uint32_t spin_us; /* spin budget before falling back to the semaphore */
    uint64_t handled; /* worker: the leaf.calls already handled */
    uint64_t seq;     /* client: the request sequence being waited for */
} FastpostgresRegipcCtx;

typedef struct FastpostgresShmCtx {
    int unused;
} FastpostgresShmCtx;

typedef struct FastpostgresTransport {
    const char *name;      /* "shm" | "regipc": the FASTPOSTGRES_TRANSPORT value */
    uint32_t    id;        /* FASTPOSTGRES_TRANSPORT_*: stored in the ring */
    const char *mechanism; /* one line for the logs and the records */

    /* `ctx` is the calling process's own state, never shared: the regipc fd,
     * a cursor, the spin budget it settled on. The engine allocates nothing
     * itself; the caller owns the storage and passes the same pointer back. */

    /* --- client (the side that asks; the regipc caller) --- */
    int (*client_init)(FastpostgresRing *r, void *ctx);
    void (*client_fini)(FastpostgresRing *r, void *ctx);
    int (*post_request)(FastpostgresRing *r, void *ctx, uint64_t seq, int timeout_ms);
    int (*wait_response)(FastpostgresRing *r, void *ctx, uint64_t seq, int timeout_ms);
    int (*wait_ready)(FastpostgresRing *r, void *ctx, int timeout_ms);

    /* --- worker (the side that answers; the regipc callee) --- */
    int (*worker_init)(FastpostgresRing *r, void *ctx);
    void (*worker_fini)(FastpostgresRing *r, void *ctx);
    int (*wait_request)(FastpostgresRing *r, void *ctx, uint64_t *seq, int timeout_ms);
    int (*post_response)(FastpostgresRing *r, void *ctx, uint64_t seq);
} FastpostgresTransport;

/* "shm" and "regipc" resolve; anything else returns NULL. */
const FastpostgresTransport *fastpostgres_transport_by_name(const char *name);

/* A ring written by another version or another engine is refused, not driven. */
int fastpostgres_transport_check(const FastpostgresRing *r, const FastpostgresTransport *t);

extern const FastpostgresTransport fastpostgres_transport_shm;
extern const FastpostgresTransport fastpostgres_transport_regipc;

#ifdef __cplusplus
}
#endif

#endif
