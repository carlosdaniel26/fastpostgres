/* ring.cpp — Ring(seg, segment_bytes=0, transport="shm"): the
 * request/response channel the backend worker services, and the data segment's
 * capacity. The client creates /dev/shm/<seg>, initialises the doorbells, and
 * bootstraps the worker with one SQL call over libpq (start); open/next/close
 * then travel through shared memory only, and the worker maps each data
 * segment at the capacity the ring carries.
 *
 * The handoffs ride the engine the segment is stamped with
 * (extension/transport/transport.h): "shm" is the original pair of semaphores,
 * "eventfd" is a pollable pair that lets a pool worker serve several rings.
 * Everything on this side that is not a doorbell — the ops, arg, batch,
 * status/nrows/error, the data segments — is transport-neutral, and every
 * handoff goes through that engine's ops table. */
#include "ring.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "connection.hpp"
#include "fastpostgres/fastpostgres_shm.h"
#include "segment.hpp"
#include "types.hpp"

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

/* The handoff budgets, in milliseconds: the response wait, the readiness wait
 * that lets a background worker register and start, and the near-immediate post
 * that only guards a broken engine. */
#define RING_WAIT_MS  10000
#define RING_READY_MS 10000
#define RING_POST_MS  1000
#define RING_STOP_MS  1000

/* An OSError carrying the engine's errno text: a failed sem_post or wait. */
static void
ring_errno(const char *what)
{
    int e = errno;

    PyErr_Format(PyExc_OSError, "%s: %s", what, strerror(e ? e : EIO));
}

/* Claim the ring's single request slot for this thread and process. Two drivers
 * interleave the slot and steal each other's wake; and a forked child shares the
 * mapped segment with its parent, so it must not drive the parent's ring at all.
 * Both are silent corruption, so they are refused here rather than left to the
 * caller's discipline. */
static int
ring_claim(RingObject *self, const char *what)
{
    if (self->ring == nullptr || self->done)
    {
        PyErr_Format(PyExc_RuntimeError, "ring: %s on a ring that was stopped", what);
        return -1;
    }
    if (self->pid != (uint32_t)getpid())
    {
        PyErr_Format(PyExc_RuntimeError, "ring: %s after fork (the ring belongs to process %u)", what,
                     (unsigned)self->pid);
        return -1;
    }
    if (self->busy)
    {
        PyErr_Format(PyExc_RuntimeError, "ring: %s while a request is already in flight", what);
        return -1;
    }
    self->busy    = 1;
    self->busy_by = PyThread_get_thread_ident();
    return 0;
}

static void
ring_unclaim(RingObject *self)
{
    self->busy    = 0;
    self->busy_by = 0;
}

/* The sticky half: an open stream belongs to the thread that opened it until
 * that stream ends. The worker keeps one stream per ring, so a second thread's
 * OPEN would silently abort the first thread's cursor and its next pull would
 * read another query's rows. */
static int
ring_own(RingObject *self, const char *what)
{
    if (self->owned && self->owner != PyThread_get_thread_ident())
    {
        PyErr_Format(PyExc_RuntimeError, "ring: %s while another thread holds the open stream", what);
        return -1;
    }
    return 0;
}

static void
ring_take(RingObject *self)
{
    self->owned = 1;
    self->owner = PyThread_get_thread_ident();
}

static void
ring_disown(RingObject *self)
{
    self->owned = 0;
    self->owner = 0;
}

/* The memfd-handoff channel: a listener the client opens before it starts the
 * worker, whose abstract address travels in the ring. The worker connects once;
 * segment descriptors ride that connection (SCM_RIGHTS). */
