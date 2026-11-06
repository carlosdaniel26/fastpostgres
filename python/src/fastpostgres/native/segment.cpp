/* segment.cpp — read a result segment (/dev/shm/<seg>) as either zero-copy
 * column views (read) or materialized typed row tuples (read_rows). The worker
 * answers a row request with the row frame (FPROWS) and falls back to the
 * columnar frame (FPCOLUMN) when an attribute is not one the client can walk in
 * place; read_rows takes either. A mapping is kept for the process lifetime:
 * the worker rewrites the same segment in place, so each read only revalidates
 * the header. */
#include "segment.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <x86intrin.h>

#include "codec.hpp"
#include "fastpostgres/fastpostgres_shm.h"
#include "shm_map.h"
#include "types.hpp"

typedef struct {
    std::string name;
    void       *base;
    size_t      size;
} ReadMap;

/* One mapping per segment name, for the process lifetime. Decoder threads share
 * this cache (the asyncio layer runs one per connection), so the lookup-and-
 * insert is locked: a reallocating push_back under a second reader is UB, even
 * with the GIL held, once the GIL stops being the lock that hides it. */
static std::mutex           g_read_maps_lock;
static std::vector<ReadMap> g_read_maps;

/* Bulk-loaded columnar tables: (pid, fd) names an immutable sealed memfd. The
 * mapping is kept for the process lifetime and never rewritten, so unlike the
 * streaming segment above there is no next-batch invalidation. */
typedef struct {
    int      pid;
    int      fd;
    uint64_t size; /* the size the caller asked for (0 = fstat) */
    void    *base;
    uint64_t len;
} ColumnarMap;

static std::mutex                 g_columnar_lock;
static std::vector<ColumnarMap>   g_columnar_maps;

static void *
read_map_get(const char *path, const std::string &name, size_t *size_out)
{
    std::lock_guard<std::mutex> held(g_read_maps_lock);

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
const uint8_t *
fp_segment_payload(const char *name, uint64_t *payload_len)
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

/* Turn one FPCOLUMN payload into the Segment of zero-copy column views. */
static PyObject *
segment_from_payload(const uint8_t *payload, uint64_t payload_len)
{
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

/* read(seg): the columnar frame as zero-copy column views. */
static PyObject *
segment_read(PyObject *, PyObject *args)
{
    const char *name;
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;

    uint64_t       payload_len = 0;
    const uint8_t *payload     = fp_segment_payload(name, &payload_len);
    if (payload == nullptr) return nullptr;

    return segment_from_payload(payload, payload_len);
}

/* Map a bulk-loaded columnar memfd, via the backend that loaded it. */
const uint8_t *
fp_columnar_map(int pid, int fd, uint64_t size, uint64_t *payload_len)
{
    std::lock_guard<std::mutex> held(g_columnar_lock);

    for (ColumnarMap &m : g_columnar_maps)
    {
        if (m.pid == pid && m.fd == fd && m.size == size)
        {
            *payload_len = m.len;
            return (const uint8_t *)m.base;
        }
    }

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/fd/%d", pid, fd);

    int h = open(path, O_RDONLY | O_CLOEXEC);
    if (h < 0)
    {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);
        return nullptr;
    }

    struct stat st;
    if (fstat(h, &st) != 0)
    {
        close(h);
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);
        return nullptr;
    }

    uint64_t len = size ? size : (uint64_t)st.st_size;
    if ((uint64_t)st.st_size < len)
    {
        close(h);
        PyErr_Format(PyExc_ValueError,
                     "columnar memfd is %lld bytes, but %llu were asked for",
                     (long long)st.st_size, (unsigned long long)len);
        return nullptr;
    }

    void *base = mmap(nullptr, (size_t)len, PROT_READ, MAP_SHARED, h, 0);
    close(h);
    if (base == MAP_FAILED)
    {
        PyErr_SetFromErrno(PyExc_OSError);
        return nullptr;
    }

    FastpostgresReader probe;
    if (!fastpostgres_reader_init(&probe, base, len))
    {
        munmap(base, (size_t)len);
        PyErr_SetString(PyExc_ValueError, "not a fastpostgres columnar payload");
        return nullptr;
    }

    g_columnar_maps.push_back({pid, fd, size, base, len});
    *payload_len = len;
    return (const uint8_t *)base;
}

