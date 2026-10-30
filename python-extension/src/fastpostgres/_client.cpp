/* _client.cpp — the fastpostgres Python client.
 *
 * A C++ extension over libpq and the shared-memory transport:
 *
 *   Connection(conninfo)      libpq control channel (exec / COPY FROM)
 *   Ring(seg)                 drives the backend worker through /dev/shm
 *   read(segName)             maps a columnar segment as zero-copy memoryviews
 *
 * The segment format and its codec live in the PostgreSQL extension's header
 * (pg-extension/fastpostgres_shm.h), which both sides include.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <libpq-fe.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "codec.hpp"
#include "fastpostgres_shm.h"
#include "ring.h"
#include "shm_map.h"
#include "synth.hpp"

/* ------------------------------------------------------------------ utils */

static PyObject *raise_libpq(PGresult *res)
{
    PyErr_Format(PyExc_RuntimeError, "libpq: %s", res ? PQresultErrorMessage(res) : "unknown error");
    return nullptr;
}

/* A read-only memoryview aliasing a region of a cached mapping (zero-copy).
 * The mapping outlives the process, so the view stays valid. */
static PyObject *memory_view(const void *p, size_t n)
{
    if (p == nullptr) Py_RETURN_NONE;
    PyObject *v = PyMemoryView_FromMemory((char *)p, (Py_ssize_t)n, PyBUF_READ);
    if (v == nullptr)
    {
        PyErr_Clear();
        Py_RETURN_NONE;
    }
    return v;
}

/* ------------------------------------------------------------- Result type */

typedef struct {
    PyObject_HEAD PyObject *fields;
    PyObject               *field_types;
    PyObject               *row_count;
    PyObject               *rows;
} ResultObject;