static int
ring_listen(RingObject *self)
{
    static unsigned serial;
    struct sockaddr_un addr;
    socklen_t          len;
    int                n, s;

    if (self->fd_listen >= 0) return 0;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    n = snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1, "fpq-%u-%u", (unsigned)getpid(), serial++);
    if (n < 0 || (size_t)n >= sizeof(addr.sun_path) - 1) return -1;
    len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)n);

    s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    if (bind(s, (struct sockaddr *)&addr, len) != 0 || listen(s, 4) != 0)
    {
        close(s);
        return -1;
    }

    self->fd_listen        = s;
    self->ring->fd_handoff = 1;
    self->ring->fd_pathlen = (uint32_t)(1 + n);
    memcpy(self->ring->fd_path, addr.sun_path, (size_t)(1 + n));
    return 0;
}

/* Close both ends of the handoff and drop the accepted socket from the
 * process-wide registry. Idempotent. */
static void
ring_handoff_close(RingObject *self)
{
    if (self->fd_sock >= 0)
    {
        fp_handoff_unregister(self->fd_sock);
        close(self->fd_sock);
        self->fd_sock = -1;
    }
    if (self->fd_listen >= 0)
    {
        close(self->fd_listen);
        self->fd_listen = -1;
    }
    if (self->ring != nullptr)
    {
        self->ring->fd_handoff = 0;
        self->ring->fd_pathlen = 0;
    }
}

/* Run the engine's teardown once. The shm engine has nothing to release, but
 * the call is kept for an engine that does. Idempotent, so the stop path and
 * the destructor can both run it. */
static void
ring_fini(RingObject *self)
{
    if (self->ring != nullptr && self->ops != nullptr && !self->done)
    {
        self->ops->client_fini(self->ring);
        self->done = 1;
    }
    ring_handoff_close(self);
}

