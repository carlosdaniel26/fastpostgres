/* types.hpp — the Python types shared by the client modules, plus small value
 * and error helpers. */
#pragma once

#include <Python.h>
#include <libpq-fe.h>

#include <cstddef>

/* exec() / read_rows() result: column names, type OIDs, affected-row count and
 * the rows themselves (tuples). */
typedef struct {
    PyObject_HEAD PyObject *fields;
    PyObject               *field_types;
    PyObject               *row_count;
    PyObject               *rows;
} ResultObject;

/* One column of a columnar segment, exposed as zero-copy memoryviews. */
typedef struct {
    PyObject_HEAD PyObject *name;
    long                    kind;
    long                    pgtype;
    PyObject               *data;  /* whole data region (offsets + bytes for text) */
    PyObject               *bytes; /* text only: the raw byte payload */
    PyObject               *nulls; /* 1 bit per row */
} ColumnObject;

/* A columnar chunk: row count plus the column views. */
typedef struct {
    PyObject_HEAD PyObject *row_count;
    PyObject               *columns;
} SegmentObject;

extern PyTypeObject ResultType;
extern PyTypeObject ColumnType;
extern PyTypeObject SegmentType;

/* Fill a type object and PyType_Ready() it. */
bool fp_init_type(PyTypeObject *type, const char *name, Py_ssize_t basicsize, destructor dealloc,
                  PyMethodDef *methods, PyMemberDef *members);

/* Ready the shared types, and add them to `module`. */
bool fp_types_init(void);
bool fp_register_types(PyObject *module);

/* Build a Result from three lists; takes ownership of them. */
PyObject *fp_result(PyObject *fields, PyObject *field_types, PyObject *rows, long row_count);

/* A RuntimeError carrying a PGresult's message. */
PyObject *fp_raise_libpq(PGresult *res);

/* A read-only memoryview aliasing a cached mapping (zero-copy), or None. */
PyObject *fp_memory_view(const void *p, size_t n);
