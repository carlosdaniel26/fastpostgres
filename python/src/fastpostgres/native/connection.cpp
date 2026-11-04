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