static void
Ring_dealloc(RingObject *self)
{
    ring_fini(self); /* the engine's teardown, on every path, including exceptions */
    if (self->map) munmap(self->map, self->size);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
Ring_init(RingObject *self, PyObject *args, PyObject *kwds)
{
    static const char *kwlist[] = {"segment", "segment_bytes", "transport", nullptr};
    const char        *seg;
    const char        *transport = "shm";
    unsigned long long data_bytes = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|Ks", (char **)kwlist, &seg, &data_bytes, &transport))
        return -1;

    /* The engine the segment is driven with. "shm" is the original semaphore
     * doorbell; "eventfd" is the pollable one a pool worker can serve several
     * rings with. An unknown name is an error, never a silent fallback. */
    const FastpostgresTransport *ops = fastpostgres_transport_by_name(transport);

    if (ops == nullptr)
    {
        PyErr_Format(PyExc_ValueError, "Ring: unknown transport %s", transport);
        return -1;
    }

    /* Sealed-memfd data segments are opt-in (FASTPOSTGRES_MEMFD=1). The default
     * stays the named /dev/shm segment, which is what a client running as a
     * different user than the server needs. */
    const char *memfd_env = getenv("FASTPOSTGRES_MEMFD");

    self->memfd     = memfd_env != nullptr && memfd_env[0] != 0 && memfd_env[0] != '0';
    self->fd_listen = -1;
    self->fd_sock   = -1;

    if (data_bytes != 0)
    {
        if (data_bytes < FASTPOSTGRES_SEGMENT_MIN)
            data_bytes = FASTPOSTGRES_SEGMENT_MIN;
        else if (data_bytes > FASTPOSTGRES_SEGMENT_BYTES)
            data_bytes = FASTPOSTGRES_SEGMENT_BYTES;
    }

    for (const char *p = seg; *p; p++)
    {
        char ch = *p;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
              ch == '-'))
        {
            PyErr_SetString(PyExc_ValueError, "Ring: invalid segment name");
            return -1;
        }
    }
    if (std::strlen(seg) >= sizeof(self->data))
    {
        PyErr_SetString(PyExc_ValueError, "Ring: segment name too long");
        return -1;
    }

    std::string path = "/" + std::string(seg);
    int         fd   = shm_open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0)
    {
        PyErr_SetString(PyExc_OSError, "Ring: cannot create /dev/shm segment");
        return -1;
    }
    if (ftruncate(fd, FASTPOSTGRES_RING_BYTES) != 0)
    {
        close(fd);
        PyErr_SetString(PyExc_OSError, "Ring: ftruncate failed");
        return -1;
    }
    void *map = mmap(nullptr, FASTPOSTGRES_RING_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
    {
        PyErr_SetString(PyExc_OSError, "Ring: mmap failed");
        return -1;
    }

    FastpostgresRing *r = (FastpostgresRing *)map;
    if (memcmp(r->magic, FASTPOSTGRES_RING_MAGIC, 8) != 0)
    {
        /* fresh segment: initialise the semaphores before the worker starts */
        memset(r, 0, FASTPOSTGRES_RING_BYTES);
        if (sem_init(&r->req_ready, 1, 0) != 0 || sem_init(&r->resp_ready, 1, 0) != 0)
        {
            munmap(map, FASTPOSTGRES_RING_BYTES);
            PyErr_SetString(PyExc_OSError, "Ring: sem_init failed");
            return -1;
        }
        r->version = FASTPOSTGRES_RING_VERSION;
        memcpy(r->magic, FASTPOSTGRES_RING_MAGIC, 8);
    }
    else if (r->version != FASTPOSTGRES_RING_VERSION)
    {
        uint32_t have = r->version;

        munmap(map, FASTPOSTGRES_RING_BYTES);
        PyErr_Format(PyExc_OSError, "Ring: segment \"%s\" speaks layout %u, not %u", seg, have,
                     FASTPOSTGRES_RING_VERSION);
        return -1;
    }

    /* The engine's id is the segment's provenance. A v4 ring made before the
     * field was filled in reads zero and is stamped here; one stamped for a
     * different engine is refused, never driven with the wrong doorbell. */
    if (r->transport == 0)
        r->transport = ops->id;
    else if (r->transport != ops->id)
    {
        uint32_t have = r->transport;

        munmap(map, FASTPOSTGRES_RING_BYTES);
        PyErr_Format(PyExc_OSError, "Ring: segment \"%s\" was made for transport %u, not %s", seg, have,
                     ops->name);
        return -1;
    }

    self->ring = r;
    self->map  = map;
    self->size = FASTPOSTGRES_RING_BYTES;
    self->ops  = ops;
    self->seq  = 0;
    self->done = 1; /* nothing to release until client_init succeeds */
    self->busy    = 0;
    self->busy_by = 0;
    self->owned   = 0;
    self->owner   = 0;
    self->pid     = (uint32_t)getpid(); /* the ring is this process's to drive */
    memcpy(self->data, seg, std::strlen(seg) + 1);
    /* A reused segment starts from a clean handoff: the counters, the op and
     * readiness are this client's to establish, and a stale ready from a
     * previous worker would let wait_ready return before this one exists. */
    r->req_seq      = 0;
    r->resp_seq     = 0;
    r->op           = FASTPOSTGRES_OP_NONE;
    r->status       = 0;
    r->worker_ready = 0;
    r->srv_pid      = 0;
    std::strncpy(r->ring_name, seg, sizeof(r->ring_name) - 1);
    r->ring_name[sizeof(r->ring_name) - 1] = 0;
    /* the capacity the worker is to map each data segment at (0 = its default) */
    r->data_bytes = data_bytes;
    r->client_pid = (uint32_t)getpid(); /* the worker exits once this process is gone */
    /* stale handoff fields from a previous owner of this name are not ours */
    r->fd_handoff = 0;
    r->fd_pathlen = 0;

    /* Last: the engine opens what it needs. A failure is raised with the errno
     * text and the segment is left alone. */
    if (ops->client_init(r) < 0)
    {
        int e = errno;

        munmap(map, self->size);
        self->ring = nullptr;
        self->map  = nullptr;
        self->size = 0;
        PyErr_Format(PyExc_OSError, "Ring: %s transport init failed: %s", ops->name, strerror(e ? e : EIO));
        return -1;
    }
    self->done = 0;
    return 0;
}

