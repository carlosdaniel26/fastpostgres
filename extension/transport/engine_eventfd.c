/*
 * engine_eventfd.c — the same ring protocol, woken by eventfds instead of the
 * ring's two semaphores.
 *
 * Why: an eventfd is pollable. A semaphore gives one waiter one wake and cannot
 * be put in a poll() set, so a process wanting to serve several rings has to
 * sleep on one semaphore at a time, and the server needs one background worker
 * per ring. With an eventfd the same worker can poll every ring it serves and
 * dispatch whichever rang, which is what lets a small pool of workers carry
 * more rings than max_worker_processes has slots.
 *
 * The fds are process-local, so the client creates the pair (it also creates
 * the segment) and hands the worker its copies over SCM_RIGHTS on an abstract
 * Unix socket named after the ring. The client's listener is opened in
 * client_init, before the bootstrap SQL call; the worker connects after it has
 * mapped the segment, and wait_ready accepts and sends the pair. Nothing on the
 * data path travels over that socket: like the semaphores, the eventfds are
 * doorbells only. The payload is still the data segment and the sequence
 * counters are still the truth (req_seq != resp_seq), so a coalesced or stale
 * wake is suppressed exactly as in engine_shm.c.
 */
#include "transport.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define EFD_SOCK_PREFIX "fastpostgres_fd_"

/* The client's fds are process-local, and the ring header is shared: a forked
 * child dropping an inherited Ring, or a Ring object deallocated after a newer
 * one took its name, must not close a number that now belongs to somebody else.
 * So the client keeps its fds beside the mapping, keyed by its address, and
 * never reads them back out of the shared header. The worker's copies stay in
 * the header: only the one worker that attached the ring ever touches them. */
#define EFD_LOCAL_MAX 64

typedef struct EfdLocal
{
    FastpostgresRing *ring;
    int               req;
    int               resp;
    int               listen;
} EfdLocal;

static EfdLocal efd_local[EFD_LOCAL_MAX];
static int      efd_local_lock; /* a spin around the table only: no syscalls */

static void
efd_local_enter(void)
{
    while (__atomic_exchange_n(&efd_local_lock, 1, __ATOMIC_ACQUIRE)) /* spin */;
}

static void
efd_local_leave(void)
{
    __atomic_store_n(&efd_local_lock, 0, __ATOMIC_RELEASE);
}

static EfdLocal *
efd_local_take(FastpostgresRing *r, bool create)
{
    EfdLocal *slot = NULL;
    int       i;

    efd_local_enter();
    for (i = 0; i < EFD_LOCAL_MAX; i++)
    {
        if (efd_local[i].ring == r)
        {
            slot = &efd_local[i];
            break;
        }
        if (create && slot == NULL && efd_local[i].ring == NULL) slot = &efd_local[i];
    }
    if (slot != NULL && create && slot->ring == NULL)
    {
        slot->ring   = r;
        slot->req    = -1;
        slot->resp   = -1;
        slot->listen = -1;
    }
    efd_local_leave();
    return slot;
}

/* A relative budget turned into a monotonic deadline: poll takes a relative
 * timeout and each retry must get the remainder, not the original budget. */
static long
efd_since_ms(const struct timespec *start)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(now.tv_sec - start->tv_sec) * 1000 + (long)(now.tv_nsec - start->tv_nsec) / 1000000L;
}

static int
efd_remaining_ms(const struct timespec *start, int timeout_ms)
{
    long spent = efd_since_ms(start);

    if (spent >= timeout_ms) return 0;
    return timeout_ms - (int)spent;
}

/* One write to the bell. EAGAIN means the counter is already non-zero, so the
 * other side has a wake pending: that is a success for a doorbell. */
static int
efd_signal(int fd)
{
    uint64_t one = 1;
    ssize_t  n;

    do
        n = write(fd, &one, sizeof(one));
    while (n < 0 && errno == EINTR);
    if (n == (ssize_t)sizeof(one)) return 1;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 1;
    return -1;
}

