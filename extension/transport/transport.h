/*
 * transport.h — the seam between the ring protocol and the mechanism that
 * notifies the two sides. The shm engine implements it:
 *
 *   shm     the ring's own semaphores: sem_post / sem_timedwait, a futex
 *           round trip and a scheduler wake per handoff (futexes, no
 *           privileges, works everywhere)
 *   eventfd two eventfds per ring, pollable, so one worker can wait on many
 *           rings in one poll(); the fds travel to the worker over SCM_RIGHTS
 *           on an abstract Unix socket the client owns (engine_eventfd.c)
 *
 * Everything else in the ring — the ops, `arg`, the batch, status/nrows/error,
 * the data segment and its publish barriers — is transport-neutral and stays
 * where it was. The payload is never carried by a doorbell.
 *
 * Each hook returns 1 on success, 0 on timeout and -1 on error, and none of
 * them may block for longer than the timeout it is given.
 */
#pragma once

#include <stdint.h>

#include "fastpostgres/ring.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FASTPOSTGRES_TRANSPORT_SHM     1u
#define FASTPOSTGRES_TRANSPORT_EVENTFD 2u

typedef struct FastpostgresTransport {
    const char *name;      /* "shm": the FASTPOSTGRES_TRANSPORT value */
    uint32_t    id;        /* FASTPOSTGRES_TRANSPORT_*: stored in the ring */
    const char *mechanism; /* one line for the logs and the records */

    /* --- client (the side that asks) --- */
    int (*client_init)(FastpostgresRing *r);
    void (*client_fini)(FastpostgresRing *r);
    int (*post_request)(FastpostgresRing *r, uint64_t seq, int timeout_ms);
    int (*wait_response)(FastpostgresRing *r, uint64_t seq, int timeout_ms);
    int (*wait_ready)(FastpostgresRing *r, int timeout_ms);

    /* --- worker (the side that answers) --- */
    int (*worker_init)(FastpostgresRing *r);
    void (*worker_fini)(FastpostgresRing *r);
    int (*wait_request)(FastpostgresRing *r, uint64_t *seq, int timeout_ms);
    int (*post_response)(FastpostgresRing *r, uint64_t seq);
} FastpostgresTransport;

/* "shm" resolves; anything else returns NULL. */
const FastpostgresTransport *fastpostgres_transport_by_name(const char *name);

/* A ring written by another version or another engine is refused, not driven. */
int fastpostgres_transport_check(const FastpostgresRing *r, const FastpostgresTransport *t);

extern const FastpostgresTransport fastpostgres_transport_shm;
extern const FastpostgresTransport fastpostgres_transport_eventfd;

/* The eventfd engine's one step the generic seam cannot express: the fds are
 * process-local, so the worker receives its copies from the client over
 * SCM_RIGHTS. Both are called with the ring mapped by the side that owns it.
 * attach() is worker-side and blocks for the client's handshake (bounded). */
int  fastpostgres_eventfd_attach(FastpostgresRing *r);
void fastpostgres_eventfd_detach(FastpostgresRing *r);

#ifdef __cplusplus
}
#endif
