/*
 * engine_regipc.c — the regipc transport: the request travels through the
 * regipc kernel module, and both sides poll counters in this segment around
 * it.
 *
 * A request is one ioctl. The client puts op/seq/batch/data_bytes into x0..x8,
 * ring 0 reads them out of its own syscall frame and writes them into the leaf
 * page the worker pinned, and the worker process never runs for it: no
 * scheduler wake, no context switch, no CR3. The trade, stated plainly: two
 * syscalls per request instead of one futex wake — the CALL itself and a
 * sem_post(&r->req_ready) as the wake hint for a worker that already went to
 * sleep after spinning. The request's bytes are ordered by the syscall, not by
 * the semaphore: the client writes the ring's op/arg/batch before the call,
 * and the worker reads arg/op from the ring and the carriers as the ordered
 * copy of that write. The semaphore never carries payload.
 *
 * The worker registers the leaf as a callee and only then publishes ready,
 * because the client creates the ring before the worker exists and has no pid
 * to pair with until then: pairing is the client's wait_ready step alone.
 *
 * Nothing here uses PostgreSQL. The extension, the host test and the Python
 * client all compile this file, and the ring's leaf mirror is held to the
 * module's layout by the asserts below.
 */
#include <sys/syscall.h> /* __NR_ioctl, consumed by the vendored handoff asm */

#include "fastpostgres/regipc.h"

#include "transport.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * ring.h mirrors the module's leaf so it stays free of the kernel ABI; these
 * asserts are where the mirror is held to the real layout. If a field moved,
 * the module would write calls and carriers into bytes the worker reads as
 * something else, so this must fail at build time, not at call time.
 */
_Static_assert(offsetof(struct regipc_leaf_result, magic) == offsetof(FastpostgresRingLeaf, magic),
               "leaf.magic offset must match the module's struct regipc_leaf_result");
_Static_assert(offsetof(struct regipc_leaf_result, calls) == offsetof(FastpostgresRingLeaf, calls),
               "leaf.calls offset must match the module's struct regipc_leaf_result");
_Static_assert(offsetof(struct regipc_leaf_result, checksum) == offsetof(FastpostgresRingLeaf, checksum),
               "leaf.checksum offset must match the module's struct regipc_leaf_result");
_Static_assert(offsetof(struct regipc_leaf_result, values) == offsetof(FastpostgresRingLeaf, values),
               "leaf.values offset must match the module's struct regipc_leaf_result");
_Static_assert(sizeof(((struct regipc_leaf_result *)0)->values) ==
                   sizeof(((FastpostgresRingLeaf *)0)->values),
               "leaf.values must have the module's nine carriers");
_Static_assert(sizeof(struct regipc_leaf_result) == sizeof(FastpostgresRingLeaf),
               "the mirrored leaf must be the module's size");
_Static_assert(sizeof(FastpostgresRingLeaf) == 88,
               "the module's leaf is 88 bytes: magic, calls, checksum, x0..x8");
_Static_assert(REGIPC_CARRIERS == 9, "the carrier set is x0..x8; the leaf's values array must be that wide");

/* pin_user_pages() rejects a leaf that straddles two pages, and the module
 * pins exactly [leaf, leaf + sizeof(leaf)). The block starts a page of a
 * page-aligned mapping, so this is a property of the layout alone. */
_Static_assert(FASTPOSTGRES_RING_REGIPC_OFFSET % 4096u == 0u, "the regipc block must start a page");
_Static_assert(offsetof(FastpostgresRingRegipc, leaf) + sizeof(FastpostgresRingLeaf) <= 4096u,
               "the leaf must sit wholly inside one page");

/* How long each side polls the counters before it falls back to the
 * semaphore. A hundred microseconds covers the handoff when the other side is
 * on CPU; longer would waste the wake, shorter would sleep too eagerly. */
#define REGIPC_SPIN_US 100u

/* Relative milliseconds from now, on CLOCK_REALTIME: the clock sem_timedwait
 * sleeps against. */
static void
regipc_deadline(struct timespec *ts, int timeout_ms)
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

/* A CLOCK_MONOTONIC instant `usec` microseconds from now. The poll budgets are
 * intervals, and a clock the system may step must not stretch them. */