/* Hand one request to the worker through the ring's engine. The response is
 * collected by ring_collect(); one request is in flight at a time, so the
 * ring's single op/arg/nrows slot is enough, and the caller may do other work
 * before waiting. The metadata writes stay here, in the transport-neutral part
 * of the ring; only the handoff itself is the engine's. */
static int
ring_send(RingObject *self, uint32_t op, const char *a1, const char *a2, uint32_t batch)
{
    FastpostgresRing *r = self->ring;

    if (ring_claim(self, "sending a request") < 0) return -1;
    if (fastpostgres_ring_arg_set(r->arg, a1, a2) != 0)
    {
        ring_unclaim(self);
        PyErr_SetString(PyExc_ValueError, "ring: argument too long");
        return -1;
    }

    r->op       = op;
    r->batch    = batch;
    r->status   = 0;
    r->nrows    = 0;
    r->error[0] = 0;

    /* The sequence this request carries: one more than the ring's counter,
     * which post_request settles; the store below is the backstop. */
    uint64_t seq = __atomic_load_n(&r->req_seq, __ATOMIC_RELAXED) + 1;
    int      rc;

    errno = 0;
    rc    = self->ops->post_request(r, seq, RING_POST_MS);
    if (rc <= 0)
    {
        ring_unclaim(self);
        if (rc == 0)
            PyErr_SetString(PyExc_TimeoutError, "ring: the transport timed out posting the request");
        else
        {
            char what[128];

            snprintf(what, sizeof(what), "ring: %s post_request failed", self->ops->name);
            ring_errno(what);
        }
        return -1;
    }
    if (__atomic_load_n(&r->req_seq, __ATOMIC_RELAXED) < seq)
        __atomic_store_n(&r->req_seq, seq, __ATOMIC_RELEASE);
    self->seq = seq;
    return 0;
}

/* Block for the response to the request last sent; nrows, or -1 with a Python
 * exception set. The wait can block for seconds, so the GIL is released
 * around it. */
static long long
ring_collect(RingObject *self)
{
    FastpostgresRing *r = self->ring;
    int               rc;

    if (!self->busy || self->busy_by != PyThread_get_thread_ident())
    {
        PyErr_SetString(PyExc_RuntimeError, "ring: no request of this thread is in flight");
        return -1;
    }

    errno = 0;
    Py_BEGIN_ALLOW_THREADS;
    rc = self->ops->wait_response(r, self->seq, RING_WAIT_MS);
    Py_END_ALLOW_THREADS;
    ring_unclaim(self); /* the slot is free again, whatever the wait decided */
    if (rc <= 0)
    {
        if (rc == 0)
            PyErr_SetString(PyExc_TimeoutError, "ring: timeout waiting for the worker");
        else
        {
            char what[128];

            snprintf(what, sizeof(what), "ring: %s wait_response failed", self->ops->name);
            ring_errno(what);
        }
        return -1;
    }
    /* Everything the worker wrote before its release post is now visible. */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (r->status != 0)
    {
        PyErr_SetString(PyExc_RuntimeError, r->error[0] ? r->error : "ring: worker error");
        return -1;
    }
    return (long long)r->nrows;
}

/* Send one request and block for its response; returns nrows, or -1 with a
 * Python exception set. */
static long long
ring_request(RingObject *self, uint32_t op, const char *a1, const char *a2, uint32_t batch)
{
    if (ring_send(self, op, a1, a2, batch) < 0) return -1;
    return ring_collect(self);
}

