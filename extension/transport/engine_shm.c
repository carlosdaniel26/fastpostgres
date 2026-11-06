/*
 * engine_shm.c — the original transport: the ring's own semaphores carry the
 * wakeups, and the two sequence counters carry the truth. The client bumps
 * req_seq (release) and posts req_ready; the worker
 * waits on req_ready, answers, bumps resp_seq (release) and posts resp_ready.
 * This is a futex round trip and a scheduler wake per handoff — no privileges,
 * works everywhere — and it is exactly the protocol ring.cpp on the client
 * side already speaks, mirrored here so both sides keep one definition.
 *
 * The worker's cursor over the requests it has answered is read from the ring
 * in the shape the counters already have: req_seq is what the client has asked,
 * resp_seq is what this worker has answered, so an unanswered request is exactly
 * req_seq != resp_seq — the duplicate suppression, with no per-process state.
 *
 * Each wait is a bounded spin on the sequence counter followed by the
 * semaphore sleep. For a small batch the partner is normally still running
 * when the handoff is posted, so the spin catches it and the futex wake costs
 * nothing; anything longer falls through to sem_timedwait, which remains the
 * whole criterion. The tally records both halves: the sleeps taken and the
 * microseconds spent spinning before a fallback.
 */
#include "transport.h"

#include <errno.h>
#include <stdlib.h>
#include <time.h>

/* Bounded spin-then-sleep: before the semaphore sleep, poll the sequence
 * counter for up to this many microseconds. The ring is synchronous, so the
 * other side is normally already working when a handoff is posted; a short
 * spin avoids one futex sleep and scheduler wake on the common path, and the
 * semaphore still carries anything longer. FASTPOSTGRES_SPIN_US=0 disables it.
 *
 * The default is deliberately small: a wait that is going to be long pays the
 * whole budget in CPU, and a client that idles between requests must not burn
 * a core. At the 3k-row batch scale the partner's turn is ~250-300 us, so a
 * budget in that range removes both wakes; 300 us measured best on this host
 * and is the default.
 *
 * Only small batches spin. A batch large enough that the partner's turn is
 * milliseconds pays the whole budget for nothing and then sleeps anyway, so the
 * spin is skipped above FASTPOSTGRES_SPIN_MAX_BATCH rows (0 = no limit).
 * Set FASTPOSTGRES_SPIN_US=0 to return to a pure semaphore sleep; see
 * bench/NOTES.md. */
#define FASTPOSTGRES_SPIN_DEFAULT_US        300
#define FASTPOSTGRES_SPIN_MAX_BATCH_DEFAULT 16384

static int
shm_spin_us(uint32_t batch)
{
    static int budget    = -1;
    static int max_batch = -1;

    if (budget < 0)
    {
        const char *env = getenv("FASTPOSTGRES_SPIN_US");
        long        v   = env && env[0] ? strtol(env, NULL, 10) : FASTPOSTGRES_SPIN_DEFAULT_US;

        if (v < 0) v = 0;
        if (v > 1000000) v = 1000000; /* never spin for more than a second */
        budget = (int)v;

        env = getenv("FASTPOSTGRES_SPIN_MAX_BATCH");
        v   = env && env[0] ? strtol(env, NULL, 10) : FASTPOSTGRES_SPIN_MAX_BATCH_DEFAULT;
        if (v < 0) v = 0;
        if (v > 100000000) v = 100000000;
        max_batch = (int)v;
    }
    if (budget == 0) return 0;
    if (max_batch != 0 && batch > (uint32_t)max_batch) return 0;
    return budget;
}

static inline void
shm_cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#endif
}

#define SHM_SPIN_CHECK 128

/* Microseconds since `start`, using the monotonic clock (a vDSO read on Linux).
 * Called only every SHM_SPIN_CHECK iterations: clock_gettime per iteration would
 * cost more than the wait it is bounding. */
static long
shm_since_us(const struct timespec *start)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(now.tv_sec - start->tv_sec) * 1000000L + (now.tv_nsec - start->tv_nsec) / 1000L;
}

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
shm_client_init(FastpostgresRing *r)
{
    (void)r;
    return 1; /* the semaphores were initialised when the ring was made */
}

static void
shm_client_fini(FastpostgresRing *r)
{
    (void)r;
}

static int
shm_post_request(FastpostgresRing *r, uint64_t seq, int timeout_ms)
{
    (void)seq; /* the caller's value is the one this increment lands on: one request is in flight at a time */
    (void)timeout_ms;

    /* The release increment is what the worker's acquire load turns into a
     * request; the doorbell may arrive before or after it, which is why the
     * worker re-reads the counter and suppresses wakes that advanced nothing.
     * The handoff and its one post are counted where they are made. */
    (void)__atomic_add_fetch(&r->req_seq, 1, __ATOMIC_RELEASE);
    fastpostgres_ring_tally(&r->tally.client, 1, 1, 0, 0);
    if (sem_post(&r->req_ready) != 0) return -1;
    return 1;
}