static void
regipc_after_us(struct timespec *ts, long usec)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_nsec += usec * 1000L;
    while (ts->tv_nsec >= 1000000000L)
    {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static int
regipc_expired(const struct timespec *end)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec > end->tv_sec || (now.tv_sec == end->tv_sec && now.tv_nsec >= end->tv_nsec);
}

static long
regipc_since_ms(const struct timespec *start)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(now.tv_sec - start->tv_sec) * 1000 + (long)(now.tv_nsec - start->tv_nsec) / 1000000L;
}

/* A spin that does not hammer the memory bus while it waits for the other
 * side's store to arrive. */
static void
regipc_pause(void)
{
#if defined(__x86_64__)
    __asm__ volatile("pause");
#endif
}

/* ---- shared teardown ---------------------------------------------------- */

static void
regipc_ctx_close(FastpostgresRegipcCtx *ctx)
{
    if (ctx->fd < 0) return;

    /* QUIT unlinks this side from its peer and unpins a callee's page, then
     * the fd goes: the caller is expected to leave first, but the module
     * unlinks both ends of the pair, so either order leaves nothing stale. */
    regipc_quit(ctx->fd);
    close(ctx->fd);
    ctx->fd     = -1;
    ctx->paired = 0;
}

/* ---- client ------------------------------------------------------------- */

static int
regipc_client_init(FastpostgresRing *r, void *vctx)
{
    FastpostgresRegipcCtx     *ctx = vctx;
    FastpostgresRingRegipc    *ip  = fastpostgres_ring_regipc(r);
    struct regipc_context_desc desc;
    int                        fd;
    int                        rc;

    fd = regipc_open();
    if (fd < 0)
    {
        errno = -fd; /* no module, no /dev/regipc: the caller asked for a transport this host cannot do */
        return -1;
    }

    /* PAIR and CALL only work from a process the module knows, so the caller
     * registers with nothing but its identity. Modules that predate that
     * shape insist every registration hands over a leaf page, and there the
     * client registers the ring's own leaf: the module pins it a second time
     * as the client's struct, but only the worker's copy is ever run by a
     * call, so the pinned duplicate stays inert. */
    memset(&desc, 0, sizeof(desc));
    rc = regipc_register(fd, &desc);
    if (rc == -EINVAL)
    {
        desc.data     = (uint64_t)(uintptr_t)&ip->leaf;
        desc.data_len = sizeof(ip->leaf);
        desc.flags    = REGIPC_F_CALLEE;
        rc            = regipc_register(fd, &desc);
    }
    if (rc < 0)
    {
        errno = -rc;
        close(fd);
        return -1;
    }

    ctx->fd      = fd;
    ctx->paired  = 0;
    ctx->spin_us = REGIPC_SPIN_US;
    ctx->handled = 0;
    ctx->seq     = 0;
    return 1;
}

static void
regipc_client_fini(FastpostgresRing *r, void *vctx)
{
    (void)r;
    regipc_ctx_close(vctx);
}

static int
regipc_wait_ready(FastpostgresRing *r, void *vctx, int timeout_ms)
{
    FastpostgresRegipcCtx  *ctx = vctx;
    FastpostgresRingRegipc *ip  = fastpostgres_ring_regipc(r);
    struct timespec         start;
    struct timespec         pause = {0, 1000000L}; /* 1 ms */

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;)
    {
        if (__atomic_load_n(&r->worker_ready, __ATOMIC_ACQUIRE) != 0)
        {
            uint32_t pid = __atomic_load_n(&ip->worker_pid, __ATOMIC_ACQUIRE);

            if (pid != 0)
            {
                int rc = regipc_pair(ctx->fd, (__s32)pid);

                if (rc == 0)
                {
                    ctx->paired = 1;
                    return 1;
                }
                /* The worker published ready after its REGISTER, but its
                 * child process can still be invisible to find_vpid() for an
                 * instant; -ESRCH is that race, not a verdict. Anything else
                 * (a full context table, an already-paired callee) is real. */
                if (rc != -ESRCH)
                {
                    errno = -rc;
                    return -1;
                }
            }
        }
        if (timeout_ms <= 0 || regipc_since_ms(&start) >= timeout_ms) return 0;
        nanosleep(&pause, NULL);
    }
}

