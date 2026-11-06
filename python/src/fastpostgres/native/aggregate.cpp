/* aggregate.cpp — reduce a columnar segment in C++, one call per batch.
 *
 * This is the consumption path for work that only wants numbers: no Python
 * object is built per row, and the loops run with the GIL released, so N threads
 * reducing N partitions (N rings) genuinely run in parallel on any interpreter —
 * the decode that materializes rows is what serializes, and this never does it.
 *
 * The unit is one batch: the caller drives the stream (dbapi.Cursor.aggregate)
 * and combines the per-batch scalars, which are a handful of Python objects per
 * batch, not per row. Semantics match the obvious SQL: NULLs are skipped by
 * SUM/MIN/MAX and not counted by COUNT; a batch with no rows gives 0 / NULL
 * identities. Integer sums accumulate in a signed 128-bit integer and are
 * returned as a Python int, so they never wrap — the server's sum(bigint) is
 * numeric and exact, and so is this. (The columnar frame itself widens numeric
 * to float8, so exact numeric arithmetic is the worker's job; with the
 * pushdown off, numeric is summed as a double.)
 *
 *     _client.reduce(seg, [(AGG_SUM, 1), (AGG_COUNT_ALL, -1)])
 *
 * That is the fallback and the reference. The default path is the worker's
 * FASTPOSTGRES_OP_REDUCE: it accumulates the same plan over the batch and
 * publishes an FPREDUCE frame, which _client.read_reduce(seg) turns into the
 * same handful of scalars. Both produce values the DB-API combines per batch,
 * so the semantics below are shared by construction.
 */
#include <Python.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "fastpostgres/fastpostgres_shm.h"
#include "codec.hpp"
#include "segment.hpp"

/* The plan's op codes (exported to Python as AGG_*). */
enum {
    FP_AGG_COUNT_ALL = 1, /* rows in the batch, NULLs included: no column needed */
    FP_AGG_COUNT = 2,     /* non-NULL values in the column */
    FP_AGG_SUM = 3,
    FP_AGG_MIN = 4,
    FP_AGG_MAX = 5,
};

typedef struct {
    int op;
    int column; /* -1 for COUNT_ALL */
} AggOp;

/* One accumulator per planned op, filled by the walks below. */
typedef struct {
    __int128           isum; /* SUM over the integer kinds: exact, 128 bits */
    double             fsum; /* SUM over the float kinds */
    long long          imin;
    long long          imax;
    double             fmin;
    double             fmax;
    const char        *best; /* MIN/MAX over text/bytea: points into the segment */
    int                best_len;
    unsigned long long count;
    int                seen; /* MIN/MAX: at least one non-NULL value */
} AggAcc;

static bool
kind_is_int(uint8_t kind)
{
    switch (kind) {
        case FASTPOSTGRES_KIND_BOOL:
        case FASTPOSTGRES_KIND_INT16:
        case FASTPOSTGRES_KIND_INT32:
        case FASTPOSTGRES_KIND_INT64:
            return true;
        default:
            return false;
    }
}

static bool
kind_is_float(uint8_t kind)
{
    return kind == FASTPOSTGRES_KIND_FLOAT4 || kind == FASTPOSTGRES_KIND_FLOAT8;
}

static bool
kind_is_textual(uint8_t kind)
{
    return kind == FASTPOSTGRES_KIND_TEXT || kind == FASTPOSTGRES_KIND_BYTEA;
}

static bool
kind_is_temporal(uint8_t kind)
{
    return kind == FASTPOSTGRES_KIND_DATE || kind == FASTPOSTGRES_KIND_TIME ||
           kind == FASTPOSTGRES_KIND_TIMESTAMP || kind == FASTPOSTGRES_KIND_TIMESTAMPTZ;
}

/* COUNT reads no value, so it is legal for every wire kind. SUM is numbers
 * only; MIN/MAX also orders text/bytea and the temporal integers. */
