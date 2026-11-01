/* ring.cpp — Ring(seg): the request/response channel the backend worker
 * services. The client creates /dev/shm/<seg>, initialises the semaphores, and
 * bootstraps the worker with one SQL call over libpq (start); open/next/close
 * then travel through shared memory only. */
#include "ring.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <string>

#include "connection.hpp"
#include "fastpostgres_shm.h"
#include "types.hpp"

static void
Ring_dealloc(RingObject *self)
{
    if (self->map) munmap(self->map, self->size);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
Ring_init(RingObject *self, PyObject *args, PyObject *kwds)
{
    static const char *kwlist[] = {"segment", nullptr};
    const char        *seg;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s", (char **)kwlist, &seg)) return -1;

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

    self->ring = r;
    self->map  = map;
    self->size = FASTPOSTGRES_RING_BYTES;
    memcpy(self->data, seg, std::strlen(seg) + 1);
    r->client_pid = (uint32_t)getpid(); /* the worker exits once this process is gone */
    return 0;
}

/* Hand one request to the worker. The response is collected by ring_collect();
 * one request is in flight at a time, so the ring's single op/arg/nrows slot is
 * enough, and the caller may do other work before waiting. */
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

    __atomic_add_fetch(&r->req_seq, 1, __ATOMIC_RELEASE);
    sem_post(&r->req_ready);
    return 0;
}

/* Block for the response to the request last sent; nrows, or -1 with a Python
 * exception set. */
static long long
ring_collect(RingObject *self)
{
    FastpostgresRing *r = self->ring;
    struct timespec   ts;
    int               rc;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 10;
    Py_BEGIN_ALLOW_THREADS rc = sem_timedwait(&r->resp_ready, &ts);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        PyErr_SetString(PyExc_TimeoutError, "ring: timeout waiting for the worker");
        return -1;
    }
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

    std::string            sql = "SELECT fastpostgres_ring_start('" + std::string(self->data) + "')";
    PGresult              *res;
    Py_BEGIN_ALLOW_THREADS res = PQexec(pg, sql.c_str());
    Py_END_ALLOW_THREADS if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        PyObject *err = fp_raise_libpq(res);
        PQclear(res);
        return err;
    }
    PQclear(res);

    int waited = 0;
    while (__atomic_load_n(&self->ring->worker_ready, __ATOMIC_ACQUIRE) == 0)
    {
        struct timespec pause = {0, 1000000}; /* 1 ms */
        nanosleep(&pause, nullptr);
        if (++waited > 10000)
        {
            PyErr_SetString(PyExc_TimeoutError, "ring: worker did not become ready");
            return nullptr;
        }
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
    FastpostgresRing *r = self->ring;
    r->op               = FASTPOSTGRES_OP_STOP;
    __atomic_add_fetch(&r->req_seq, 1, __ATOMIC_RELEASE);
    sem_post(&r->req_ready);
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