static int
regipc_post_request(FastpostgresRing *r, void *vctx, uint64_t seq, int timeout_ms)
{
    FastpostgresRegipcCtx *ctx = vctx;
    struct regipc_carriers c;
    long                   ret;

    (void)timeout_ms; /* the ioctl runs the leaf and returns; it never waits on a peer */

    /* The nine carriers are the module's whole input. Fields the engine does
     * not use yet travel as zero, so a future module reading x4..x8 sees a
     * defined value rather than a stale register. */
    memset(&c, 0, sizeof(c));
    c.w[0]   = r->op;
    c.w[1]   = seq;
    c.w[2]   = r->batch;
    c.w[3]   = r->data_bytes;
    ctx->seq = seq;

    /* One call delivers the request and increments the callee's counter in
     * ring 0; when it returns, a spinning worker can already see the leaf. */
    ret = regipc_call(ctx->fd, &c);
    if (ret < 0)
    {
        errno = (int)-ret;
        return -1;
    }

    /* The second syscall's only job is to wake a worker that is asleep on the
     * semaphore after its spin expired. A failed post costs latency, not the
     * request: the leaf already holds it and the worker polls on timeout. */
    (void)sem_post(&r->req_ready);
    return 1;
}

static int
regipc_wait_response(FastpostgresRing *r, void *vctx, uint64_t seq, int timeout_ms)
{
    FastpostgresRegipcCtx *ctx = vctx;
    struct timespec        deadline;
    struct timespec        spin_end;
    int                    rc;

    regipc_deadline(&deadline, timeout_ms);
    regipc_after_us(&spin_end, (long)ctx->spin_us);

    /* Poll first: the answer may already be in the segment, and a load costs
     * a fraction of a syscall. The semaphore is deliberately not consumed
     * when the spin wins — its count is a hint, the sequence is the truth. */
    while (!regipc_expired(&spin_end))
    {
        if (__atomic_load_n(&r->resp_seq, __ATOMIC_ACQUIRE) >= seq) return 1;
        regipc_pause();
    }

    for (;;)
    {
        if (__atomic_load_n(&r->resp_seq, __ATOMIC_ACQUIRE) >= seq) return 1;
        rc = sem_timedwait(&r->resp_ready, &deadline);
        if (rc != 0) return errno == ETIMEDOUT ? 0 : -1;
        /* Woken by the count but not by the sequence: a stale post from an
         * earlier handoff, or a response to something older. Only the sequence
         * ending the wait means this request was answered. */
    }
}

/* ---- worker ------------------------------------------------------------- */

static int
regipc_worker_init(FastpostgresRing *r, void *vctx)
{
    FastpostgresRegipcCtx     *ctx = vctx;
    FastpostgresRingRegipc    *ip  = fastpostgres_ring_regipc(r);
    struct regipc_context_desc desc;
    uintptr_t                  off;
    int                        fd;
    int                        rc;

    fd = regipc_open();
    if (fd < 0)
    {
        errno = -fd;
        return -1;
    }

    /* The module pins the leaf page on REGISTER and rejects a leaf that
     * crosses a page boundary; mmap returned a page-aligned mapping and the
     * block starts a page, so this can only fail on an odd mapping, which the
     * engine then must not drive. */
    off = (uintptr_t)&ip->leaf & 4095u;
    if (off + sizeof(ip->leaf) > 4096u)
    {
        close(fd);
        errno = EINVAL;
        return -1;
    }

    /* What the module reads on the very first call must be in place before
     * the call can happen: magic says "this page is a leaf", and the checksum
     * starts at FNV-1a's offset basis and accumulates the carriers in order. */
    memset(ip, 0, sizeof(*ip));
    ip->leaf.magic    = REGIPC_LEAF_MAGIC;
    ip->leaf.checksum = 14695981039346656037ull;

    memset(&desc, 0, sizeof(desc));
    desc.data     = (uint64_t)(uintptr_t)&ip->leaf;
    desc.data_len = sizeof(ip->leaf);
    desc.flags    = REGIPC_F_CALLEE;
    rc            = regipc_register(fd, &desc);
    if (rc < 0)
    {
        errno = -rc;
        close(fd);
        return -1;
    }

    ctx->fd      = fd;
    ctx->paired  = 0;
    ctx->spin_us = REGIPC_SPIN_US;
    ctx->handled = 0;
    ctx->seq     = 0;

    /* The pid first, then ready, both release: the client reads ready, then
     * the pid, and only then pairs, so a visible ready always names a pid the
     * module can already resolve. */
    __atomic_store_n(&ip->worker_pid, (uint32_t)getpid(), __ATOMIC_RELEASE);
    __atomic_store_n(&ip->ready, 1u, __ATOMIC_RELEASE);
    return 1;
}