static bool
op_supported(int op, uint8_t kind)
{
    switch (op) {
        case FP_AGG_COUNT_ALL:
        case FP_AGG_COUNT:
            return true;
        case FP_AGG_SUM:
            return kind_is_int(kind) || kind_is_float(kind);
        case FP_AGG_MIN:
        case FP_AGG_MAX:
            return kind_is_int(kind) || kind_is_float(kind) || kind_is_textual(kind) || kind_is_temporal(kind);
        default:
            return false;
    }
}

/* One decoded Min/Max value, so the result loop can hand it to the same
 * fp_decode_reduce_value() the worker's frame uses. */
typedef struct {
    uint8_t        rtype;
    uint64_t       lo;
    uint64_t       hi;
    const char    *aux;
    uint32_t       aux_len;
} AggValue;

static void
value_from_kind(const AggAcc *a, uint8_t kind, int op, AggValue *v)
{
    memset(v, 0, sizeof(*v));
    if (op == FP_AGG_SUM) {
        if (kind_is_float(kind)) {
            v->rtype = FASTPOSTGRES_RVAL_DOUBLE;
            memcpy(&v->lo, &a->fsum, sizeof(a->fsum));
        } else {
            v->rtype = FASTPOSTGRES_RVAL_INT128;
            v->lo    = (uint64_t)a->isum;
            v->hi    = (uint64_t)(a->isum >> 64);
        }
        return;
    }
    if (op == FP_AGG_COUNT || op == FP_AGG_COUNT_ALL) {
        v->rtype = FASTPOSTGRES_RVAL_INT64;
        v->lo    = a->count;
        return;
    }
    if (!a->seen) {
        v->rtype = FASTPOSTGRES_RVAL_INT64;
        return;
    }
    switch (kind) {
        case FASTPOSTGRES_KIND_FLOAT4:
        case FASTPOSTGRES_KIND_FLOAT8: {
            double d = (op == FP_AGG_MIN) ? a->fmin : a->fmax;
            v->rtype = FASTPOSTGRES_RVAL_DOUBLE;
            memcpy(&v->lo, &d, sizeof(d));
            break;
        }
        case FASTPOSTGRES_KIND_TEXT:
            v->rtype   = FASTPOSTGRES_RVAL_TEXT;
            v->aux     = a->best;
            v->aux_len = (uint32_t)a->best_len;
            break;
        case FASTPOSTGRES_KIND_BYTEA:
            v->rtype   = FASTPOSTGRES_RVAL_BYTEA;
            v->aux     = a->best;
            v->aux_len = (uint32_t)a->best_len;
            break;
        case FASTPOSTGRES_KIND_DATE:
            v->rtype = FASTPOSTGRES_RVAL_DATE;
            v->lo    = (uint64_t)((op == FP_AGG_MIN) ? a->imin : a->imax);
            break;
        case FASTPOSTGRES_KIND_TIME:
            v->rtype = FASTPOSTGRES_RVAL_TIME;
            v->lo    = (uint64_t)((op == FP_AGG_MIN) ? a->imin : a->imax);
            break;
        case FASTPOSTGRES_KIND_TIMESTAMP:
            v->rtype = FASTPOSTGRES_RVAL_TIMESTAMP;
            v->lo    = (uint64_t)((op == FP_AGG_MIN) ? a->imin : a->imax);
            break;
        case FASTPOSTGRES_KIND_TIMESTAMPTZ:
            v->rtype = FASTPOSTGRES_RVAL_TIMESTAMPTZ;
            v->lo    = (uint64_t)((op == FP_AGG_MIN) ? a->imin : a->imax);
            break;
        default:
            v->rtype = FASTPOSTGRES_RVAL_INT64;
            v->lo    = (uint64_t)((op == FP_AGG_MIN) ? a->imin : a->imax);
            break;
    }
}

/* The integer kinds share one pass: T is the wire width, the accumulator is
 * 128-bit. The op switch sits inside the row loop — a branch per row against
 * the memory traffic this is bound by — but the kind switch is outside it, so
 * each instantiation walks its own tight loop. */
