/* ring.cpp — Ring(seg, segment_bytes=0, transport="shm"): the request/response
 * channel the backend worker services, and the data segment's capacity. The
 * client creates /dev/shm/<seg>, initialises the semaphores, and bootstraps the
 * worker with one SQL call over libpq (start); open/next/close then travel
 * through shared memory only, and the worker maps each data segment at the
 * capacity the ring carries.
 *
 * Two engines can carry the handoffs (extension/transport/transport.h): the
 * ring's own semaphores ("shm", the default) and the regipc kernel module
 * ("regipc"). Everything on this side that is not a doorbell — the ops, arg,
 * batch, status/nrows/error, the data segments — is the same for both; the
 * transport is resolved once, at construction, and every handoff goes through
 * the ops table the segment was stamped with. */
#include "ring.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "connection.hpp"
#include "fastpostgres/fastpostgres_shm.h"
#include "types.hpp"

/* The handoff budgets, in milliseconds. The response wait is what the old
 * sem_timedwait used; readiness allows a background worker to be registered and
 * start; posting is immediate in both engines and only guards a broken one. */
#define RING_WAIT_MS  10000
#define RING_READY_MS 10000
#define RING_POST_MS  1000
#define RING_STOP_MS  1000

/* An OSError carrying the engine's errno text: a missing /dev/regipc, a
 * registration the module refused, a failed sem_post. */
static void
ring_errno(const char *what)
{
    int e = errno;

    PyErr_Format(PyExc_OSError, "%s: %s", what, strerror(e ? e : EIO));
}

/* Run the engine's teardown once. For regipc that is QUIT + close, and the
 * caller leaves first; for shm there is nothing to release. Idempotent, so the
 * stop path and the destructor can both run it. */
static void
ring_fini(RingObject *self)
{
    if (self->ring != nullptr && self->ops != nullptr && !self->done)
    {
        self->ops->client_fini(self->ring, &self->ctx);
        self->done = 1;
    }
}