static void
regipc_worker_fini(FastpostgresRing *r, void *vctx)
{
    (void)r;
    regipc_ctx_close(vctx);
}

static int
regipc_wait_request(FastpostgresRing *r, void *vctx, uint64_t *seq, int timeout_ms)
{
    FastpostgresRegipcCtx  *ctx = vctx;
    FastpostgresRingRegipc *ip  = fastpostgres_ring_regipc(r);
    struct timespec         deadline;
    struct timespec         spin_end;
    uint32_t                calls;

    regipc_deadline(&deadline, timeout_ms);
    regipc_after_us(&spin_end, (long)ctx->spin_us);

    /* Poll first: the module wrote the leaf from the caller's own syscall, so
     * when this worker is on CPU the request may already be waiting and no
     * wake is needed. */
    calls = __atomic_load_n(&ip->leaf.calls, __ATOMIC_ACQUIRE);
    while ((uint64_t)calls == ctx->handled && !regipc_expired(&spin_end))
    {
        regipc_pause();
        calls = __atomic_load_n(&ip->leaf.calls, __ATOMIC_ACQUIRE);
    }
    if ((uint64_t)calls == ctx->handled)
    {
        /* The idle path: the client posts the semaphore right after its call,
         * but the count is a hint. A wake that did not advance the leaf is
         * stale and must not be reported as a request, or the worker would
         * re-run the last op against a stale arg. */
        for (;;)
        {
            int rc = sem_timedwait(&r->req_ready, &deadline);

            if (rc != 0) return errno == ETIMEDOUT ? 0 : -1;
            calls = __atomic_load_n(&ip->leaf.calls, __ATOMIC_ACQUIRE);
            if ((uint64_t)calls != ctx->handled) break;
        }
    }

    /* The callee's cursor and the request's identity. values[0] is the ring's
     * op as the client wrote it; if it disagrees with the ring the caller and
     * the carriers are out of sync, and running the ring on a guess would run
     * the wrong statement — refuse instead. */
    ctx->handled = calls;
    *seq         = ip->leaf.values[1];
    if (ip->leaf.values[0] != r->op)
    {
        errno = EPROTO;
        return -1;
    }
    return 1;
}

static int
regipc_post_response(FastpostgresRing *r, void *vctx, uint64_t seq)
{
    FastpostgresRingRegipc *ip = fastpostgres_ring_regipc(r);

    (void)vctx;

    /* Both counters, release: the client polls the ring's copy and the block's
     * copy is this engine's own record. The semaphore is the hint for a client
     * that already went to sleep after its spin. */
    __atomic_store_n(&ip->resp_seq, seq, __ATOMIC_RELEASE);
    __atomic_store_n(&r->resp_seq, seq, __ATOMIC_RELEASE);
    if (sem_post(&r->resp_ready) != 0) return -1;
    return 1;
}

const FastpostgresTransport fastpostgres_transport_regipc = {
    .name          = "regipc",
    .id            = FASTPOSTGRES_TRANSPORT_REGIPC,
    .mechanism     = "a cross-process call through /dev/regipc: no scheduler wake, two syscalls per request",
    .client_init   = regipc_client_init,
    .client_fini   = regipc_client_fini,
    .post_request  = regipc_post_request,
    .wait_response = regipc_wait_response,
    .wait_ready    = regipc_wait_ready,
    .worker_init   = regipc_worker_init,
    .worker_fini   = regipc_worker_fini,
    .wait_request  = regipc_wait_request,
    .post_response = regipc_post_response,
};