static PyObject *
Ring_start(RingObject *self, PyObject *conn)
{
    if (!PyObject_TypeCheck(conn, &ConnectionType))
    {
        PyErr_SetString(PyExc_TypeError, "ring.start(conn): expected a Connection");
        return nullptr;
    }
    PGconn *pg = ((ConnectionObject *)conn)->pg;
    if (!pg)
    {
        PyErr_SetString(PyExc_ValueError, "connection is closed");
        return nullptr;
    }
    if (ring_claim(self, "starting a worker") < 0) return nullptr;

    /* Before the worker starts: publish where it should connect to hand over
     * its data segment. If the listener cannot be made, the ring falls back to
     * the named segment without failing the session. */
    if (self->memfd && self->fd_sock < 0 && ring_listen(self) != 0)
    {
        self->memfd           = 0;
        self->ring->fd_handoff = 0;
        self->ring->fd_pathlen = 0;
    }

    /* The bootstrap asks the server to register the worker over libpq, as it
     * always has. The shm engine keeps the one-argument call, which is also
     * what a server that predates the two-argument overload understands; any
     * other engine names itself, so the server never has to guess. */
    std::string sql = "SELECT fastpostgres_ring_start('" + std::string(self->data) + "'";
    if (self->ops->id != FASTPOSTGRES_TRANSPORT_SHM)
        sql += ", '" + std::string(self->ops->name) + "'";
    sql += ")";

    PGresult *res;

    Py_BEGIN_ALLOW_THREADS;
    res = PQexec(pg, sql.c_str());
    Py_END_ALLOW_THREADS;
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        PyObject *err = fp_raise_libpq(res);
        PQclear(res);
        ring_unclaim(self);
        return err;
    }
    PQclear(res);

    /* Readiness is the engine's; the shm wait_ready polls worker_ready. */
    int rc;

    errno = 0;
    Py_BEGIN_ALLOW_THREADS;
    rc = self->ops->wait_ready(self->ring, RING_READY_MS);
    Py_END_ALLOW_THREADS;
    if (rc <= 0)
    {
        ring_unclaim(self);
        if (rc == 0)
            PyErr_SetString(PyExc_TimeoutError, "ring: worker did not become ready");
        else
        {
            char what[128];

            snprintf(what, sizeof(what), "ring: %s wait_ready failed", self->ops->name);
            ring_errno(what);
        }
        return nullptr;
    }

    /* The worker connects while it starts, so the accept has the connection
     * queued by now. The wait bounds an old server that ignored the request:
     * its segment stays /dev/shm and read() falls back to it. */
    if (self->fd_listen >= 0)
    {
        struct pollfd p;
        int           ready;

        p.fd      = self->fd_listen;
        p.events  = POLLIN;
        p.revents = 0;
        do
            ready = poll(&p, 1, 2000);
        while (ready < 0 && errno == EINTR);
        if (ready > 0)
        {
            int s = accept(self->fd_listen, nullptr, nullptr);

            if (s >= 0)
            {
                fcntl(s, F_SETFD, FD_CLOEXEC);
                self->fd_sock = s;
                fp_handoff_register(s);
            }
        }
        close(self->fd_listen);
        self->fd_listen = -1;
    }

    ring_unclaim(self);
    Py_RETURN_NONE;
}

static PyObject *
Ring_open(RingObject *self, PyObject *args)
{
    const char *seg, *query;
    if (!PyArg_ParseTuple(args, "ss", &seg, &query)) return nullptr;
    if (strlen(seg) >= sizeof(self->data))
    {
        PyErr_SetString(PyExc_ValueError, "ring: stream name too long");
        return nullptr;
    }
    if (ring_own(self, "opening a stream") < 0) return nullptr;
    memcpy(self->data, seg, strlen(seg) + 1);
    if (ring_request(self, FASTPOSTGRES_OP_OPEN, self->data, query, 0) < 0)
    {
        ring_disown(self); /* the stream is over here, whatever the worker saw */
        return nullptr;
    }
    ring_take(self);
    Py_RETURN_NONE;
}