/* Drain a pending wake. 0 means "nothing left" (a stale wake we consumed). */
static int
efd_drain(int fd)
{
    uint64_t v;
    ssize_t  n;

    do
        n = read(fd, &v, sizeof(v));
    while (n < 0 && errno == EINTR);
    if (n == (ssize_t)sizeof(v)) return 1;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return -1;
}

static int
efd_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ---- the SCM_RIGHTS rendezvous ------------------------------------------ */

static void
efd_sock_addr(const char *ring_name, struct sockaddr_un *sa, socklen_t *len)
{
    int n;

    memset(sa, 0, sizeof(*sa));
    sa->sun_family = AF_UNIX;
    /* abstract namespace: sun_path[0] stays NUL and the name follows it. A
     * filesystem socket would need cleanup on every crash path; an abstract
     * one dies with its last fd. */
    n    = snprintf(sa->sun_path + 1, sizeof(sa->sun_path) - 1, "%s%s", EFD_SOCK_PREFIX, ring_name);
    *len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (n > 0 ? n : 0));
}

static int
efd_send_pair(int conn, int a, int b)
{
    struct msghdr   msg;
    struct iovec    iov;
    char            buf[CMSG_SPACE(sizeof(int) * 2)];
    struct cmsghdr *cmsg;
    char            byte = 'F';
    int             fds[2] = {a, b};

    memset(&msg, 0, sizeof(msg));
    memset(buf, 0, sizeof(buf));
    iov.iov_base       = &byte;
    iov.iov_len        = 1;
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = buf;
    msg.msg_controllen = sizeof(buf);

    cmsg             = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(fds));
    memcpy(CMSG_DATA(cmsg), fds, sizeof(fds));

    return sendmsg(conn, &msg, MSG_NOSIGNAL) == 1 ? 1 : -1;
}

static int
efd_recv_pair(int conn, int *a, int *b)
{
    struct msghdr   msg;
    struct iovec    iov;
    char            buf[CMSG_SPACE(sizeof(int) * 2)];
    struct cmsghdr *cmsg;
    char            byte;
    int             rc;

    memset(&msg, 0, sizeof(msg));
    memset(buf, 0, sizeof(buf));
    iov.iov_base       = &byte;
    iov.iov_len        = 1;
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = buf;
    msg.msg_controllen = sizeof(buf);

    do
        rc = recvmsg(conn, &msg, 0);
    while (rc < 0 && errno == EINTR);
    if (rc != 1) return -1;

    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg))
    {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
        if (cmsg->cmsg_len < CMSG_LEN(sizeof(int) * 2)) return -1;
        memcpy(a, CMSG_DATA(cmsg), sizeof(int));
        memcpy(b, (char *)CMSG_DATA(cmsg) + sizeof(int), sizeof(int));
        return 1;
    }
    return -1;
}

/* ---- client ------------------------------------------------------------- */

/* Create the pair and the rendezvous listener. Runs in Ring_init, before the
 * worker exists, so the worker can connect the moment it maps the segment. */
static int
eventfd_client_init(FastpostgresRing *r)
{
    struct sockaddr_un sa;
    socklen_t          salen;
    EfdLocal          *st;
    int                lfd = -1;

    st = efd_local_take(r, true);
    if (st == NULL)
    {
        errno = EMFILE;
        return -1;
    }
    /* A second init on the same mapping (Python allows an explicit __init__)
     * starts from a clean pair rather than leaking the first one. */
    if (st->req >= 0) close(st->req);
    if (st->resp >= 0) close(st->resp);
    if (st->listen >= 0) close(st->listen);

    st->req  = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    st->resp = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (st->req < 0 || st->resp < 0) goto fail;

    lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0) goto fail;

    efd_sock_addr(r->ring_name, &sa, &salen);
    if (bind(lfd, (struct sockaddr *)&sa, salen) != 0) goto fail;
    if (listen(lfd, 4) != 0) goto fail;
    if (efd_set_nonblock(lfd) != 0) goto fail;
    st->listen = lfd;
    return 1;

