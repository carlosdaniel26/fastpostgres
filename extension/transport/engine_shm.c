/*
 * engine_shm.c — the original transport, unchanged in behaviour: the ring's
 * own semaphores carry the wakeups, and the two sequence counters carry the
 * truth. The client bumps req_seq (release) and posts req_ready; the worker
 * waits on req_ready, answers, bumps resp_seq (release) and posts resp_ready.
 * This is a futex round trip and a scheduler wake per handoff — no privileges,
 * works everywhere — and it is exactly the protocol ring.cpp on the client
 * side already speaks, mirrored here so both sides keep one definition.
 *
 * The worker's cursor over the requests it has answered is not kept in its ctx
 * (the shm ctx has no fields: the engine needs no per-process state). It is
 * read from the ring instead, in the shape the counters already have: req_seq
 * is what the client has asked, resp_seq is what this worker has answered, so
 * an unanswered request is exactly req_seq != resp_seq. That is the same
 * duplicate suppression the worker used to do with a local cursor, moved into
 * the engine so the worker loop is engine-agnostic.
 */
#include "transport.h"

#include <errno.h>
#include <time.h>

/* Absolute CLOCK_REALTIME deadline for sem_timedwait, which only has an
 * absolute form; the hooks are given a relative budget. */
static void
shm_deadline(struct timespec *ts, int timeout_ms)
{
    if (timeout_ms < 0) timeout_ms = 0;
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += timeout_ms / 1000;
    ts->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L)
    {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

/* Milliseconds since `start`, for polls that must not overrun their budget. */
static long
shm_since_ms(const struct timespec *start)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(now.tv_sec - start->tv_sec) * 1000 + (long)(now.tv_nsec - start->tv_nsec) / 1000000L;
}

/* ---- client ------------------------------------------------------------- */

static int
shm_client_init(FastpostgresRing *r, void *ctx)
{
    (void)r;
    (void)ctx;
    return 1; /* the semaphores were initialised when the ring was made */
}

static void
shm_client_fini(FastpostgresRing *r, void *ctx)
{
    (void)r;
    (void)ctx;
}

static int
shm_post_request(FastpostgresRing *r, void *ctx, uint64_t seq, int timeout_ms)
{
    (void)ctx;
    (void)seq; /* the caller's value is the one this increment lands on: one request is in flight at a time */
    (void)timeout_ms;

    /* The release increment is what the worker's acquire load turns into a
     * request; the doorbell may arrive before or after it, which is why the
     * worker re-reads the counter and suppresses wakes that advanced nothing. */
    (void)__atomic_add_fetch(&r->req_seq, 1, __ATOMIC_RELEASE);
    if (sem_post(&r->req_ready) != 0) return -1;
    return 1;
}

static int
shm_wait_response(FastpostgresRing *r, void *ctx, uint64_t seq, int timeout_ms)
{
    struct timespec deadline;
    int             rc;

    (void)ctx;
    (void)seq; /* the semaphore is the whole criterion; status/nrows are read under the fence below */
    shm_deadline(&deadline, timeout_ms);
    rc = sem_timedwait(&r->resp_ready, &deadline);
    if (rc != 0) return errno == ETIMEDOUT ? 0 : -1;
    /* Everything the worker wrote before its release post is now visible. */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return 1;
}

static int
shm_wait_ready(FastpostgresRing *r, void *ctx, int timeout_ms)
{
    struct timespec start;
    struct timespec pause = {0, 1000000L}; /* 1 ms */

    (void)ctx;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;)
    {
        if (__atomic_load_n(&r->worker_ready, __ATOMIC_ACQUIRE) != 0) return 1;
        if (timeout_ms <= 0 || shm_since_ms(&start) >= timeout_ms) return 0;
        nanosleep(&pause, NULL); /* a signal only shortens this wait, the loop re-checks the clock */
    }
}

/* ---- worker ------------------------------------------------------------- */

static int
shm_worker_init(FastpostgresRing *r, void *ctx)
{
    (void)r;
    (void)ctx;
    return 1;
}

static void
shm_worker_fini(FastpostgresRing *r, void *ctx)
{
    (void)r;
    (void)ctx;
}

static int
shm_wait_request(FastpostgresRing *r, void *ctx, uint64_t *seq, int timeout_ms)
{
    struct timespec deadline;
    int             rc;

    (void)ctx;
    shm_deadline(&deadline, timeout_ms);
    for (;;)
    {
        uint64_t asked;
        uint64_t answered;

        rc = sem_timedwait(&r->req_ready, &deadline);
        if (rc != 0) return errno == ETIMEDOUT ? 0 : -1;

        asked    = __atomic_load_n(&r->req_seq, __ATOMIC_ACQUIRE);
        answered = __atomic_load_n(&r->resp_seq, __ATOMIC_ACQUIRE);
        if (asked != answered)
        {
            *seq = asked;
            return 1;
        }
        /* A stale wake: the doorbell rang but the last unanswered sequence is
         * still the one this worker already handled. Report nothing and keep
         * sleeping until the deadline, so the caller's SIGTERM and liveness
         * checks still run. */
    }
}

static int
shm_post_response(FastpostgresRing *r, void *ctx, uint64_t seq)
{
    (void)ctx;

    /* Release: the worker's status/nrows/error writes must be visible to the
     * client before the response counter that lets it look at them. */
    __atomic_store_n(&r->resp_seq, seq, __ATOMIC_RELEASE);
    if (sem_post(&r->resp_ready) != 0) return -1;
    return 1;
}

const FastpostgresTransport fastpostgres_transport_shm = {
    .name          = "shm",
    .id            = FASTPOSTGRES_TRANSPORT_SHM,
    .mechanism     = "the ring's own semaphores: a futex wake per handoff",
    .client_init   = shm_client_init,
    .client_fini   = shm_client_fini,
    .post_request  = shm_post_request,
    .wait_response = shm_wait_response,
    .wait_ready    = shm_wait_ready,
    .worker_init   = shm_worker_init,
    .worker_fini   = shm_worker_fini,
    .wait_request  = shm_wait_request,
    .post_response = shm_post_response,
};
