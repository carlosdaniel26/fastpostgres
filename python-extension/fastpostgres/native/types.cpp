/* types.cpp — Result / Column / Segment: the Python-visible value types. */
#include "types.hpp"

#include <cstddef>

/* ------------------------------------------------------------------ Result */

static void
Result_dealloc(ResultObject *self)
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

PyTypeObject ResultType = {PyVarObject_HEAD_INIT(nullptr, 0)};

/* ------------------------------------------------------------------ Column */

static void
Column_dealloc(ColumnObject *self)
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

PyTypeObject ColumnType = {PyVarObject_HEAD_INIT(nullptr, 0)};

/* ----------------------------------------------------------------- Segment */

static void
Segment_dealloc(SegmentObject *self)
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

PyTypeObject SegmentType = {PyVarObject_HEAD_INIT(nullptr, 0)};

/* ------------------------------------------------------------------ shared */

bool
fp_init_type(PyTypeObject *type, const char *name, Py_ssize_t basicsize, destructor dealloc,
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

PyObject *
fp_result(PyObject *fields, PyObject *field_types, PyObject *rows, long row_count)
{
    ResultObject *result = PyObject_New(ResultObject, &ResultType);
    if (!result)
    {
        Py_DECREF(fields);
        Py_DECREF(field_types);
        Py_DECREF(rows);
        return nullptr;
    }
    result->fields      = fields;
    result->field_types = field_types;
    result->rows        = rows;
    result->row_count   = PyLong_FromLong(row_count);
    return (PyObject *)result;
}

PyObject *
fp_raise_libpq(PGresult *res)
{
    PyErr_Format(PyExc_RuntimeError, "libpq: %s", res ? PQresultErrorMessage(res) : "unknown error");
    return nullptr;
}

PyObject *
fp_memory_view(const void *p, size_t n)
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

bool
fp_types_init(void)
{
    if (!fp_init_type(&ResultType, "fastpostgres.Result", sizeof(ResultObject), (destructor)Result_dealloc,
                      nullptr, Result_members))
        return false;
    if (!fp_init_type(&ColumnType, "fastpostgres.Column", sizeof(ColumnObject), (destructor)Column_dealloc,
                      nullptr, Column_members))
        return false;
    if (!fp_init_type(&SegmentType, "fastpostgres.Segment", sizeof(SegmentObject),
                      (destructor)Segment_dealloc, nullptr, Segment_members))
        return false;
    return true;
}

bool
fp_register_types(PyObject *module)
{
    struct {
        const char   *name;
        PyTypeObject *type;
    } types[] = {
        {"Result", &ResultType},
        {"Column", &ColumnType},
        {"Segment", &SegmentType},
    };

    for (auto &t : types)
    {
        Py_INCREF(t.type);
        if (PyModule_AddObject(module, t.name, (PyObject *)t.type) != 0)
        {
            Py_DECREF(t.type);
            return false;
        }
    }
    return true;
}