fail:
    if (lfd >= 0) close(lfd);
    if (st->req >= 0) close(st->req);
    if (st->resp >= 0) close(st->resp);
    efd_local_enter();
    st->ring = NULL;
    efd_local_leave();
    return -1;
}

/* Close this process's copies, and only this mapping's. A forked child that
 * drops an inherited Ring closes the child's copies (the parent's stay open),
 * and an old Ring object deallocated after a newer one took the same name does
 * not touch the newer object's numbers. */
static void
eventfd_client_fini(FastpostgresRing *r)
{
    EfdLocal *st = efd_local_take(r, false);

    if (st == NULL) return;
    if (st->req >= 0) close(st->req);
    if (st->resp >= 0) close(st->resp);
    if (st->listen >= 0) close(st->listen);
    efd_local_enter();
    st->ring = NULL;
    efd_local_leave();
}

static int
eventfd_post_request(FastpostgresRing *r, uint64_t seq, int timeout_ms)
{
    EfdLocal *st = efd_local_take(r, false);

    (void)seq; /* one request in flight: the increment lands on the caller's seq */
    (void)timeout_ms;
    if (st == NULL)
    {
        errno = EBADF;
        return -1;
    }

    (void)__atomic_add_fetch(&r->req_seq, 1, __ATOMIC_RELEASE);
    fastpostgres_ring_tally(&r->tally.client, 1, 1, 0, 0);
    return efd_signal(st->req);
}

static int
eventfd_wait_response(FastpostgresRing *r, uint64_t seq, int timeout_ms)
{
    struct timespec start;
    struct pollfd   p;
    EfdLocal       *st = efd_local_take(r, false);

    (void)seq;
    if (st == NULL)
    {
        errno = EBADF;
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &start);

    /* One wait per handoff, counted where it happens: this is the sleep a wait
     * policy would have to remove, exactly as the semaphore engine counts it. */
    fastpostgres_ring_tally(&r->tally.client, 0, 0, 1, 0);

    p.fd     = st->resp;
    p.events = POLLIN;
    for (;;)
    {
        int left = efd_remaining_ms(&start, timeout_ms);
        int rc   = poll(&p, 1, left);

        if (rc < 0)
        {
            if (errno == EINTR) continue;
            return -1;
        }
        if (rc == 0) return 0;
        rc = efd_drain(st->resp);
        if (rc > 0)
        {
            /* Everything the worker wrote before its bell is now visible. */
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            return 1;
        }
        if (rc < 0) return -1;
        /* stale wake: the counter was consumed by someone else */
        if (efd_remaining_ms(&start, timeout_ms) == 0) return 0;
    }
}

/* Wait for the worker, serving its SCM_RIGHTS handshake while we wait. The
 * worker cannot set worker_ready until it has the pair, so accepting here is
 * part of readiness, not an afterthought. */
static int
eventfd_wait_ready(FastpostgresRing *r, int timeout_ms)
{
    struct timespec start;
    struct timespec pause = {0, 1000000L}; /* 1 ms */
    EfdLocal       *st    = efd_local_take(r, false);

    clock_gettime(CLOCK_MONOTONIC, &start);
    if (st == NULL)
    {
        errno = EBADF;
        return -1;
    }
    for (;;)
    {
        int conn;

        if (__atomic_load_n(&r->worker_ready, __ATOMIC_ACQUIRE) != 0) return 1;

        conn = accept(st->listen, NULL, NULL);
        if (conn >= 0)
        {
            (void)efd_send_pair(conn, st->req, st->resp);
            close(conn);
        }
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return -1;

        if (timeout_ms <= 0 || efd_since_ms(&start) >= timeout_ms) return 0;
        nanosleep(&pause, NULL);
    }
}