template <typename T>
static void
walk_int(const FastpostgresColumn *c, const AggOp *ops, AggAcc *accs, size_t nops, uint32_t column)
{
    const T       *data  = reinterpret_cast<const T *>(c->data);
    const uint8_t *nulls = c->nulls;

    for (uint64_t r = 0; r < c->nrows; r++) {
        if (nulls != nullptr && (nulls[r >> 3] & (1u << (r & 7))) != 0) continue;

        const long long v = (long long)data[r];

        for (size_t k = 0; k < nops; k++) {
            if (ops[k].column != (int)column) continue;

            AggAcc *a = &accs[k];

            switch (ops[k].op) {
                case FP_AGG_SUM:
                    a->isum += (__int128)v;
                    break;
                case FP_AGG_MIN:
                    if (!a->seen || v < a->imin) a->imin = v;
                    a->seen = 1;
                    break;
                case FP_AGG_MAX:
                    if (!a->seen || v > a->imax) a->imax = v;
                    a->seen = 1;
                    break;
                case FP_AGG_COUNT:
                    a->count++;
                    break;
                default:
                    break;
            }
        }
    }
}

template <typename T>
static void
walk_float(const FastpostgresColumn *c, const AggOp *ops, AggAcc *accs, size_t nops, uint32_t column)
{
    const T       *data  = reinterpret_cast<const T *>(c->data);
    const uint8_t *nulls = c->nulls;

    for (uint64_t r = 0; r < c->nrows; r++) {
        if (nulls != nullptr && (nulls[r >> 3] & (1u << (r & 7))) != 0) continue;

        const double v = (double)data[r];

        for (size_t k = 0; k < nops; k++) {
            if (ops[k].column != (int)column) continue;

            AggAcc *a = &accs[k];

            switch (ops[k].op) {
                case FP_AGG_SUM:
                    a->fsum += v;
                    break;
                case FP_AGG_MIN:
                    if (!a->seen || v < a->fmin) a->fmin = v;
                    a->seen = 1;
                    break;
                case FP_AGG_MAX:
                    if (!a->seen || v > a->fmax) a->fmax = v;
                    a->seen = 1;
                    break;
                case FP_AGG_COUNT:
                    a->count++;
                    break;
                default:
                    break;
            }
        }
    }
}

/* Text/bytea: the column is an int32 offset table plus a byte area. The
 * comparison is bytewise; under a non-C server collation the worker's
 * varstr_cmp is the exact path (the pushdown is the default), and this is the
 * fallback's documented approximation. */
static void
walk_text(const FastpostgresColumn *c, const AggOp *ops, AggAcc *accs, size_t nops, uint32_t column)
{
    const int32_t *off   = reinterpret_cast<const int32_t *>(c->data);
    const char    *bytes = reinterpret_cast<const char *>(c->bytes);
    const uint8_t *nulls = c->nulls;

    for (uint64_t r = 0; r < c->nrows; r++) {
        if (nulls != nullptr && (nulls[r >> 3] & (1u << (r & 7))) != 0) continue;

        const int32_t start = off[r];
        const int32_t end   = off[r + 1];
        const char   *data  = bytes + start;
        const int     len   = (int)(end - start);

        for (size_t k = 0; k < nops; k++) {
            if (ops[k].column != (int)column) continue;

            AggAcc *a = &accs[k];

            switch (ops[k].op) {
                case FP_AGG_MIN:
                case FP_AGG_MAX: {
                    int cmp = 0;

                    if (a->seen) {
                        cmp = memcmp(data, a->best, (size_t)(a->best_len < len ? a->best_len : len));
                        if (cmp == 0) cmp = len < a->best_len ? -1 : len > a->best_len ? 1 : 0;
                    }
                    if (!a->seen || (ops[k].op == FP_AGG_MIN ? cmp < 0 : cmp > 0)) {
                        a->best     = data;
                        a->best_len = len;
                    }
                    a->seen = 1;
                    break;
                }
                case FP_AGG_COUNT:
                    a->count++;
                    break;
                default:
                    break;
            }
        }
    }
}

/* The reduce itself, over an FPCOLUMN payload already mapped by the caller.
 * Shared by the streaming segment name and the bulk-loaded memfd descriptor.
 *
 * reduce(seg, [(op, column), ...]) -> [value, ...] in plan order.
 *
 * The payload is mapped under the GIL (the mapping cache and every error path
 * need it), then the whole walk runs without it: the loops read the mapped bytes
 * and nothing else. */
