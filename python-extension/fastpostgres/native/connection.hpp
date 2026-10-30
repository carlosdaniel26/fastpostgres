/* connection.hpp — the libpq control channel. */
#pragma once

#include <Python.h>
#include <libpq-fe.h>

typedef struct {
    PyObject_HEAD PGconn *pg;
} ConnectionObject;

extern PyTypeObject ConnectionType;

bool fp_register_connection(PyObject *module);