/* ---- worker ------------------------------------------------------------- */

static int
eventfd_worker_init(FastpostgresRing *r)
{
    (void)r;
    return 1;
}

static void
eventfd_worker_fini(FastpostgresRing *r)
{
    fastpostgres_eventfd_detach(r);
}

static int
eventfd_wait_request(FastpostgresRing *r, uint64_t *seq, int timeout_ms)
{
    struct timespec start;

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;)
    {
        struct pollfd p;
        int           left = efd_remaining_ms(&start, timeout_ms);
        int           rc;

        p.fd     = r->worker_req_efd;
        p.events = POLLIN;
        rc       = poll(&p, 1, left);
        if (rc < 0)
        {
            if (errno == EINTR) continue;
            return -1;
        }
        if (rc == 0) return 0;

        rc = efd_drain(r->worker_req_efd);
        if (rc < 0) return -1;
        /* A stale wake: the doorbell rang but every unanswered request was
         * already answered. Keep waiting until the deadline. */
        {
            uint64_t asked    = __atomic_load_n(&r->req_seq, __ATOMIC_ACQUIRE);
            uint64_t answered = __atomic_load_n(&r->resp_seq, __ATOMIC_ACQUIRE);

            if (asked != answered)
            {
                *seq = asked;
                return 1;
            }
        }
        if (efd_remaining_ms(&start, timeout_ms) == 0) return 0;
    }
}

static int
eventfd_post_response(FastpostgresRing *r, uint64_t seq)
{
    __atomic_store_n(&r->resp_seq, seq, __ATOMIC_RELEASE);
    fastpostgres_ring_tally(&r->tally.worker, 1, 1, 0, 0);
    return efd_signal(r->worker_resp_efd);
}

/* ---- the SCM_RIGHTS worker side ----------------------------------------- */

int
fastpostgres_eventfd_attach(FastpostgresRing *r)
{
    struct sockaddr_un sa;
    socklen_t          salen;
    struct timeval     tv = {2, 0}; /* a client that never accepts must not stall the worker */
    int                conn;
    int                a = -1;
    int                b = -1;

    if (r->ring_name[0] == 0) return -1; /* the client did not publish its name */

    efd_sock_addr(r->ring_name, &sa, &salen);
    conn = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (conn < 0) return -1;
    if (connect(conn, (struct sockaddr *)&sa, salen) != 0)
    {
        close(conn);
        return -1;
    }
    setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (efd_recv_pair(conn, &a, &b) != 1)
    {
        close(conn);
        return -1;
    }
    close(conn);

    if (efd_set_nonblock(a) != 0 || efd_set_nonblock(b) != 0)
    {
        close(a);
        close(b);
        return -1;
    }
    r->worker_req_efd  = a;
    r->worker_resp_efd = b;
    r->srv_pid         = (uint32_t)getpid();
    return 1;
}

void
fastpostgres_eventfd_detach(FastpostgresRing *r)
{
    if (r->worker_req_efd >= 0) close(r->worker_req_efd);
    if (r->worker_resp_efd >= 0) close(r->worker_resp_efd);
    r->worker_req_efd = r->worker_resp_efd = -1;
}

const FastpostgresTransport fastpostgres_transport_eventfd = {
    .name          = "eventfd",
    .id            = FASTPOSTGRES_TRANSPORT_EVENTFD,
    .mechanism     = "two eventfds per ring: one poll() serves many rings",
    .client_init   = eventfd_client_init,
    .client_fini   = eventfd_client_fini,
    .post_request  = eventfd_post_request,
    .wait_response = eventfd_wait_response,
    .wait_ready    = eventfd_wait_ready,
    .worker_init   = eventfd_worker_init,
    .worker_fini   = eventfd_worker_fini,
    .wait_request  = eventfd_wait_request,
    .post_response = eventfd_post_response,
};