static void Result_dealloc(ResultObject *self)
{
    Py_XDECREF(self->fields);
    Py_XDECREF(self->field_types);
    Py_XDECREF(self->row_count);
    Py_XDECREF(self->rows);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyMemberDef Result_members[] = {
    {"fields", Py_T_OBJECT_EX, offsetof(ResultObject, fields), Py_READONLY, nullptr},
    {"field_types", Py_T_OBJECT_EX, offsetof(ResultObject, field_types), Py_READONLY, nullptr},
    {"row_count", Py_T_OBJECT_EX, offsetof(ResultObject, row_count), Py_READONLY, nullptr},
    {"rows", Py_T_OBJECT_EX, offsetof(ResultObject, rows), Py_READONLY, nullptr},
    {nullptr},
};

static PyTypeObject ResultType = {PyVarObject_HEAD_INIT(nullptr, 0)};

static PyObject *row_tuple(PGresult *res, int r, int ncols)
{
    PyObject *row = PyTuple_New(ncols);
    if (!row) return nullptr;
    for (int f = 0; f < ncols; f++)
    {
        PyObject *v = fp_decode_value(res, r, f);
        if (!v)
        {
            Py_DECREF(row);
            return nullptr;
        }
        PyTuple_SET_ITEM(row, f, v);
    }
    return row;
}

static PyObject *result_from_pg(PGresult *res)
{
    int         nrows     = PQntuples(res);
    int         ncols     = PQnfields(res);
    const char *tuples    = PQcmdTuples(res);
    long        row_count = (tuples && tuples[0]) ? strtol(tuples, nullptr, 10) : 0;
    PyObject   *fields    = PyList_New(ncols);
    PyObject   *types     = PyList_New(ncols);
    PyObject   *rows      = PyList_New(nrows);

    if (!fields || !types || !rows)
    {
        Py_XDECREF(fields);
        Py_XDECREF(types);
        Py_XDECREF(rows);
        return nullptr;
    }

    for (int f = 0; f < ncols; f++)
    {
        PyList_SET_ITEM(fields, f, PyUnicode_FromString(PQfname(res, f)));
        PyList_SET_ITEM(types, f, PyLong_FromLong((long)PQftype(res, f)));
    }

    for (int r = 0; r < nrows; r++)
    {
        PyObject *row = row_tuple(res, r, ncols);
        if (!row)
        {
            Py_DECREF(fields);
            Py_DECREF(types);
            Py_DECREF(rows);
            return nullptr;
        }
        PyList_SET_ITEM(rows, r, row);
    }

    ResultObject *result = PyObject_New(ResultObject, &ResultType);
    if (!result)
    {
        Py_DECREF(fields);
        Py_DECREF(types);
        Py_DECREF(rows);
        return nullptr;
    }
    result->fields      = fields;
    result->field_types = types;
    result->rows        = rows;
    result->row_count   = PyLong_FromLong(row_count);
    return (PyObject *)result;
}

/* --------------------------------------------------------- Connection type */

typedef struct {
    PyObject_HEAD PGconn *pg;
} ConnectionObject;

static void Connection_dealloc(ConnectionObject *self)
{
    if (self->pg) PQfinish(self->pg);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int Connection_init(ConnectionObject *self, PyObject *args, PyObject *kwds)
{
    static const char *kwlist[] = {"conninfo", nullptr};
    const char        *conninfo = "";
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|s", (char **)kwlist, &conninfo)) return -1;

    PGconn                *pg;
    Py_BEGIN_ALLOW_THREADS pg = PQconnectdb(conninfo);
    Py_END_ALLOW_THREADS

        if (PQstatus(pg) != CONNECTION_OK)
    {
        PyErr_Format(PyExc_ConnectionError, "libpq: %s", PQerrorMessage(pg));
        PQfinish(pg);
        return -1;
    }
    self->pg = pg;
    return 0;
}

static PyObject *Connection_exec(ConnectionObject *self, PyObject *arg)
{
    if (!self->pg)
    {
        PyErr_SetString(PyExc_ValueError, "connection is closed");
        return nullptr;
    }
    if (!PyUnicode_Check(arg))
    {
        PyErr_SetString(PyExc_TypeError, "exec(sql): sql must be str");
        return nullptr;
    }
    const char *sql = PyUnicode_AsUTF8(arg);
    if (!sql) return nullptr;

    PGresult              *res;
    Py_BEGIN_ALLOW_THREADS res = PQexec(self->pg, sql);
    Py_END_ALLOW_THREADS

        ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK)
    {
        PyObject *err = raise_libpq(res);
        PQclear(res);
        return err;
    }
    PyObject *out = result_from_pg(res);
    PQclear(res);
    return out;
}

/* Stream rows with libpq's COPY protocol, retrying while it needs flushing. */
static int copy_put(PGconn *pg, const char *buf, size_t n)
{
    size_t off = 0;
    while (off < n)
    {
        size_t chunk = n - off;
        if (chunk > (size_t)(1 << 20)) chunk = 1 << 20;
        int rc = PQputCopyData(pg, buf + off, (int)chunk);
        if (rc < 0) return -1;
        if (rc == 0)
        {
            if (PQflush(pg) != 0) return -1;
            continue; /* retry the same chunk */
        }
        off += chunk;
    }
    return 0;
}

/* Send one bytes-like item; returns 0, or -1 with a Python error set. */
static int copy_put_object(PGconn *pg, PyObject *item)
{
    Py_buffer view;
    if (PyObject_GetBuffer(item, &view, PyBUF_SIMPLE) != 0)
    {
        PyErr_SetString(PyExc_TypeError, "copy_from: data must be bytes-like, or a sequence of them");
        return -1;
    }
    int rc = copy_put(pg, (const char *)view.buf, (size_t)view.len);
    PyBuffer_Release(&view);
    if (rc != 0)
    {
        PyErr_Format(PyExc_RuntimeError, "libpq: %s", PQerrorMessage(pg));
        return -1;
    }
    return 0;
}

static PyObject *Connection_copy_from(ConnectionObject *self, PyObject *args)
{
    const char *sql;
    PyObject   *data;
    if (!PyArg_ParseTuple(args, "sO", &sql, &data)) return nullptr;
    if (!self->pg)
    {
        PyErr_SetString(PyExc_ValueError, "connection is closed");
        return nullptr;
    }

    PGresult *res = PQexec(self->pg, sql);
    if (PQresultStatus(res) != PGRES_COPY_IN)
    {
        PyObject *err = raise_libpq(res);
        PQclear(res);
        return err;
    }
    PQclear(res);

    int rc = 0;
    if (PyList_Check(data) || PyTuple_Check(data))
    {
        Py_ssize_t count = PySequence_Size(data);
        for (Py_ssize_t i = 0; i < count && rc == 0; i++)
        {
            PyObject *item = PySequence_GetItem(data, i);
            if (!item)
            {
                rc = -1;
                break;
            }
            rc = copy_put_object(self->pg, item);
            Py_DECREF(item);
        }
    }
    else
    {
        rc = copy_put_object(self->pg, data);
    }

    if (rc != 0)
    {
        while ((res = PQgetResult(self->pg)) != nullptr)
            PQclear(res); /* resync */
        return nullptr;
    }

    for (;;)
    {
        int e = PQputCopyEnd(self->pg, nullptr);
        if (e < 0)
        {
            while ((res = PQgetResult(self->pg)) != nullptr)
                PQclear(res);
            PyErr_Format(PyExc_RuntimeError, "libpq: %s", PQerrorMessage(self->pg));
            return nullptr;
        }
        if (e > 0) break;
        if (PQflush(self->pg) != 0)
        {
            while ((res = PQgetResult(self->pg)) != nullptr)
                PQclear(res);
            PyErr_Format(PyExc_RuntimeError, "libpq: %s", PQerrorMessage(self->pg));
            return nullptr;
        }
    }

    res = PQgetResult(self->pg);
    if (res == nullptr || PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        PyObject *err = raise_libpq(res);
        if (res) PQclear(res);
        while ((res = PQgetResult(self->pg)) != nullptr)
            PQclear(res);
        return err;
    }
    PQclear(res);
    while ((res = PQgetResult(self->pg)) != nullptr)
        PQclear(res);
    Py_RETURN_NONE;
}

static PyObject *Connection_close(ConnectionObject *self, PyObject *)
{
    if (self->pg)
    {
        PQfinish(self->pg);
        self->pg = nullptr;
    }
    Py_RETURN_NONE;
}

static PyMethodDef Connection_methods[] = {
    {"exec", (PyCFunction)Connection_exec, METH_O, "exec(sql) -> Result"},
    {"copy_from", (PyCFunction)Connection_copy_from, METH_VARARGS, "copy_from(sql, data): COPY FROM STDIN"},
    {"close", (PyCFunction)Connection_close, METH_NOARGS, "close()"},
    {nullptr},
};

static PyTypeObject ConnectionType = {PyVarObject_HEAD_INIT(nullptr, 0)};

/* ------------------------------------------------------------- Column type */

typedef struct {
    PyObject_HEAD PyObject *name;
    long                    kind;
    long                    pgtype;
    PyObject               *data;  /* whole data region (offsets + bytes for text) */
    PyObject               *bytes; /* text only: the raw byte payload */
    PyObject               *nulls; /* 1 bit per row */
} ColumnObject;

static void Column_dealloc(ColumnObject *self)
{
    Py_XDECREF(self->name);
    Py_XDECREF(self->data);
    Py_XDECREF(self->bytes);
    Py_XDECREF(self->nulls);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyMemberDef Column_members[] = {
    {"name", Py_T_OBJECT_EX, offsetof(ColumnObject, name), Py_READONLY, nullptr},
    {"kind", Py_T_LONG, offsetof(ColumnObject, kind), Py_READONLY, nullptr},
    {"pgtype", Py_T_LONG, offsetof(ColumnObject, pgtype), Py_READONLY, nullptr},
    {"data", Py_T_OBJECT_EX, offsetof(ColumnObject, data), Py_READONLY, nullptr},
    {"bytes", Py_T_OBJECT_EX, offsetof(ColumnObject, bytes), Py_READONLY, nullptr},
    {"nulls", Py_T_OBJECT_EX, offsetof(ColumnObject, nulls), Py_READONLY, nullptr},
    {nullptr},
};

static PyTypeObject ColumnType = {PyVarObject_HEAD_INIT(nullptr, 0)};

/* ------------------------------------------------------------ Segment type */

typedef struct {
    PyObject_HEAD PyObject *row_count;
    PyObject               *columns;
} SegmentObject;

static void Segment_dealloc(SegmentObject *self)
{
    Py_XDECREF(self->row_count);
    Py_XDECREF(self->columns);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyMemberDef Segment_members[] = {
    {"row_count", Py_T_OBJECT_EX, offsetof(SegmentObject, row_count), Py_READONLY, nullptr},
    {"columns", Py_T_OBJECT_EX, offsetof(SegmentObject, columns), Py_READONLY, nullptr},
    {nullptr},
};

static PyTypeObject SegmentType = {PyVarObject_HEAD_INIT(nullptr, 0)};

/* ---------------------------------------------------------- read(segName) */

typedef struct {
    std::string name;
    void       *base;
    size_t      size;
} ReadMap;

static std::vector<ReadMap> g_read_maps;

static void *read_map_get(const char *path, const std::string &name, size_t *size_out)
{
    for (ReadMap &m : g_read_maps)
    {
        if (m.name == name)
        {
            *size_out = m.size;
            return m.base;
        }
    }
    size_t size = 0;
    void  *base = fp_map_ro(path, &size);
    if (base == nullptr) return nullptr;
    g_read_maps.push_back({name, base, size});
    *size_out = size;
    return base;
}

static PyObject *module_read(PyObject *, PyObject *args)
{
    const char *name;
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;

    std::string path = "/" + std::string(name);
    size_t      size = 0;
    void       *map  = read_map_get(path.c_str(), name, &size);
    if (map == nullptr)
    {
        PyErr_SetString(PyExc_OSError, "cannot map /dev/shm segment");
        return nullptr;
    }

    uint64_t payload_len = 0;
    if (!fastpostgres_shm_validate(map, size, &payload_len))
    {
        PyErr_SetString(PyExc_ValueError, "not a fastpostgres segment");
        return nullptr;
    }

    const uint8_t     *payload = (const uint8_t *)map + FASTPOSTGRES_SHM_HEADER_SIZE;
    FastpostgresReader rd;
    if (!fastpostgres_reader_init(&rd, payload, payload_len))
    {
        PyErr_SetString(PyExc_ValueError, "not a fastpostgres columnar payload");
        return nullptr;
    }

    PyObject *columns = PyList_New(rd.ncols);
    if (!columns) return nullptr;

    for (uint32_t c = 0; c < rd.ncols; c++)
    {
        FastpostgresColumn col;
        if (!fastpostgres_reader_next(&rd, &col)) break;

        ColumnObject *column = PyObject_New(ColumnObject, &ColumnType);
        if (!column)
        {
            Py_DECREF(columns);
            return nullptr;
        }
        column->name         = PyUnicode_FromStringAndSize(col.name, col.name_len);
        column->kind         = col.kind;
        column->pgtype       = (long)col.pgtype;
        column->data         = memory_view(col.data, col.data_len);
        size_t offsets_bytes = (size_t)(col.nrows + 1) * 4;
        column->bytes        = (col.kind == FASTPOSTGRES_KIND_TEXT && col.bytes)
                                   ? memory_view(col.bytes, (size_t)col.data_len - offsets_bytes)
                                   : Py_NewRef(Py_None);
        column->nulls        = memory_view(col.nulls, (size_t)((rd.nrows + 7) / 8));
        PyList_SET_ITEM(columns, c, (PyObject *)column);
    }

    SegmentObject *segment = PyObject_New(SegmentObject, &SegmentType);
    if (!segment)
    {
        Py_DECREF(columns);
        return nullptr;
    }
    segment->columns   = columns;
    segment->row_count = PyLong_FromLongLong((long long)rd.nrows);
    return (PyObject *)segment;
}

/* Materialize a columnar chunk as typed row tuples (the DB-API fast path). */
static PyObject *module_read_rows(PyObject *, PyObject *args)
{
    const char *name;
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;

    std::string path = "/" + std::string(name);
    size_t      size = 0;
    void       *map  = read_map_get(path.c_str(), name, &size);
    if (map == nullptr)
    {
        PyErr_SetString(PyExc_OSError, "cannot map /dev/shm segment");
        return nullptr;
    }

    uint64_t payload_len = 0;
    if (!fastpostgres_shm_validate(map, size, &payload_len))
    {
        PyErr_SetString(PyExc_ValueError, "not a fastpostgres segment");
        return nullptr;
    }

    const uint8_t     *payload = (const uint8_t *)map + FASTPOSTGRES_SHM_HEADER_SIZE;
    FastpostgresReader rd;
    if (!fastpostgres_reader_init(&rd, payload, payload_len))
    {
        PyErr_SetString(PyExc_ValueError, "not a fastpostgres columnar payload");
        return nullptr;
    }

    std::vector<FastpostgresColumn> cols(rd.ncols);
    uint32_t                        got = 0;
    for (uint32_t c = 0; c < rd.ncols; c++)
    {
        if (!fastpostgres_reader_next(&rd, &cols[c])) break;
        got++;
    }
    if (got != rd.ncols)
    {
        PyErr_SetString(PyExc_ValueError, "truncated columnar payload");
        return nullptr;
    }

    PyObject *fields = PyList_New(rd.ncols);
    PyObject *types  = PyList_New(rd.ncols);
    PyObject *rows   = PyList_New((Py_ssize_t)rd.nrows);
    if (!fields || !types || !rows)
    {
        Py_XDECREF(fields);
        Py_XDECREF(types);
        Py_XDECREF(rows);
        return nullptr;
    }

    for (uint32_t c = 0; c < rd.ncols; c++)
    {
        PyList_SET_ITEM(fields, c, PyUnicode_FromStringAndSize(cols[c].name, cols[c].name_len));
        PyList_SET_ITEM(types, c, PyLong_FromLong((long)cols[c].pgtype));
    }

    for (uint64_t r = 0; r < rd.nrows; r++)
    {
        PyObject *row = PyTuple_New(rd.ncols);
        if (!row)
        {
            Py_DECREF(fields);
            Py_DECREF(types);
            Py_DECREF(rows);
            return nullptr;
        }
        for (uint32_t c = 0; c < rd.ncols; c++)
        {
            PyObject *v = fp_decode_column(&cols[c], r);
            if (!v)
            {
                Py_DECREF(row);
                Py_DECREF(fields);
                Py_DECREF(types);
                Py_DECREF(rows);
                return nullptr;
            }
            PyTuple_SET_ITEM(row, c, v);
        }
        PyList_SET_ITEM(rows, r, row);
    }

    ResultObject *result = PyObject_New(ResultObject, &ResultType);
    if (!result)
    {
        Py_DECREF(fields);
        Py_DECREF(types);
        Py_DECREF(rows);
        return nullptr;
    }
    result->fields      = fields;
    result->field_types = types;
    result->rows        = rows;
    result->row_count   = PyLong_FromLongLong((long long)rd.nrows);
    return (PyObject *)result;
}

/* -------------------------------------------------------------- Ring type */
typedef struct {
    PyObject_HEAD FastpostgresRing *ring;
    void                           *map;
    size_t                          size;
    char                            data[128]; /* the columnar segment of the current stream */
} RingObject;

static void Ring_dealloc(RingObject *self)
{
    if (self->map) munmap(self->map, self->size);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int Ring_init(RingObject *self, PyObject *args, PyObject *kwds)
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
        if (sem_init(&r->req_ready, 1, 0) != 0 || sem_init(&r->resp_ready, 1, 0) != 0 ||
            sem_init(&r->push_free, 1, FASTPOSTGRES_PUSH_SLOTS) != 0 || sem_init(&r->push_ready, 1, 0) != 0)
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
    return 0;
}

/* Send one request and block for its response; returns nrows, or -1 with a
 * Python exception set. */
static long long ring_request(RingObject *self, uint32_t op, const char *a1, const char *a2, uint32_t batch)
{
    FastpostgresRing *r = self->ring;
    size_t            n = 0;

    if (a1)
    {
        size_t l = strlen(a1);
        if (l + 1 > FASTPOSTGRES_RING_ARG)
        {
            PyErr_SetString(PyExc_ValueError, "ring: argument too long");
            return -1;
        }
        memcpy(r->arg, a1, l + 1);
        n = l + 1;
    }
    else
    {
        r->arg[0] = 0;
        n         = 1;
    }
    if (a2)
    {
        size_t l = strlen(a2);
        if (n + l + 1 > FASTPOSTGRES_RING_ARG)
        {
            PyErr_SetString(PyExc_ValueError, "ring: argument too long");
            return -1;
        }
        memcpy(r->arg + n, a2, l + 1);
    }

    r->op       = op;
    r->batch    = batch;
    r->status   = 0;
    r->nrows    = 0;
    r->error[0] = 0;

    __atomic_add_fetch(&r->req_seq, 1, __ATOMIC_RELEASE);
    sem_post(&r->req_ready);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 10;
    int                    rc;
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

static PyObject *Ring_start(RingObject *self, PyObject *conn)
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
        PyObject *err = raise_libpq(res);
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

static PyObject *Ring_open(RingObject *self, PyObject *args)
{
    const char *seg, *query;
    if (!PyArg_ParseTuple(args, "ss", &seg, &query)) return nullptr;
    memcpy(self->data, seg, strlen(seg) + 1);
    if (ring_request(self, FASTPOSTGRES_OP_OPEN, self->data, query, 0) < 0) return nullptr;
    Py_RETURN_NONE;
}

static PyObject *Ring_next(RingObject *self, PyObject *args)
{
    long long batch = 100000;
    if (!PyArg_ParseTuple(args, "|L", &batch)) return nullptr;
    if (batch <= 0) batch = 100000;
    long long n = ring_request(self, FASTPOSTGRES_OP_NEXT, self->data, nullptr, (uint32_t)batch);
    if (n < 0) return nullptr;
    return PyLong_FromLongLong(n);
}

static PyObject *Ring_close(RingObject *self, PyObject *)
{
    if (ring_request(self, FASTPOSTGRES_OP_CLOSE, self->data, nullptr, 0) < 0) return nullptr;
    Py_RETURN_NONE;
}

static PyObject *Ring_write(RingObject *self, PyObject *args)
{
    const char *seg, *table;
    if (!PyArg_ParseTuple(args, "ss", &seg, &table)) return nullptr;
    long long n = ring_request(self, FASTPOSTGRES_OP_WRITE, seg, table, 0);
    if (n < 0) return nullptr;
    return PyLong_FromLongLong(n);
}

static PyObject *Ring_stop(RingObject *self, PyObject *)
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
    {"next", (PyCFunction)Ring_next, METH_VARARGS, "next(batch=100000) -> int rows"},
    {"close", (PyCFunction)Ring_close, METH_NOARGS, "close(): end the stream"},
    {"write", (PyCFunction)Ring_write, METH_VARARGS, "write(seg, table) -> int rows"},
    {"stop", (PyCFunction)Ring_stop, METH_NOARGS, "stop(): ask the worker to exit"},
    {nullptr},
};

static PyTypeObject RingType = {PyVarObject_HEAD_INIT(nullptr, 0)};

/* ------------------------------------------------------------- module init */

static PyMethodDef module_methods[] = {
    {"read", module_read, METH_VARARGS, "read(segName) -> Segment (zero-copy column views)"},
    {"read_rows", module_read_rows, METH_VARARGS, "read_rows(segName) -> Result (typed tuple rows)"},
    {nullptr},
};

static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "fastpostgres._client",
    "fastpostgres client (shared memory + libpq)",
    -1,
    module_methods,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

static bool init_type(PyTypeObject *type, const char *name, Py_ssize_t basicsize, destructor dealloc,
                      PyMethodDef *methods, PyMemberDef *members)
{
    type->tp_name      = name;
    type->tp_basicsize = basicsize;
    type->tp_dealloc   = dealloc;
    type->tp_flags     = Py_TPFLAGS_DEFAULT;
    type->tp_methods   = methods;
    type->tp_members   = members;
    return PyType_Ready(type) == 0;
}

PyMODINIT_FUNC PyInit__client(void)
{
    if (!fp_codec_init()) return nullptr;
    if (!init_type(&ResultType, "fastpostgres.Result", sizeof(ResultObject), (destructor)Result_dealloc,
                   nullptr, Result_members))
        return nullptr;
    if (!init_type(&ConnectionType, "fastpostgres.Connection", sizeof(ConnectionObject),
                   (destructor)Connection_dealloc, Connection_methods, nullptr))
        return nullptr;
    if (!init_type(&ColumnType, "fastpostgres.Column", sizeof(ColumnObject), (destructor)Column_dealloc,
                   nullptr, Column_members))
        return nullptr;
    if (!init_type(&SegmentType, "fastpostgres.Segment", sizeof(SegmentObject), (destructor)Segment_dealloc,
                   nullptr, Segment_members))
        return nullptr;
    if (!init_type(&RingType, "fastpostgres.Ring", sizeof(RingObject), (destructor)Ring_dealloc, Ring_methods,
                   nullptr))
        return nullptr;
    ConnectionType.tp_init = (initproc)Connection_init;
    ConnectionType.tp_new  = PyType_GenericNew;
    RingType.tp_init       = (initproc)Ring_init;
    RingType.tp_new        = PyType_GenericNew;

    PyObject *m = PyModule_Create(&moduledef);
    if (!m) return nullptr;

    struct {
        const char   *name;
        PyTypeObject *type;
    } types[] = {
        {"Result", &ResultType}, {"Connection", &ConnectionType},
        {"Column", &ColumnType}, {"Segment", &SegmentType},
        {"Ring", &RingType},
    };
    for (auto &t : types)
    {
        Py_INCREF(t.type);
        if (PyModule_AddObject(m, t.name, (PyObject *)t.type) != 0)
        {
            Py_DECREF(t.type);
            Py_DECREF(m);
            return nullptr;
        }
    }
    fp_register_synth(m);
    return m;
}
