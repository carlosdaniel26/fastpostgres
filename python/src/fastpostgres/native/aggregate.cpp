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
 * identities. Integer sums accumulate in 64 bits and wrap on overflow — the
 * server's own sum(bigint) is numeric and exact, so compare large sums with
 * care (the wire types top out at int64 anyway).
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
    unsigned long long isum; /* SUM over the integer kinds: 64-bit, wrapping */
    double             fsum; /* SUM over the float kinds */
    long long          imin;
    long long          imax;
    double             fmin;
    double             fmax;
    unsigned long long count;
    int                seen; /* MIN/MAX: at least one non-NULL value */
} AggAcc;

/* The kinds this module walks: the fixed-width numbers and bool. Anything else
 * (text, bytea, dates, uuid, ...) is the caller's to handle. */
static bool
kind_supported(uint8_t kind)
{
    switch (kind)
    {
        case FASTPOSTGRES_KIND_BOOL:
        case FASTPOSTGRES_KIND_INT16:
        case FASTPOSTGRES_KIND_INT32:
        case FASTPOSTGRES_KIND_INT64:
        case FASTPOSTGRES_KIND_FLOAT4:
        case FASTPOSTGRES_KIND_FLOAT8:
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

/* The integer kinds share one pass: T is the wire width, the accumulator is
 * 64-bit. The op switch sits inside the row loop — a branch per row against the
 * memory traffic this is bound by — but the kind switch is outside it, so each
 * instantiation walks its own tight loop. */
template <typename T>
static void
walk_int(const FastpostgresColumn *c, const AggOp *ops, AggAcc *accs, size_t nops, uint32_t column)
{
    const T       *data  = reinterpret_cast<const T *>(c->data);
    const uint8_t *nulls = c->nulls;

    for (uint64_t r = 0; r < c->nrows; r++)
    {
        if (nulls != nullptr && (nulls[r >> 3] & (1u << (r & 7))) != 0) continue;

        const long long v = (long long)data[r];

        for (size_t k = 0; k < nops; k++)
        {
            if (ops[k].column != (int)column) continue;

            AggAcc *a = &accs[k];

            switch (ops[k].op)
            {
                case FP_AGG_SUM:
                    a->isum += (unsigned long long)v;
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

    for (uint64_t r = 0; r < c->nrows; r++)
    {
        if (nulls != nullptr && (nulls[r >> 3] & (1u << (r & 7))) != 0) continue;

        const double v = (double)data[r];

        for (size_t k = 0; k < nops; k++)
        {
            if (ops[k].column != (int)column) continue;

            AggAcc *a = &accs[k];

            switch (ops[k].op)
            {
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

    PyObject *seq = PySequence_Fast(plan, "reduce(seg, ops): ops must be a sequence of (op, column)");
    if (seq == nullptr) return nullptr;
    const Py_ssize_t nops = PySequence_Fast_GET_SIZE(seq);

    std::vector<AggOp>   ops((size_t)nops);
    std::vector<AggAcc>  accs((size_t)nops);
    std::vector<uint8_t> kinds((size_t)nops, 0);

    for (Py_ssize_t k = 0; k < nops; k++)
    {
        PyObject *pair = PySequence_Fast_GET_ITEM(seq, k);
        long      op, column;

        if (!PyArg_ParseTuple(pair, "ll", &op, &column))
        {
            Py_DECREF(seq);
            return nullptr;
        }
        if (op < FP_AGG_COUNT_ALL || op > FP_AGG_MAX || column < -1)
        {
            Py_DECREF(seq);
            PyErr_SetString(PyExc_ValueError, "reduce: unknown op or column");
            return nullptr;
        }
        ops[(size_t)k] = {(int)op, (int)column};
        accs[(size_t)k] = AggAcc{};
    }
    Py_DECREF(seq);

    uint64_t       payload_len = 0;
    const uint8_t *payload     = fp_segment_payload(name, &payload_len);
    if (payload == nullptr) return nullptr;

    FastpostgresReader rd;
    if (!fastpostgres_reader_init(&rd, payload, payload_len))
    {
        PyErr_SetString(PyExc_ValueError, "not a fastpostgres columnar payload");
        return nullptr;
    }

    for (Py_ssize_t k = 0; k < nops; k++)
    {
        if (ops[(size_t)k].op == FP_AGG_COUNT_ALL)
        {
            accs[(size_t)k].count = rd.nrows;
            continue;
        }
        if (ops[(size_t)k].column < 0 || ops[(size_t)k].column >= (int)rd.ncols)
        {
            PyErr_SetString(PyExc_ValueError, "reduce: column out of range");
            return nullptr;
        }
    }

    bool unsupported = false;

    Py_BEGIN_ALLOW_THREADS;
    {
        for (uint32_t c = 0; c < rd.ncols; c++)
        {
            FastpostgresColumn col;

            if (!fastpostgres_reader_next(&rd, &col)) break;
            if (!kind_supported(col.kind))
            {
                for (Py_ssize_t k = 0; k < nops; k++)
                    if (ops[(size_t)k].column == (int)c) unsupported = true;
                if (unsupported) break;
                continue;
            }
            for (Py_ssize_t k = 0; k < nops; k++)
                if (ops[(size_t)k].column == (int)c && ops[(size_t)k].op != FP_AGG_COUNT_ALL)
                    kinds[(size_t)k] = col.kind;

            switch (col.kind)
            {
                case FASTPOSTGRES_KIND_BOOL:
                    walk_int<uint8_t>(&col, ops.data(), accs.data(), (size_t)nops, c);
                    break;
                case FASTPOSTGRES_KIND_INT16:
                    walk_int<int16_t>(&col, ops.data(), accs.data(), (size_t)nops, c);
                    break;
                case FASTPOSTGRES_KIND_INT32:
                    walk_int<int32_t>(&col, ops.data(), accs.data(), (size_t)nops, c);
                    break;
                case FASTPOSTGRES_KIND_INT64:
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

    if (unsupported)
    {
        PyErr_SetString(PyExc_ValueError,
                        "reduce: this kind is not one the C++ path walks (numbers and bool only)");
        return nullptr;
    }

    PyObject *out = PyList_New(nops);
    if (out == nullptr) return nullptr;

    for (Py_ssize_t k = 0; k < nops; k++)
    {
        const AggAcc *a = &accs[(size_t)k];
        PyObject     *v = nullptr;

        switch (ops[(size_t)k].op)
        {
            case FP_AGG_COUNT_ALL:
            case FP_AGG_COUNT:
                v = PyLong_FromUnsignedLongLong(a->count);
                break;
            case FP_AGG_SUM:
                v = kind_is_float(kinds[(size_t)k]) ? PyFloat_FromDouble(a->fsum)
                                                    : PyLong_FromLongLong((long long)a->isum);
                break;
            case FP_AGG_MIN:
                if (!a->seen) v = Py_NewRef(Py_None);
                else if (kind_is_float(kinds[(size_t)k])) v = PyFloat_FromDouble(a->fmin);
                else v = PyLong_FromLongLong(a->imin);
                break;
            case FP_AGG_MAX:
                if (!a->seen) v = Py_NewRef(Py_None);
                else if (kind_is_float(kinds[(size_t)k])) v = PyFloat_FromDouble(a->fmax);
                else v = PyLong_FromLongLong(a->imax);
                break;
            default:
                break;
        }
        if (v == nullptr)
        {
            Py_DECREF(out);
            return nullptr;
        }
        PyList_SET_ITEM(out, k, v);
    }
    return out;
}

/* read_reduce(seg) -> [value, ...] in plan order: the worker's FPREDUCE frame,
 * one slot per planned op. The whole point of the frame is that it is tiny, so
 * this is a handful of Python objects per batch, not per row. */
static PyObject *
aggregate_read_reduce(PyObject *, PyObject *args)
{
    const char *name;

    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;

    uint64_t       payload_len = 0;
    const uint8_t *payload     = fp_segment_payload(name, &payload_len);
    if (payload == nullptr) return nullptr;

    FastpostgresReduceReader rd;
    if (!fastpostgres_reduce_init(&rd, payload, payload_len))
    {
        PyErr_SetString(PyExc_ValueError, "not a fastpostgres reduce payload");
        return nullptr;
    }

    PyObject *out = PyList_New((Py_ssize_t)rd.nops);
    if (out == nullptr) return nullptr;

    for (uint32_t k = 0; k < rd.nops; k++)
    {
        FastpostgresReduceSlot slot;
        PyObject             *v;

        if (!fastpostgres_reduce_next(&rd, &slot))
        {
            Py_DECREF(out);
            PyErr_SetString(PyExc_ValueError, "truncated reduce payload");
            return nullptr;
        }
        if (slot.is_null)
            v = Py_NewRef(Py_None);
        else if (slot.is_float)
        {
            double d;

            memcpy(&d, &slot.bits, sizeof(d));
            v = PyFloat_FromDouble(d);
        }
        else
            v = PyLong_FromLongLong((long long)slot.bits);
        if (v == nullptr)
        {
            Py_DECREF(out);
            return nullptr;
        }
        PyList_SET_ITEM(out, (Py_ssize_t)k, v);
    }
    return out;
}

static PyMethodDef aggregate_methods[] = {
    {"reduce", aggregate_reduce, METH_VARARGS,
     "reduce(seg, [(agg_op, column_index), ...]) -> list of scalars (no Python objects per row)"},
    {"read_reduce", aggregate_read_reduce, METH_VARARGS,
     "read_reduce(seg) -> list of scalars from the worker's FPREDUCE frame"},
    {nullptr},
};

bool
fp_register_aggregate(PyObject *module)
{
    for (PyMethodDef *m = aggregate_methods; m->ml_name != nullptr; m++)
    {
        PyObject *fn = PyCFunction_NewEx(m, nullptr, nullptr);
        if (fn == nullptr) return false;
        if (PyModule_AddObject(module, m->ml_name, fn) != 0)
        {
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