static PyObject *
reduce_payload(const uint8_t *payload, uint64_t payload_len, PyObject *plan)
{
    PyObject *seq = PySequence_Fast(plan, "reduce(seg, ops): ops must be a sequence of (op, column)");
    if (seq == nullptr) return nullptr;
    const Py_ssize_t nops = PySequence_Fast_GET_SIZE(seq);

    std::vector<AggOp>   ops((size_t)nops);
    std::vector<AggAcc>  accs((size_t)nops);
    std::vector<uint8_t> kinds((size_t)nops, 0);

    for (Py_ssize_t k = 0; k < nops; k++) {
        PyObject *pair = PySequence_Fast_GET_ITEM(seq, k);
        long      op, column;

        if (!PyArg_ParseTuple(pair, "ll", &op, &column)) {
            Py_DECREF(seq);
            return nullptr;
        }
        if (op < FP_AGG_COUNT_ALL || op > FP_AGG_MAX || column < -1) {
            Py_DECREF(seq);
            PyErr_SetString(PyExc_ValueError, "reduce: unknown op or column");
            return nullptr;
        }
        ops[(size_t)k]  = {(int)op, (int)column};
        accs[(size_t)k] = AggAcc{};
    }
    Py_DECREF(seq);

    FastpostgresReader rd;
    if (!fastpostgres_reader_init(&rd, payload, payload_len)) {
        PyErr_SetString(PyExc_ValueError, "not a fastpostgres columnar payload");
        return nullptr;
    }

    for (Py_ssize_t k = 0; k < nops; k++) {
        if (ops[(size_t)k].op == FP_AGG_COUNT_ALL) {
            accs[(size_t)k].count = rd.nrows;
            continue;
        }
        if (ops[(size_t)k].column < 0 || ops[(size_t)k].column >= (int)rd.ncols) {
            PyErr_SetString(PyExc_ValueError, "reduce: column out of range");
            return nullptr;
        }
    }

    bool unsupported = false;

    Py_BEGIN_ALLOW_THREADS;
    {
        for (uint32_t c = 0; c < rd.ncols; c++) {
            FastpostgresColumn col;

            if (!fastpostgres_reader_next(&rd, &col)) break;
            for (Py_ssize_t k = 0; k < nops; k++) {
                if (ops[(size_t)k].column != (int)c || ops[(size_t)k].op == FP_AGG_COUNT_ALL) continue;
                if (!op_supported(ops[(size_t)k].op, col.kind)) unsupported = true;
            }
            if (unsupported) break;

            for (Py_ssize_t k = 0; k < nops; k++)
                if (ops[(size_t)k].column == (int)c && ops[(size_t)k].op != FP_AGG_COUNT_ALL)
                    kinds[(size_t)k] = col.kind;

            if (col.kind == FASTPOSTGRES_KIND_TEXT || col.kind == FASTPOSTGRES_KIND_BYTEA) {
                walk_text(&col, ops.data(), accs.data(), (size_t)nops, c);
                continue;
            }

            switch (col.kind) {
                case FASTPOSTGRES_KIND_BOOL:
                    walk_int<uint8_t>(&col, ops.data(), accs.data(), (size_t)nops, c);
                    break;
                case FASTPOSTGRES_KIND_INT16:
                    walk_int<int16_t>(&col, ops.data(), accs.data(), (size_t)nops, c);
                    break;
                case FASTPOSTGRES_KIND_INT32:
                case FASTPOSTGRES_KIND_DATE:
                    walk_int<int32_t>(&col, ops.data(), accs.data(), (size_t)nops, c);
                    break;
                case FASTPOSTGRES_KIND_INT64:
                case FASTPOSTGRES_KIND_TIME:
                case FASTPOSTGRES_KIND_TIMESTAMP:
                case FASTPOSTGRES_KIND_TIMESTAMPTZ:
                    walk_int<int64_t>(&col, ops.data(), accs.data(), (size_t)nops, c);
                    break;
                case FASTPOSTGRES_KIND_FLOAT4:
                    walk_float<float>(&col, ops.data(), accs.data(), (size_t)nops, c);
                    break;
                case FASTPOSTGRES_KIND_FLOAT8:
                    walk_float<double>(&col, ops.data(), accs.data(), (size_t)nops, c);
                    break;
                default:
                    break;
            }
        }
    }
    Py_END_ALLOW_THREADS;

    if (unsupported) {
        PyErr_SetString(PyExc_ValueError, "reduce: this operation is not supported for this column type");
        return nullptr;
    }

    PyObject *out = PyList_New(nops);
    if (out == nullptr) return nullptr;

    for (Py_ssize_t k = 0; k < nops; k++) {
        const AggAcc *a = &accs[(size_t)k];
        AggValue      v;
        PyObject     *value;

        value_from_kind(a, kinds[(size_t)k], ops[(size_t)k].op, &v);
        value = fp_decode_reduce_value(v.rtype, v.lo, v.hi, v.aux, v.aux_len);
        if (value == nullptr) {
            Py_DECREF(out);
            return nullptr;
        }
        PyList_SET_ITEM(out, k, value);
    }
    return out;
}

