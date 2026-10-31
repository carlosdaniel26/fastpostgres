/* segment.cpp — read a columnar segment (/dev/shm/<seg>) as either zero-copy
 * column views (read) or materialized typed row tuples (read_rows). A mapping
 * is kept for the process lifetime: the worker rewrites the same segment in
 * place, so each read only revalidates the header. */
#include "segment.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "codec.hpp"
#include "fastpostgres_shm.h"
#include "kernels.h"
#include "shm_map.h"
#include "types.hpp"

typedef struct {
    std::string name;
    void       *base;
    size_t      size;
} ReadMap;

static std::vector<ReadMap> g_read_maps;

static void *
read_map_get(const char *path, const std::string &name, size_t *size_out)
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

/* Map the segment and point a reader at its FPCOLUMN payload. */
static bool
segment_reader(const char *name, FastpostgresReader *rd, size_t *mapped_size)
{
    std::string path = "/" + std::string(name);
    size_t      size = 0;
    void       *map  = read_map_get(path.c_str(), name, &size);
    if (map == nullptr)
    {
        PyErr_SetString(PyExc_OSError, "cannot map /dev/shm segment");
        return false;
    }

    uint64_t payload_len = 0;
    if (!fastpostgres_shm_validate(map, size, &payload_len))
    {
        PyErr_SetString(PyExc_ValueError, "not a fastpostgres segment");
        return false;
    }
    if (!fastpostgres_reader_init(rd, (const uint8_t *)map + FASTPOSTGRES_SHM_HEADER_SIZE, payload_len))
    {
        PyErr_SetString(PyExc_ValueError, "not a fastpostgres columnar payload");
        return false;
    }
    *mapped_size = size;
    return true;
}

static PyObject *
segment_read(PyObject *, PyObject *args)
{
    const char *name;
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;

    FastpostgresReader rd;
    size_t             size = 0;
    if (!segment_reader(name, &rd, &size)) return nullptr;

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
        column->nrows        = (long)rd.nrows;
        column->vdata        = col.data; /* the mapping outlives the chunk, so these stay valid */
        column->vbytes       = col.bytes;
        column->vnulls       = col.nulls;
        column->data         = fp_memory_view(col.data, col.data_len);
        size_t offsets_bytes = (size_t)(col.nrows + 1) * 4;
        column->bytes        = (fastpostgres_kind_is_varlena(col.kind) && col.bytes)
                                   ? fp_memory_view(col.bytes, (size_t)col.data_len - offsets_bytes)
                                   : Py_NewRef(Py_None);
        column->nulls        = fp_memory_view(col.nulls, (size_t)((rd.nrows + 7) / 8));
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

/* Materialize the chunk as typed row tuples (the DB-API fast path). */
static PyObject *
segment_read_rows(PyObject *, PyObject *args)
{
    const char *name;
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;

    FastpostgresReader rd;
    size_t             size = 0;
    if (!segment_reader(name, &rd, &size)) return nullptr;

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

    /* One bulk null test per column, rather than a bitmap probe per value:
     * a column with no nulls skips the test for every row it holds. */
    std::vector<char> nullfree(rd.ncols);
    for (uint32_t c = 0; c < rd.ncols; c++)
        nullfree[c] = !fp_kernels->nullmap_any(cols[c].nulls, (size_t)((rd.nrows + 7) / 8));

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
            PyObject *v = nullfree[c] ? fp_decode_column_nonull(&cols[c], r)
                                      : fp_decode_column(&cols[c], r);
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

    return fp_result(fields, types, rows, (long)rd.nrows);
}

static PyMethodDef segment_methods[] = {
    {"read", segment_read, METH_VARARGS, "read(segName) -> Segment (zero-copy column views)"},
    {"read_rows", segment_read_rows, METH_VARARGS, "read_rows(segName) -> Result (typed tuple rows)"},
    {nullptr},
};

bool
fp_register_segment(PyObject *module)
{
    for (PyMethodDef *m = segment_methods; m->ml_name != nullptr; m++)
    {
        PyObject *fn = PyCFunction_NewEx(m, nullptr, nullptr);
        if (fn == nullptr) return false;
        if (PyModule_AddObject(module, m->ml_name, fn) != 0)
        {
            Py_DECREF(fn);
            return false;
        }
    }
    return true;
}