static PyObject *
Ring_next(RingObject *self, PyObject *args)
{
    long long batch = 100000;
    if (!PyArg_ParseTuple(args, "|L", &batch)) return nullptr;
    if (batch <= 0) batch = 100000;
    if (ring_own(self, "pulling the next batch") < 0) return nullptr;
    long long n = ring_request(self, FASTPOSTGRES_OP_NEXT, self->data, self->data, (uint32_t)batch);
    if (n < 0) return nullptr;
    return PyLong_FromLongLong(n);
}

/* The same pull, answered with the row frame: the result tuples themselves, for
 * callers that build Python objects from them. */
static PyObject *
Ring_next_rows(RingObject *self, PyObject *args)
{
    long long batch = 100000;
    if (!PyArg_ParseTuple(args, "|L", &batch)) return nullptr;
    if (batch <= 0) batch = 100000;
    if (ring_own(self, "pulling the next batch") < 0) return nullptr;
    long long n = ring_request(self, FASTPOSTGRES_OP_NEXT_ROWS, self->data, self->data, (uint32_t)batch);
    if (n < 0) return nullptr;
    return PyLong_FromLongLong(n);
}

/* The same pull, answered with the scalar frame: the worker accumulates the
 * plan over the batch and publishes one value per op, not the rows. `seg` is
 * both the stream and the data segment, as with next(). */
static PyObject *
Ring_reduce(RingObject *self, PyObject *args)
{
    const char *seg, *plan;
    long long   batch = 100000;

    if (!PyArg_ParseTuple(args, "ss|L", &seg, &plan, &batch)) return nullptr;
    if (batch <= 0) batch = 100000;
    if (ring_own(self, "reducing the next batch") < 0) return nullptr;
    long long n = ring_request(self, FASTPOSTGRES_OP_REDUCE, seg, plan, (uint32_t)batch);
    if (n < 0) return nullptr;
    return PyLong_FromLongLong(n);
}

/* Send a NEXT for `seg` without waiting: the worker fills that segment while the
 * caller decodes the previous batch, and wait() collects the answer. One request
 * is in flight at a time, so the caller must wait before posting the next. */
static PyObject *
Ring_post_rows(RingObject *self, PyObject *args)
{
    const char *seg;
    long long   batch = 100000;

    if (!PyArg_ParseTuple(args, "s|L", &seg, &batch)) return nullptr;
    if (batch <= 0) batch = 100000;
    if (self->data[0] == 0)
    {
        PyErr_SetString(PyExc_ValueError, "ring: open a stream first");
        return nullptr;
    }
    if (ring_own(self, "posting the next batch") < 0) return nullptr;
    if (ring_send(self, FASTPOSTGRES_OP_NEXT_ROWS, self->data, seg, (uint32_t)batch) < 0) return nullptr;
    Py_RETURN_NONE;
}

/* Send a NEXT for `seg` without waiting, answered with the columnar frame.
 * The same pipelining post_rows() gives the row frame: the worker fills `seg`
 * while the caller copies the previous batch out of the other segment. */
static PyObject *
Ring_post(RingObject *self, PyObject *args)
{
    const char *seg;
    long long   batch = 100000;

    if (!PyArg_ParseTuple(args, "s|L", &seg, &batch)) return nullptr;
    if (batch <= 0) batch = 100000;
    if (self->data[0] == 0)
    {
        PyErr_SetString(PyExc_ValueError, "ring: open a stream first");
        return nullptr;
    }
    if (ring_own(self, "posting the next batch") < 0) return nullptr;
    if (ring_send(self, FASTPOSTGRES_OP_NEXT, self->data, seg, (uint32_t)batch) < 0) return nullptr;
    Py_RETURN_NONE;
}

/* Collect the response to the request last sent. */
static PyObject *
Ring_wait(RingObject *self, PyObject *)
{
    if (ring_own(self, "collecting a batch") < 0) return nullptr;
    long long n = ring_collect(self);
    if (n < 0) return nullptr;
    return PyLong_FromLongLong(n);
}