/* reduce(seg, [(op, column), ...]) -> [value, ...] in plan order.
 *
 * The payload is mapped under the GIL (the mapping cache and every error path
 * need it), then the whole walk runs without it: the loops read the mapped bytes
 * and nothing else. */
static PyObject *
aggregate_reduce(PyObject *, PyObject *args)
{
    const char *name;
    PyObject   *plan;

    if (!PyArg_ParseTuple(args, "sO", &name, &plan)) return nullptr;

    uint64_t       payload_len = 0;
    const uint8_t *payload     = fp_segment_payload(name, &payload_len);
    if (payload == nullptr) return nullptr;

    return reduce_payload(payload, payload_len, plan);
}

/* columnar_reduce(pid, fd, size, [(op, column), ...]): the same reduce over a
 * bulk-loaded, immutable memfd. */
static PyObject *
columnar_reduce(PyObject *, PyObject *args)
{
    int                pid, fd;
    unsigned long long size;
    PyObject          *plan;

    if (!PyArg_ParseTuple(args, "iiKO", &pid, &fd, &size, &plan)) return nullptr;

    uint64_t       payload_len = 0;
    const uint8_t *payload     = fp_columnar_map(pid, fd, (uint64_t)size, &payload_len);
    if (payload == nullptr) return nullptr;

    return reduce_payload(payload, payload_len, plan);
}

/* columnar_reduce_handle(handle, size, plan): the same reduce over a descriptor
 * the client received with SCM_RIGHTS (segment.cpp owns the mapping). */
static PyObject *
columnar_reduce_handle(PyObject *, PyObject *args)
{
    int                handle;
    unsigned long long size;
    PyObject          *plan;

    if (!PyArg_ParseTuple(args, "iKO", &handle, &size, &plan)) return nullptr;

    uint64_t       payload_len = 0;
    const uint8_t *payload     = fp_columnar_map_handle(handle, (uint64_t)size, &payload_len);
    if (payload == nullptr) return nullptr;

    return reduce_payload(payload, payload_len, plan);
}

/* One FPREDUCE payload -> [value, ...] in plan order: the worker's frame, one
 * slot per planned op. The whole point of the frame is that it is tiny, so this
 * is a handful of Python objects per batch, not per row.
 *
 * Shared by read_reduce() (a segment the worker just published) and
 * read_reduce_bytes() (a frame the parallel aggregate's threads copied out).
 * The bytes are only read here; nothing keeps a reference to them. */