static int
shm_wait_response(FastpostgresRing *r, uint64_t seq, int timeout_ms)
{
    struct timespec deadline;
    int             rc;
    int             spin_us = shm_spin_us(r->batch);

    if (timeout_ms <= 0 || spin_us > timeout_ms * 1000) spin_us = timeout_ms > 0 ? timeout_ms * 1000 : 0;

    shm_deadline(&deadline, timeout_ms);
    /* A bounded spin first: the worker is normally already running the request
     * we just posted, so the counter turns before a sleep would be worth it.
     * The semaphore remains the truth for anything the spin does not catch. */
    if (spin_us > 0)
    {
        struct timespec start;
        int             i     = 0;
        long            spent = 0;

        clock_gettime(CLOCK_MONOTONIC, &start);
        for (;;)
        {
            if (__atomic_load_n(&r->resp_seq, __ATOMIC_ACQUIRE) >= seq)
            {
                /* Everything the worker wrote before its release store is now
                 * visible. */
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                return 1;
            }
            shm_cpu_relax();
            if ((++i & (SHM_SPIN_CHECK - 1)) == 0)
            {
                spent = shm_since_us(&start);
                if (spent >= spin_us) break;
            }
        }
        fastpostgres_ring_tally(&r->tally.client, 0, 0, 0, (uint64_t)spent);
    }

    /* The futex sleep: reachable when the spin budget did not cover the turn,
     * so it is the client's `sleeps` on this engine. A spun response leaves its
     * post in the semaphore, so a wake is re-checked against the counter rather
     * than trusted: the counter is the truth, the post is only a hint. */
    for (;;)
    {
        fastpostgres_ring_tally(&r->tally.client, 0, 0, 1, 0);
        rc = sem_timedwait(&r->resp_ready, &deadline);
        if (rc != 0) return errno == ETIMEDOUT ? 0 : -1;
        if (__atomic_load_n(&r->resp_seq, __ATOMIC_ACQUIRE) >= seq)
        {
            /* Everything the worker wrote before its release post is now
             * visible. */
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            return 1;
        }
        /* a stale post from a response the spin already collected */
    }
}

static int
shm_wait_ready(FastpostgresRing *r, int timeout_ms)
{
    struct timespec start;
    struct timespec pause = {0, 1000000L}; /* 1 ms */

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
shm_worker_init(FastpostgresRing *r)
{
    (void)r;
    return 1;
}

static void
shm_worker_fini(FastpostgresRing *r)
{
    (void)r;
}

static int
shm_wait_request(FastpostgresRing *r, uint64_t *seq, int timeout_ms)
{
    struct timespec deadline;
    int             rc;
    int             spin_us = shm_spin_us(r->batch);

    if (timeout_ms <= 0 || spin_us > timeout_ms * 1000) spin_us = timeout_ms > 0 ? timeout_ms * 1000 : 0;

    shm_deadline(&deadline, timeout_ms);
    /* The mirror of the client's spin: after answering, the client is normally
     * decoding the batch we just published, so the next request is already on
     * its way. Spin for it rather than sleeping and waiting to be woken. */
    if (spin_us > 0)
    {
        struct timespec start;
        int             i     = 0;
        long            spent = 0;

        clock_gettime(CLOCK_MONOTONIC, &start);
        for (;;)
        {
            uint64_t asked    = __atomic_load_n(&r->req_seq, __ATOMIC_ACQUIRE);
            uint64_t answered = __atomic_load_n(&r->resp_seq, __ATOMIC_ACQUIRE);

            if (asked != answered)
            {
                *seq = asked;
                return 1;
            }
            shm_cpu_relax();
            if ((++i & (SHM_SPIN_CHECK - 1)) == 0)
            {
                spent = shm_since_us(&start);
                if (spent >= spin_us) break;
            }
        }
        fastpostgres_ring_tally(&r->tally.worker, 0, 0, 0, (uint64_t)spent);
    }

    for (;;)
    {
        uint64_t asked;
        uint64_t answered;

        fastpostgres_ring_tally(&r->tally.worker, 0, 0, 1, 0); /* one wait per turn, and a stale wake sleeps again */
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
shm_post_response(FastpostgresRing *r, uint64_t seq)
{
    /* Release: the worker's status/nrows/error writes must be visible to the
     * client before the response counter that lets it look at them. */
    __atomic_store_n(&r->resp_seq, seq, __ATOMIC_RELEASE);
    fastpostgres_ring_tally(&r->tally.worker, 1, 1, 0, 0); /* the answer, and its wake hint */
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