static PyObject *
columnar_attach(PyObject *, PyObject *args)
{
    int                pid, fd;
    unsigned long long size;

    if (!PyArg_ParseTuple(args, "iiK", &pid, &fd, &size)) return nullptr;

    uint64_t       payload_len = 0;
    const uint8_t *payload     = fp_columnar_map(pid, fd, (uint64_t)size, &payload_len);
    if (payload == nullptr) return nullptr;

    return segment_from_payload(payload, payload_len);
}

/* FNV-1a over 8-byte words (and a byte tail), chosen only so the full-column
 * walk has a data dependency on every byte it reads: the point is to touch
 * every page, not to be cryptographic.  Hashing a word at a time keeps the
 * chain from hiding the memory traffic it is supposed to measure. */
static inline uint64_t
hash_bytes(const uint8_t *p, uint64_t n, uint64_t h)
{
    uint64_t i = 0;

    for (; i + 8 <= n; i += 8)
    {
        uint64_t w;

        memcpy(&w, p + i, 8);
        h ^= w;
        h *= 1099511628211ULL;
        h ^= h >> 29;
    }
    for (; i < n; i++)
    {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* columnar_scan(pid, fd, size) -> (rows, data_bytes, checksum, ncols)
 *
 * A real scan of the whole table: every fixed-width byte and every varlena
 * payload byte is read where it lies, with no per-row object and no copy.  The
 * checksum is a deterministic function of the payload, and `data_bytes` is the
 * column bytes touched (null slots for fixed kinds included, matching data_len),
 * so a caller can report bytes/s and page faults for the whole SELECT *. */
static PyObject *
columnar_scan(PyObject *, PyObject *args)
{
    int                pid, fd;
    unsigned long long size;

    if (!PyArg_ParseTuple(args, "iiK", &pid, &fd, &size)) return nullptr;

    uint64_t       payload_len = 0;
    const uint8_t *payload     = fp_columnar_map(pid, fd, (uint64_t)size, &payload_len);
    if (payload == nullptr) return nullptr;

    FastpostgresReader rd;
    if (!fastpostgres_reader_init(&rd, payload, payload_len))
    {
        PyErr_SetString(PyExc_ValueError, "not a fastpostgres columnar payload");
        return nullptr;
    }

    const uint64_t rows     = rd.nrows;
    const uint32_t ncols    = rd.ncols;
    uint64_t       bytes    = 0;
    uint64_t       checksum = 1469598103934665603ULL;

    Py_BEGIN_ALLOW_THREADS;
    for (uint32_t c = 0; c < ncols; c++)
    {
        FastpostgresColumn col;

        if (!fastpostgres_reader_next(&rd, &col)) break;

        if (fastpostgres_kind_is_varlena(col.kind))
        {
            const uint32_t *off   = (const uint32_t *)col.data;
            uint64_t        total = rows ? (uint64_t)off[rows] : 0;

            bytes += total;
            checksum = hash_bytes(col.bytes, total, checksum);
        }
        else
        {
            int      width = fastpostgres_kind_width(col.kind);
            uint64_t n     = width > 0 ? rows * (uint64_t)width : 0;

            bytes += n;
            checksum = hash_bytes(col.data, n, checksum);
        }
    }
    Py_END_ALLOW_THREADS;

    return Py_BuildValue("(KKKn)", (unsigned long long)rows, (unsigned long long)bytes,
                         (unsigned long long)checksum, (Py_ssize_t)ncols);
}


/* Materialize the chunk as typed row tuples (the DB-API fast path). */
static PyObject *
segment_read_rows(PyObject *, PyObject *args)
{
    const char *name;
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;

    uint64_t           payload_len = 0;
    unsigned long long t0          = __rdtsc();
    const uint8_t     *payload     = fp_segment_payload(name, &payload_len);
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
    {"columnar_attach", columnar_attach, METH_VARARGS,
     "columnar_attach(pid, fd, size) -> Segment (read-only memfd, zero-copy column views)"},
    {"columnar_scan", columnar_scan, METH_VARARGS,
     "columnar_scan(pid, fd, size) -> (rows, data_bytes, checksum, ncols) (in-place full walk)"},
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