static PyObject *
read_reduce_payload(const uint8_t *payload, uint64_t payload_len)
{
    FastpostgresReduceReader rd;
    if (!fastpostgres_reduce_init(&rd, payload, payload_len)) {
        PyErr_SetString(PyExc_ValueError, "not a fastpostgres reduce payload");
        return nullptr;
    }

    PyObject *out = PyList_New((Py_ssize_t)rd.nops);
    if (out == nullptr) return nullptr;

    for (uint32_t k = 0; k < rd.nops; k++) {
        FastpostgresReduceSlot slot;
        const uint8_t         *aux = nullptr;
        uint32_t               aux_len = 0;
        PyObject              *v;

        if (!fastpostgres_reduce_next(&rd, &slot)) {
            Py_DECREF(out);
            PyErr_SetString(PyExc_ValueError, "truncated reduce payload");
            return nullptr;
        }
        if (slot.is_null) {
            v = Py_NewRef(Py_None);
        } else {
            aux = fastpostgres_reduce_slot_data(&rd, &slot, &aux_len);
            if (slot.aux_len != 0 && aux == nullptr) {
                Py_DECREF(out);
                PyErr_SetString(PyExc_ValueError, "truncated reduce payload");
                return nullptr;
            }
            v = fp_decode_reduce_value(slot.rtype, slot.lo, slot.hi, (const char *)aux, aux_len);
        }
        if (v == nullptr) {
            Py_DECREF(out);
            return nullptr;
        }
        PyList_SET_ITEM(out, (Py_ssize_t)k, v);
    }
    return out;
}

/* read_reduce(seg) -> [value, ...]: decode the FPREDUCE frame the worker just
 * published into that segment. */
static PyObject *
aggregate_read_reduce(PyObject *, PyObject *args)
{
    const char *name;

    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;

    uint64_t       payload_len = 0;
    const uint8_t *payload     = fp_segment_payload(name, &payload_len);
    if (payload == nullptr) return nullptr;

    return read_reduce_payload(payload, payload_len);
}

/* read_reduce_bytes(buf) -> [value, ...]: the same decode over a frame the
 * caller holds as bytes, never a segment. The parallel aggregate's threads copy
 * each batch's frame out of its ring under a length prefix, and this is how the
 * Python side turns those copies back into scalars. "y*" takes anything the
 * buffer protocol exposes contiguously — bytes, bytearray, memoryview. */
static PyObject *
aggregate_read_reduce_bytes(PyObject *, PyObject *args)
{
    Py_buffer view;

    if (!PyArg_ParseTuple(args, "y*", &view)) return nullptr;

    PyObject *out = read_reduce_payload((const uint8_t *)view.buf, (uint64_t)view.len);

    PyBuffer_Release(&view);
    return out;
}

static PyMethodDef aggregate_methods[] = {
    {"reduce", aggregate_reduce, METH_VARARGS,
     "reduce(seg, [(agg_op, column_index), ...]) -> list of scalars (no Python objects per row)"},
    {"read_reduce", aggregate_read_reduce, METH_VARARGS,
     "read_reduce(seg) -> list of scalars from the worker's FPREDUCE frame"},
    {"read_reduce_bytes", aggregate_read_reduce_bytes, METH_VARARGS,
     "read_reduce_bytes(buf) -> list of scalars from one FPREDUCE frame held as bytes/memoryview"},
    {"columnar_reduce", columnar_reduce, METH_VARARGS,
     "columnar_reduce(pid, fd, size, [(agg_op, column_index), ...]) -> list of scalars"},
    {"columnar_reduce_handle", columnar_reduce_handle, METH_VARARGS,
     "columnar_reduce_handle(handle, size, [(agg_op, column_index), ...]) -> list of scalars"},
    {nullptr},
};

bool
fp_register_aggregate(PyObject *module)
{
    for (PyMethodDef *m = aggregate_methods; m->ml_name != nullptr; m++) {
        PyObject *fn = PyCFunction_NewEx(m, nullptr, nullptr);
        if (fn == nullptr) return false;
        if (PyModule_AddObject(module, m->ml_name, fn) != 0) {
            Py_DECREF(fn);
            return false;
        }
    }

    struct {
        const char *name;
        int         value;
    } consts[] = {
        {"AGG_COUNT_ALL", FP_AGG_COUNT_ALL},
        {"AGG_COUNT", FP_AGG_COUNT},
        {"AGG_SUM", FP_AGG_SUM},
        {"AGG_MIN", FP_AGG_MIN},
        {"AGG_MAX", FP_AGG_MAX},
    };

    for (auto &c : consts)
        if (PyModule_AddIntConstant(module, c.name, c.value) != 0) return false;
    return true;
}
