/* connection.cpp — Connection(conninfo): the libpq control channel for the
 * statements that have no cursor to stream through. Values are decoded by type
 * OID (codec.cpp) so callers see real Python types. */
#include "connection.hpp"

#include <cstdlib>

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

/* ---- a PGresult held for lazy decoding -----------------------------------
 *
 * libpq has already put the text values in memory by the time PQexec returns;
 * clearing the PGresult frees them. Holding it lets the read return the same
 * lazy shape as the shared-memory frame without decoding a tuple per row up
 * front. The rows cost a PGresult-sized allocation until this object dies (or
 * to_tuples() materialises them), so it is a read-path lifetime, not a cache.
 */

typedef struct {
    PyObject_HEAD PGresult *res;
    PyObject               *fields;
    PyObject               *field_types;
    PyObject               *row_count;
} LazyResultObject;

static void
LazyResult_dealloc(LazyResultObject *self)
{
    if (self->res) PQclear(self->res);
    Py_XDECREF(self->fields);
    Py_XDECREF(self->field_types);
    Py_XDECREF(self->row_count);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static Py_ssize_t
LazyResult_length(LazyResultObject *self)
{
    return self->res ? (Py_ssize_t)PQntuples(self->res) : 0;
}

static PyObject *
LazyResult_decode_row(LazyResultObject *self, PyObject *arg)
{
    Py_ssize_t row = PyLong_AsSsize_t(arg);

    if (row == -1 && PyErr_Occurred()) return nullptr;
    if (!self->res || row < 0 || row >= (Py_ssize_t)PQntuples(self->res))
    {
        PyErr_SetString(PyExc_IndexError, "row out of range for this result");
        return nullptr;
    }
    return row_tuple(self->res, (int)row, PQnfields(self->res));
}

static PyObject *
lazy_decode_range(LazyResultObject *self, Py_ssize_t start, Py_ssize_t count)
{
    if (start < 0 || count < 0 || !self->res ||
        start + count > (Py_ssize_t)PQntuples(self->res))
    {
        PyErr_SetString(PyExc_IndexError, "row range out of range for this result");
        return nullptr;
    }

    int       ncols = PQnfields(self->res);
    PyObject *rows  = PyList_New(count);
    if (!rows) return nullptr;
    for (Py_ssize_t i = 0; i < count; i++)
    {
        PyObject *row = row_tuple(self->res, (int)(start + i), ncols);
        if (!row)
        {
            Py_DECREF(rows);
            return nullptr;
        }
        PyList_SET_ITEM(rows, i, row);
    }
    return rows;
}

static PyObject *
LazyResult_decode_rows(LazyResultObject *self, PyObject *args)
{
    Py_ssize_t start, count;

    if (!PyArg_ParseTuple(args, "nn", &start, &count)) return nullptr;
    return lazy_decode_range(self, start, count);
}

static PyObject *
LazyResult_to_tuples(LazyResultObject *self, PyObject *)
{
    return lazy_decode_range(self, 0, self->res ? (Py_ssize_t)PQntuples(self->res) : 0);
}

static PyMemberDef LazyResult_members[] = {
    {"fields", Py_T_OBJECT_EX, offsetof(LazyResultObject, fields), Py_READONLY, nullptr},
    {"field_types", Py_T_OBJECT_EX, offsetof(LazyResultObject, field_types), Py_READONLY, nullptr},
    {"row_count", Py_T_OBJECT_EX, offsetof(LazyResultObject, row_count), Py_READONLY, nullptr},
    {nullptr},
};

static PyMethodDef LazyResult_methods[] = {
    {"decode_row", (PyCFunction)LazyResult_decode_row, METH_O, "decode_row(row) -> tuple"},
    {"decode_rows", (PyCFunction)LazyResult_decode_rows, METH_VARARGS,
     "decode_rows(start, count) -> list of tuples"},
    {"to_tuples", (PyCFunction)LazyResult_to_tuples, METH_NOARGS, "to_tuples() -> list of tuples"},
    {nullptr},
};

static PySequenceMethods LazyResult_as_sequence = {
    (lenfunc)LazyResult_length, /* sq_length */
};

static PyTypeObject LazyResultType = {PyVarObject_HEAD_INIT(nullptr, 0)};

static PyObject *
lazy_result_from_pg(PGresult *res)
{
    int      ncols = PQnfields(res);
    PyObject *fields = PyList_New(ncols);
    PyObject *types  = PyList_New(ncols);
    long      row_count = PQntuples(res);

    if (!fields || !types)
    {
        Py_XDECREF(fields);
        Py_XDECREF(types);
        PQclear(res);
        return nullptr;
    }
    for (int f = 0; f < ncols; f++)
    {
        PyList_SET_ITEM(fields, f, PyUnicode_FromString(PQfname(res, f)));
        PyList_SET_ITEM(types, f, PyLong_FromLong((long)PQftype(res, f)));
    }

    LazyResultObject *out = PyObject_New(LazyResultObject, &LazyResultType);
    if (!out)
    {
        Py_DECREF(fields);
        Py_DECREF(types);
        PQclear(res);
        return nullptr;
    }
    out->res         = res;
    out->fields      = fields;
    out->field_types = types;
    out->row_count   = PyLong_FromLong(row_count);
    if (out->row_count == nullptr)
    {
        Py_DECREF(out); /* LazyResult_dealloc frees res and the lists */
        return nullptr;
    }
    return (PyObject *)out;
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

static PyObject *
Connection_exec_lazy(ConnectionObject *self, PyObject *arg)
{
    if (!self->pg)
    {
        PyErr_SetString(PyExc_ValueError, "connection is closed");
        return nullptr;
    }
    if (!PyUnicode_Check(arg))
    {
        PyErr_SetString(PyExc_TypeError, "exec_lazy(sql): sql must be str");
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
    return lazy_result_from_pg(res); /* the result owns and clears it */
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
    {"exec", (PyCFunction)Connection_exec, METH_O, "exec(sql) -> Result (eager tuples)"},
    {"exec_lazy", (PyCFunction)Connection_exec_lazy, METH_O,
     "exec_lazy(sql) -> LazyResult (holds the PGresult, decodes on demand)"},
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

    /* the lazy libpq result is the fallback's shape: same sequence surface as
     * the shared-memory ResultSet, backed by a held PGresult */
    LazyResultType.tp_as_sequence = &LazyResult_as_sequence;
    if (!fp_init_type(&LazyResultType, "fastpostgres.LazyResult", sizeof(LazyResultObject),
                      (destructor)LazyResult_dealloc, LazyResult_methods,
                      LazyResult_members))
        return false;

    Py_INCREF(&ConnectionType);
    if (PyModule_AddObject(module, "Connection", (PyObject *)&ConnectionType) != 0)
    {
        Py_DECREF(&ConnectionType);
        return false;
    }
    Py_INCREF(&LazyResultType);
    if (PyModule_AddObject(module, "LazyResult", (PyObject *)&LazyResultType) != 0)
    {
        Py_DECREF(&LazyResultType);
        return false;
    }
    return true;
}
