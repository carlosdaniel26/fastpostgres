/* connection.cpp — Connection(conninfo): libpq exec and COPY FROM STDIN, the
 * statements that have no cursor to stream through. Values are decoded by type
 * OID (codec.cpp) so callers see real Python types. */
#include "connection.hpp"

#include <cstdlib>
#include <cstring>

#include "codec.hpp"
#include "types.hpp"

static PyObject *
row_tuple(PGresult *res, int r, int ncols)
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

static PyObject *
result_from_pg(PGresult *res)
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

    return fp_result(fields, types, rows, row_count);
}

static void
Connection_dealloc(ConnectionObject *self)
{
    if (self->pg) PQfinish(self->pg);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
Connection_init(ConnectionObject *self, PyObject *args, PyObject *kwds)
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

static PyObject *
Connection_exec(ConnectionObject *self, PyObject *arg)
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
        PyObject *err = fp_raise_libpq(res);
        PQclear(res);
        return err;
    }
    PyObject *out = result_from_pg(res);
    PQclear(res);
    return out;
}

/* Stream rows with libpq's COPY protocol, retrying while it needs flushing. */
static int
copy_put(PGconn *pg, const char *buf, size_t n)
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
static int
copy_put_object(PGconn *pg, PyObject *item)
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

static PyObject *
Connection_copy_from(ConnectionObject *self, PyObject *args)
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
        PyObject *err = fp_raise_libpq(res);
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
        PyObject *err = fp_raise_libpq(res);
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

static PyObject *
Connection_close(ConnectionObject *self, PyObject *)
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

PyTypeObject ConnectionType = {PyVarObject_HEAD_INIT(nullptr, 0)};

bool
fp_register_connection(PyObject *module)
{
    if (!fp_init_type(&ConnectionType, "fastpostgres.Connection", sizeof(ConnectionObject),
                      (destructor)Connection_dealloc, Connection_methods, nullptr))
        return false;
    ConnectionType.tp_init = (initproc)Connection_init;
    ConnectionType.tp_new  = PyType_GenericNew;

    Py_INCREF(&ConnectionType);
    if (PyModule_AddObject(module, "Connection", (PyObject *)&ConnectionType) != 0)
    {
        Py_DECREF(&ConnectionType);
        return false;
    }
    return true;
}
