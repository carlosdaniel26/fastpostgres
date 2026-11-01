/* segment.cpp — read a result segment (/dev/shm/<seg>) as either zero-copy
 * column views (read) or materialized typed row tuples (read_rows). The worker
 * answers a row request with the row frame (FPROWS) and falls back to the
 * columnar frame (FPCOLUMN) when an attribute is not one the client can walk in
 * place; read_rows takes either. A mapping is kept for the process lifetime:
 * the worker rewrites the same segment in place, so each read only revalidates
 * the header. */
#include "segment.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <x86intrin.h>

#include "codec.hpp"
#include "fastpostgres_shm.h"
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

/* Map the segment and hand back its payload. */
static const uint8_t *
segment_payload(const char *name, uint64_t *payload_len)
{
    std::string path = "/" + std::string(name);
    size_t      size = 0;
    void       *map  = read_map_get(path.c_str(), name, &size);

    if (map == nullptr)
    {
        PyErr_SetString(PyExc_OSError, "cannot map /dev/shm segment");
        return nullptr;
    }
    if (!fastpostgres_shm_validate(map, size, payload_len))
    {
        PyErr_SetString(PyExc_ValueError, "not a fastpostgres segment");
        return nullptr;
    }
    return (const uint8_t *)map + FASTPOSTGRES_SHM_HEADER_SIZE;
}

/* The field names and type OIDs every Result carries. */
static bool
result_meta(const std::vector<FastpostgresColumn> &cols, PyObject **fields, PyObject **types)
{
    *fields = PyList_New((Py_ssize_t)cols.size());
    *types  = PyList_New((Py_ssize_t)cols.size());
    if (*fields == nullptr || *types == nullptr)
    {
        Py_XDECREF(*fields);
        Py_XDECREF(*types);
        return false;
    }
    for (size_t c = 0; c < cols.size(); c++)
    {
        PyObject *name = PyUnicode_FromStringAndSize(cols[c].name, cols[c].name_len);
        PyObject *oid  = PyLong_FromLong((long)cols[c].pgtype);

        if (name == nullptr || oid == nullptr)
        {
            Py_XDECREF(name);
            Py_XDECREF(oid);
            Py_DECREF(*fields);
            Py_DECREF(*types);
            return false;
        }
        PyList_SET_ITEM(*fields, (Py_ssize_t)c, name);
        PyList_SET_ITEM(*types, (Py_ssize_t)c, oid);
    }
    return true;
}

static PyObject *
result_from(const std::vector<FastpostgresColumn> &cols, uint64_t nrows, PyObject *rows)
{
    PyObject *fields, *types;

    if (!result_meta(cols, &fields, &types))
    {
        Py_DECREF(rows);
        return nullptr;
    }
    return fp_result(fields, types, rows, (long)nrows);
}

/* read(seg): the columnar frame as zero-copy column views. */
static PyObject *
segment_read(PyObject *, PyObject *args)
{
    const char *name;
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;

    uint64_t       payload_len = 0;
    const uint8_t *payload     = segment_payload(name, &payload_len);
    if (payload == nullptr) return nullptr;

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
        column->nrows        = (long)rd.nrows;
        column->data         = fp_memory_view(col.data, col.data_len); /* the mapping outlives the chunk */
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

    uint64_t           payload_len = 0;
    unsigned long long t0          = __rdtsc();
    const uint8_t     *payload     = segment_payload(name, &payload_len);
    if (payload == nullptr) return nullptr;

    FastpostgresRowsReader rrd;
    if (fastpostgres_rows_init(&rrd, payload, payload_len))
    {
        /* The row frame: PostgreSQL's own tuple bytes, decoded where they lie.
         * The tuples are built by the decoder as it walks each one. */
        FastpostgresRowsDesc            desc;
        std::vector<FastpostgresColumn> cols(rrd.ncols);

        fastpostgres_rows_desc_begin(&desc, payload, (uint64_t)(rrd.p - rrd.base));
        for (uint32_t c = 0; c < rrd.ncols; c++)
            if (!fastpostgres_rows_desc_next(&desc, &cols[c]))
            {
                PyErr_SetString(PyExc_ValueError, "truncated row payload");
                return nullptr;
            }

        PyObject *rows = PyList_New((Py_ssize_t)rrd.left);
        if (!rows) return nullptr;

        if (!fp_decode_tuples(cols.data(), rrd.ncols, &rrd, rows))
        {
            Py_DECREF(rows);
            return nullptr;
        }
        fp_stats_add(__rdtsc() - t0, (unsigned long long)PyList_GET_SIZE(rows));
        return result_from(cols, (uint64_t)PyList_GET_SIZE(rows), rows);
    }

    /* The columnar frame, from the fallback for attributes that have to go
     * through their output function. */
    FastpostgresReader              rd;
    std::vector<FastpostgresColumn> cols;

    if (!fastpostgres_reader_init(&rd, payload, payload_len))
    {
        PyErr_SetString(PyExc_ValueError, "not a fastpostgres payload");
        return nullptr;
    }
    cols.resize(rd.ncols);
    uint32_t got = 0;
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

    PyObject *rows = PyList_New((Py_ssize_t)rd.nrows);
    if (!rows) return nullptr;

    /* Every tuple up front, then one specialized pass per column, writing into
     * the row slots as each column is swept. A tuple with NULL slots is only
     * reachable through the list, and both the GC and the deallocator walk
     * tuple items null-safely. */
    for (uint64_t r = 0; r < rd.nrows; r++)
    {
        PyObject *row = PyTuple_New(rd.ncols);
        if (!row)
        {
            Py_DECREF(rows);
            return nullptr;
        }
        PyList_SET_ITEM(rows, (Py_ssize_t)r, row);
    }

    if (!fp_decode_columns(cols.data(), rd.ncols, rd.nrows, rows))
    {
        Py_DECREF(rows);
        return nullptr;
    }
    return result_from(cols, rd.nrows, rows);
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