static void
Ring_dealloc(RingObject *self)
{
    ring_fini(self); /* regipc: QUIT + close, on every path, including exceptions */
    if (self->map) munmap(self->map, self->size);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
Ring_init(RingObject *self, PyObject *args, PyObject *kwds)
{
    static const char *kwlist[] = {"segment", "segment_bytes", "transport", nullptr};
    const char        *seg;
    const char        *transport  = "shm";
    unsigned long long data_bytes = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|Ks", (char **)kwlist, &seg, &data_bytes, &transport))
        return -1;

    /* Resolve before touching the segment: an unknown name is a caller error,
     * and the two engines mean different bytes behind the same doorbell, so
     * there is no safe default to fall back to. */
    const FastpostgresTransport *ops = fastpostgres_transport_by_name(transport);
    if (ops == nullptr)
    {
        PyErr_Format(PyExc_ValueError, "Ring: unknown transport \"%s\" (expected \"shm\" or \"regipc\")",
                     transport);
        return -1;
    }

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
     * field was filled in reads zero and is stamped here; one stamped for the
     * other engine is refused, never driven with the wrong doorbell. */
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
    memset(&self->ctx, 0, sizeof(self->ctx));
    memcpy(self->data, seg, std::strlen(seg) + 1);
    /* the capacity the worker is to map each data segment at (0 = its default) */
    r->data_bytes = data_bytes;
    r->client_pid = (uint32_t)getpid(); /* the worker exits once this process is gone */

    /* Last: the engine opens what it needs (regipc opens /dev/regipc and
     * registers with the module). A failure is raised with the errno text and
     * the segment is left alone, never silently served by shm: the caller
     * asked for this transport, and a substitution would be a different ring. */
    if (ops->client_init(r, &self->ctx) < 0)
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

    if (fastpostgres_ring_arg_set(r->arg, a1, a2) != 0)
    {
        PyErr_SetString(PyExc_ValueError, "ring: argument too long");
        return -1;
    }

    r->op       = op;
    r->batch    = batch;
    r->status   = 0;
    r->nrows    = 0;
    r->error[0] = 0;

    /* The sequence this request carries: one more than the ring's counter. The
     * shm engine bumps that counter itself inside post_request; the regipc
     * engine carries it in the call's carriers and leaves the ring's copy for
     * the store below. */
    uint64_t seq = __atomic_load_n(&r->req_seq, __ATOMIC_RELAXED) + 1;
    int      rc;

    errno = 0;
    rc    = self->ops->post_request(r, &self->ctx, seq, RING_POST_MS);
    if (rc <= 0)
    {
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
 * exception set. The wait can outlast a spin by seconds, so the GIL is released
 * around it, as it was around the semaphore wait this replaces. */
static long long
ring_collect(RingObject *self)
{
    FastpostgresRing *r = self->ring;
    int               rc;

    errno = 0;
    Py_BEGIN_ALLOW_THREADS;
    rc = self->ops->wait_response(r, &self->ctx, self->seq, RING_WAIT_MS);
    Py_END_ALLOW_THREADS;
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

    /* The bootstrap asks the server to register the worker over libpq, as it
     * always has. The two-argument overload names the transport; shm keeps the
     * one-argument call, which is also what a server that predates the overload
     * understands. */
    std::string sql = "SELECT fastpostgres_ring_start('" + std::string(self->data) + "'";
    if (self->ops->id != FASTPOSTGRES_TRANSPORT_SHM) sql += ", '" + std::string(self->ops->name) + "'";
    sql += ")";

    PGresult *res;

    Py_BEGIN_ALLOW_THREADS;
    res = PQexec(pg, sql.c_str());
    Py_END_ALLOW_THREADS;
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        PyObject *err = fp_raise_libpq(res);
        PQclear(res);
        return err;
    }
    PQclear(res);

    /* Readiness is the engine's: for regipc it also pairs this caller with the
     * worker's leaf, which is why it cannot be a bare poll of worker_ready. */
    int rc;

    errno = 0;
    Py_BEGIN_ALLOW_THREADS;
    rc = self->ops->wait_ready(self->ring, &self->ctx, RING_READY_MS);
    Py_END_ALLOW_THREADS;
    if (rc <= 0)
    {
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
    Py_RETURN_NONE;
}

static PyObject *
Ring_open(RingObject *self, PyObject *args)
{
    const char *seg, *query;
    if (!PyArg_ParseTuple(args, "ss", &seg, &query)) return nullptr;
    memcpy(self->data, seg, strlen(seg) + 1);
    if (ring_request(self, FASTPOSTGRES_OP_OPEN, self->data, query, 0) < 0) return nullptr;
    Py_RETURN_NONE;
}

static PyObject *
Ring_next(RingObject *self, PyObject *args)
{
    long long batch = 100000;
    if (!PyArg_ParseTuple(args, "|L", &batch)) return nullptr;
    if (batch <= 0) batch = 100000;
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
    long long n = ring_request(self, FASTPOSTGRES_OP_NEXT_ROWS, self->data, self->data, (uint32_t)batch);
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
    if (ring_send(self, FASTPOSTGRES_OP_NEXT_ROWS, self->data, seg, (uint32_t)batch) < 0) return nullptr;
    Py_RETURN_NONE;
}

/* Collect the response to the request last sent. */
static PyObject *
Ring_wait(RingObject *self, PyObject *)
{
    long long n = ring_collect(self);
    if (n < 0) return nullptr;
    return PyLong_FromLongLong(n);
}

static PyObject *
Ring_close(RingObject *self, PyObject *)
{
    if (ring_request(self, FASTPOSTGRES_OP_CLOSE, self->data, nullptr, 0) < 0) return nullptr;
    Py_RETURN_NONE;
}

static PyObject *
Ring_stop(RingObject *self, PyObject *)
{
    if (self->ring == nullptr || self->ops == nullptr || self->done)
    {
        Py_RETURN_NONE;
    }

    FastpostgresRing *r = self->ring;
    r->op               = FASTPOSTGRES_OP_STOP;

    /* Best effort, as before: the shm doorbell ignores a failed post, and on
     * regipc the worker also sees op == STOP on its next timeout. A teardown
     * path must not turn that into an exception. */
    uint64_t seq = __atomic_load_n(&r->req_seq, __ATOMIC_RELAXED) + 1;

    if (self->ops->post_request(r, &self->ctx, seq, RING_STOP_MS) > 0)
    {
        if (__atomic_load_n(&r->req_seq, __ATOMIC_RELAXED) < seq)
            __atomic_store_n(&r->req_seq, seq, __ATOMIC_RELEASE);
    }

    /* The caller leaves first: regipc's QUIT unlinks this end of the pair and
     * the fd goes; the worker's own fini then runs on its side. */
    ring_fini(self);
    Py_RETURN_NONE;
}

static PyMethodDef Ring_methods[] = {
    {"start", (PyCFunction)Ring_start, METH_O, "start(conn): bootstrap the worker"},
    {"open", (PyCFunction)Ring_open, METH_VARARGS, "open(seg, query): start a stream"},
    {"next", (PyCFunction)Ring_next, METH_VARARGS, "next(batch=100000) -> int rows (the columnar frame)"},
    {"next_rows", (PyCFunction)Ring_next_rows, METH_VARARGS,
     "next_rows(batch=100000) -> int rows (the row frame; falls back to columnar)"},
    {"post_rows", (PyCFunction)Ring_post_rows, METH_VARARGS,
     "post_rows(seg, batch=100000): ask for a batch into seg, do not wait"},
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
