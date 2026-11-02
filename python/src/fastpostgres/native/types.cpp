/* types.cpp — Result / Column / Segment: the Python-visible value types. */
#include "types.hpp"

#include <cstddef>
#include <cstring>

#include "codec.hpp"

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

/* Once per column, not once per value: the same bulk test the decode path uses
 * to decide whether it can skip its per-row null checks. */
static int
column_has_nulls(ColumnObject *self)
{
    size_t nbytes = (size_t)(((size_t)self->nrows + 7) / 8);

    const uint8_t *bits = fp_col_bytes(self->nulls);

    if (bits == nullptr || nbytes == 0) return 0;
    return fastpostgres_nullmap_any(bits, nbytes);
}

/* The numpy dtype for a fixed-width kind; nullptr for the varlena kinds. The
 * date/time kinds are their wire integers (days, microseconds), not objects:
 * that is what makes the view free. */
static const char *
column_typestr(long kind)
{
    switch (kind)
    {
        case FASTPOSTGRES_KIND_BOOL:
            return "|b1";
        case FASTPOSTGRES_KIND_INT16:
            return "<i2";
        case FASTPOSTGRES_KIND_INT32:
        case FASTPOSTGRES_KIND_DATE: /* days since 2000-01-01 */
            return "<i4";
        case FASTPOSTGRES_KIND_FLOAT4:
            return "<f4";
        case FASTPOSTGRES_KIND_FLOAT8:
            return "<f8";
        case FASTPOSTGRES_KIND_INT64:
        case FASTPOSTGRES_KIND_TIME: /* microseconds since midnight */
        case FASTPOSTGRES_KIND_TIMESTAMP:
        case FASTPOSTGRES_KIND_TIMESTAMPTZ:
            return "<i8";
        case FASTPOSTGRES_KIND_UUID:
            return "|V16";
        default:
            return nullptr;
    }
}

/* One decoded value, on demand: the row is materialised only if it is asked
 * for, so a consumer that reads a column's summary allocates nothing. */
static PyObject *
Column_value_at(ColumnObject *self, PyObject *arg)
{
    FastpostgresColumn c;
    long               row = PyLong_AsLong(arg);

    if (row == -1 && PyErr_Occurred()) return nullptr;
    if (row < 0 || row >= self->nrows)
    {
        PyErr_SetString(PyExc_IndexError, "row out of range for this chunk");
        return nullptr;
    }
    memset(&c, 0, sizeof(c));
    c.kind   = (uint8_t)self->kind;
    c.pgtype = (uint32_t)self->pgtype;
    c.data   = fp_col_bytes(self->data);
    c.bytes  = fp_col_bytes(self->bytes);
    c.nulls  = fp_col_bytes(self->nulls);
    c.nrows  = (uint64_t)self->nrows;
    return fp_decode_column(&c, (uint64_t)row);
}

static PyObject *
Column_get_has_nulls(ColumnObject *self, void *)
{
    if (column_has_nulls(self)) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

/* For the varlena kinds: the int32 offset table on its own, one entry per row
 * plus the end, so text can be sliced without building a string per value. */
static PyObject *
Column_get_offsets(ColumnObject *self, void *)
{
    PyObject *v;

    if (!fastpostgres_kind_is_varlena((uint8_t)self->kind)) Py_RETURN_NONE;
    v = PyMemoryView_FromMemory((char *)fp_col_bytes(self->data), (Py_ssize_t)(((size_t)self->nrows + 1) * 4),
                                PyBUF_READ);
    if (v == nullptr)
    {
        PyErr_Clear();
        Py_RETURN_NONE;
    }
    return v;
}

/* Lets numpy wrap the segment without copying: np.asarray(column) is one object
 * for the whole column instead of one per value. */
static PyObject *
Column_get_array_interface(ColumnObject *self, void *)
{
    const char *typestr = column_typestr(self->kind);
    PyObject   *dict, *shape, *data, *ts, *ver;
    int         failed;

    if (typestr == nullptr)
    {
        PyErr_SetString(PyExc_TypeError, "varlena column: use .offsets and .bytes");
        return nullptr;
    }
    if (column_has_nulls(self))
    {
        /* the null slots hold zeros, so a typed view would quietly lie */
        PyErr_SetString(PyExc_ValueError, "column has nulls: materialise it with read_rows() first");
        return nullptr;
    }

    dict  = PyDict_New();
    shape = Py_BuildValue("(n)", (Py_ssize_t)self->nrows);
    data = Py_BuildValue("(NK)", PyLong_FromVoidPtr((void *)fp_col_bytes(self->data)), (unsigned long long)1);
    ts   = PyUnicode_FromString(typestr);
    ver  = PyLong_FromLong(3);
    if (!dict || !shape || !data || !ts || !ver)
    {
        Py_XDECREF(dict);
        Py_XDECREF(shape);
        Py_XDECREF(data);
        Py_XDECREF(ts);
        Py_XDECREF(ver);
        return nullptr;
    }

    failed =
        PyDict_SetItemString(dict, "shape", shape) != 0 || PyDict_SetItemString(dict, "typestr", ts) != 0 ||
        PyDict_SetItemString(dict, "data", data) != 0 || PyDict_SetItemString(dict, "version", ver) != 0 ||
        PyDict_SetItemString(dict, "readonly", Py_True) != 0;
    Py_DECREF(shape);
    Py_DECREF(data);
    Py_DECREF(ts);
    Py_DECREF(ver);
    if (failed)
    {
        Py_DECREF(dict);
        return nullptr;
    }
    return dict;
}

static PyMemberDef Column_members[] = {
    {"name", Py_T_OBJECT_EX, offsetof(ColumnObject, name), Py_READONLY, nullptr},
    {"kind", Py_T_LONG, offsetof(ColumnObject, kind), Py_READONLY, nullptr},
    {"pgtype", Py_T_LONG, offsetof(ColumnObject, pgtype), Py_READONLY, nullptr},
    {"nrows", Py_T_LONG, offsetof(ColumnObject, nrows), Py_READONLY, nullptr},
    {"data", Py_T_OBJECT_EX, offsetof(ColumnObject, data), Py_READONLY, nullptr},
    {"bytes", Py_T_OBJECT_EX, offsetof(ColumnObject, bytes), Py_READONLY, nullptr},
    {"nulls", Py_T_OBJECT_EX, offsetof(ColumnObject, nulls), Py_READONLY, nullptr},
    {nullptr},
};

static PyMethodDef Column_methods[] = {
    {"value_at", (PyCFunction)Column_value_at, METH_O, "value_at(row) -> one decoded value"},
    {nullptr},
};

static PyGetSetDef Column_getset[] = {
    {"has_nulls", (getter)Column_get_has_nulls, nullptr, "True when any row in this chunk is null", nullptr},
    {"offsets", (getter)Column_get_offsets, nullptr,
     "varlena kinds: zero-copy int32 offsets, one per row plus the end", nullptr},
    {"__array_interface__", (getter)Column_get_array_interface, nullptr,
     "zero-copy typed view of a fixed-width, null-free column", nullptr},
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
    /* set before fp_init_type, which readies the type */
    ColumnType.tp_getset = Column_getset;
    if (!fp_init_type(&ColumnType, "fastpostgres.Column", sizeof(ColumnObject), (destructor)Column_dealloc,
                      Column_methods, Column_members))
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