static PyObject *
Ring_close(RingObject *self, PyObject *)
{
    /* CLOSE ends the stream however it is asked — a stream abandoned by a dead
     * task must not wedge the ring for its next lease — so ownership is dropped
     * either way; the request itself still needs the slot. */
    int rc = ring_request(self, FASTPOSTGRES_OP_CLOSE, self->data, nullptr, 0);

    ring_disown(self);
    if (rc < 0) return nullptr;
    Py_RETURN_NONE;
}

static PyObject *
Ring_stop(RingObject *self, PyObject *)
{
    if (self->ring == nullptr || self->ops == nullptr || self->done)
    {
        Py_RETURN_NONE;
    }
    if (ring_claim(self, "stopping") < 0)
    {
        /* Teardown is best effort and must not raise: a ring already being
         * driven by another thread, or one inherited by a forked child, is left
         * to its owner; the worker also exits when the client process goes. */
        PyErr_Clear();
        Py_RETURN_NONE;
    }

    FastpostgresRing *r = self->ring;
    r->op               = FASTPOSTGRES_OP_STOP;

    /* Best effort, as before: the doorbell ignores a failed post, and the worker
     * also sees op == STOP on its next timeout. A teardown path must not turn
     * that into an exception. */
    uint64_t seq = __atomic_load_n(&r->req_seq, __ATOMIC_RELAXED) + 1;

    if (self->ops->post_request(r, seq, RING_STOP_MS) > 0)
    {
        if (__atomic_load_n(&r->req_seq, __ATOMIC_RELAXED) < seq)
            __atomic_store_n(&r->req_seq, seq, __ATOMIC_RELEASE);
    }
    ring_unclaim(self);
    ring_disown(self);

    /* The engine's teardown; the worker's own fini runs on its side. */
    ring_fini(self);
    Py_RETURN_NONE;
}

static PyMethodDef Ring_methods[] = {
    {"start", (PyCFunction)Ring_start, METH_O, "start(conn): bootstrap the worker"},
    {"open", (PyCFunction)Ring_open, METH_VARARGS, "open(seg, query): start a stream"},
    {"next", (PyCFunction)Ring_next, METH_VARARGS, "next(batch=100000) -> int rows (the columnar frame)"},
    {"next_rows", (PyCFunction)Ring_next_rows, METH_VARARGS,
     "next_rows(batch=100000) -> int rows (the row frame; falls back to columnar)"},
    {"reduce", (PyCFunction)Ring_reduce, METH_VARARGS,
     "reduce(seg, plan, batch=100000) -> int rows (the scalar frame, reduced in the worker)"},
    {"post_rows", (PyCFunction)Ring_post_rows, METH_VARARGS,
     "post_rows(seg, batch=100000): ask for a batch into seg, do not wait"},
    {"post", (PyCFunction)Ring_post, METH_VARARGS,
     "post(seg, batch=100000): ask for a columnar batch into seg, do not wait"},
    {"wait", (PyCFunction)Ring_wait, METH_NOARGS, "wait() -> int rows for the request last sent"},
    {"close", (PyCFunction)Ring_close, METH_NOARGS, "close(): end the stream"},
    {"stop", (PyCFunction)Ring_stop, METH_NOARGS, "stop(): ask the worker to exit"},
    {nullptr},
};

PyTypeObject RingType = {PyVarObject_HEAD_INIT(nullptr, 0)};

bool
fp_register_ring(PyObject *module)
{
    if (!fp_init_type(&RingType, "fastpostgres.Ring", sizeof(RingObject), (destructor)Ring_dealloc,
                      Ring_methods, nullptr))
        return false;
    RingType.tp_init = (initproc)Ring_init;
    RingType.tp_new  = PyType_GenericNew;

    Py_INCREF(&RingType);
    if (PyModule_AddObject(module, "Ring", (PyObject *)&RingType) != 0)
    {
        Py_DECREF(&RingType);
        return false;
    }
    return true;
}
