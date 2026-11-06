/* reduce.c — accumulate SUM/COUNT/MIN/MAX over a batch inside the worker.
 *
 * The columnar path ships every row to the client and lets its C++ reduce walk
 * the mapping. That is the right trade for a client that wants the bytes, and
 * the wrong one for a query that wants two numbers: the reduce can be done
 * where the tuples already are. This file does that and publishes FPREDUCE
 * (extension/include/fastpostgres/fastpostgres_shm.h) instead of a batch.
 *
 * The loop is deliberately the same shape as the client's
 * (python/src/fastpostgres/native/aggregate.cpp): one accumulator per planned
 * op, NULLs skipped by SUM/MIN/MAX and not counted by COUNT, integer sums in
 * 64 bits that wrap. Keeping the semantics identical is what lets the DB-API
 * combine per-batch scalars from either side without knowing which ran.
 *
 * Plan parsing is here rather than in the worker because the column names can
 * only be resolved against the cursor's TupleDesc, which arrives with the
 * batch. A malformed plan or an unsupported column rejects the request with a
 * "reduce: " message, never a partial answer.
 */
#include "postgres.h"

#include <string.h>
#include <sys/mman.h>

#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"

#include "fastpostgres/fastpostgres_shm.h"

#include "columnar.h"
#include "reduce.h"
#include "shm.h"

/* The plan's op codes; the client exports the same numbers as AGG_*. */
enum
{
    FP_AGG_COUNT_ALL = 1,
    FP_AGG_COUNT     = 2,
    FP_AGG_SUM       = 3,
    FP_AGG_MIN       = 4,
    FP_AGG_MAX       = 5
};

typedef struct ReduceAcc
{
    int                op;
    int                column; /* -1 for COUNT_ALL */
    bool               by_name; /* column is a name at name_off, not an index yet */
    uint32_t           name_off;
    uint8_t            kind;
    uint32_t           pgtype;
    unsigned long long isum;
    double             fsum;
    long long          imin;
    long long          imax;
    double             fmin;
    double             fmax;
    unsigned long long count;
    int                seen; /* MIN/MAX: at least one non-NULL value */
} ReduceAcc;

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

/* Resolve a plan column against the batch's descriptor. */
static int
resolve_column(TupleDesc td, bool by_index, int index, const char *name)
{
    int i;

    if (by_index)
    {
        if (index < 0) index += td->natts;
        if (index < 0 || index >= td->natts) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                                             errmsg("reduce: column index out of range")));
        return index;
    }
    for (i = 0; i < td->natts; i++)
    {
        Form_pg_attribute att = TupleDescAttr(td, i);

        if (att->attisdropped) continue;
        if (strcmp(NameStr(att->attname), name) == 0) return i;
    }
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("reduce: no column \"%s\" in the result", name)));
    return -1;
}

/* Split the plan on unescaped commas and unescape each entry in place. `copy`
 * is palloc'd in the caller's context and lives until SPI_finish. */
static int
parse_plan(char *copy, ReduceAcc *accs)
{
    char *p   = copy;
    int   n   = 0;

    while (*p)
    {
        char *end = p;
        char *w;
        char *colon;
        bool  esc  = false;
        bool  last;
        int   op;

        while (*end)
        {
            if (esc)
                esc = false;
            else if (*end == '\\')
                esc = true;
            else if (*end == ',')
                break;
            end++;
        }
        last = (*end == 0);
        if (!last) *end = 0;

        w = p;
        for (char *q = p; *q; q++)
        {
            if (*q == '\\' && q[1] != 0) q++;
            *w++ = *q;
        }
        *w = 0;

        if (n >= FASTPOSTGRES_REDUCE_MAX_OPS)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("reduce: too many operations (max %d)", FASTPOSTGRES_REDUCE_MAX_OPS)));

        colon = strchr(p, ':');
        if (colon != NULL) *colon++ = 0;

        op = atoi(p);
        if (op < FP_AGG_COUNT_ALL || op > FP_AGG_MAX || (op != FP_AGG_COUNT_ALL && (colon == NULL || colon[0] == 0)))
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("reduce: malformed plan")));

        accs[n].op = op;
        if (op == FP_AGG_COUNT_ALL)
        {
            if (colon != NULL && colon[0] != 0)
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("reduce: count_all takes no column")));
            accs[n].column = -1;
        }
        else if (colon[0] == 'i')
        {
            char *after = NULL;
            long  idx   = strtol(colon + 1, &after, 10);

            if (after == colon + 1 || *after != 0)
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("reduce: malformed column index")));
            accs[n].column = (int)idx;
        }
        else if (colon[0] == 'n')
        {
            accs[n].by_name  = true;
            accs[n].name_off = (uint32_t)(colon + 1 - copy);
        }
        else
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("reduce: malformed column")));

        n++;
        if (last) break;
        p = end + 1;
    }
    return n;
}

static long long
int_value(Datum d, uint8_t kind)
{
    switch (kind)
    {
        case FASTPOSTGRES_KIND_BOOL:
            return DatumGetBool(d) ? 1 : 0;
        case FASTPOSTGRES_KIND_INT16:
            return (long long)DatumGetInt16(d);
        case FASTPOSTGRES_KIND_INT32:
            return (long long)DatumGetInt32(d);
        case FASTPOSTGRES_KIND_INT64:
            return (long long)DatumGetInt64(d);
        default:
            return 0;
    }
}

static double
float_value(Datum d, uint8_t kind, uint32_t pgtype)
{
    if (pgtype == FASTPOSTGRES_OID_NUMERIC)
        return DatumGetFloat8(DirectFunctionCall1(numeric_float8, d));
    if (kind == FASTPOSTGRES_KIND_FLOAT4) return (double)DatumGetFloat4(d);
    return DatumGetFloat8(d);
}

/* Write the scalar frame into the segment and publish it. */
static uint64
publish(const char *path, const ReduceAcc *accs, int nops, uint64_t nrows, size_t capacity)
{
    size_t  cap    = capacity ? capacity : FASTPOSTGRES_SEGMENT_BYTES;
    size_t  mapped = 0;
    void   *map    = fastpostgres_shm_map_rw(path, cap, &mapped);
    char   *out;
    char   *p;
    uint64_t total;
    int      i;

    if (map == NULL) elog(ERROR, "fastpostgres: cannot map reduce segment");
    if (mapped < FASTPOSTGRES_SHM_HEADER_SIZE + FASTPOSTGRES_REDUCE_FRAME +
                     (uint64_t)nops * FASTPOSTGRES_REDUCE_SLOT)
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("fastpostgres: reduce frame exceeds the segment")));

    out = (char *)map + FASTPOSTGRES_SHM_HEADER_SIZE;
    p   = out;

    memcpy(p, FASTPOSTGRES_REDUCE_MAGIC, 8);
    p += 8;
    fastpostgres_put_u32(p, FASTPOSTGRES_REDUCE_VERSION);
    p += 4;
    fastpostgres_put_u32(p, 1); /* flags: little-endian */
    p += 4;
    fastpostgres_put_u32(p, (uint32_t)nops);
    p += 4;
    fastpostgres_put_u32(p, 0);
    p += 4;
    fastpostgres_put_u64(p, nrows);
    p += 8;

    for (i = 0; i < nops; i++)
    {
        const ReduceAcc *a       = &accs[i];
        bool             is_float = kind_is_float(a->kind);
        uint64_t         bits     = 0;

        *p++ = 0; /* is_null */
        *p++ = 0; /* is_float */
        *p++ = 0;
        *p++ = 0;

        switch (a->op)
        {
            case FP_AGG_COUNT_ALL:
                fastpostgres_put_u64(p, nrows);
                break;
            case FP_AGG_COUNT:
                fastpostgres_put_u64(p, a->count);
                break;
            case FP_AGG_SUM:
                if (is_float)
                {
                    double v = a->fsum;

                    memcpy(&bits, &v, 8);
                }
                else
                    bits = (uint64_t)a->isum;
                fastpostgres_put_u64(p, bits);
                break;
            case FP_AGG_MIN:
            case FP_AGG_MAX:
                if (!a->seen)
                {
                    *(p - 4) = 1; /* is_null: an empty MIN/MAX is None */
                    fastpostgres_put_u64(p, 0);
                }
                else if (is_float)
                {
                    double v = (a->op == FP_AGG_MIN) ? a->fmin : a->fmax;

                    memcpy(&bits, &v, 8);
                    fastpostgres_put_u64(p, bits);
                }
                else
                    fastpostgres_put_u64(p, (uint64_t)((a->op == FP_AGG_MIN) ? a->imin : a->imax));
                break;
            default:
                fastpostgres_put_u64(p, 0);
                break;
        }
        /* is_float is meaningful for the value-bearing ops only; the client's
         * combine treats SUM/COUNT as integers regardless. */
        if (is_float && a->op != FP_AGG_COUNT && a->op != FP_AGG_COUNT_ALL) *(p - 3) = 1;
        p += 8;
    }

    total = (uint64_t)(p - out);
    fastpostgres_shm_publish(map, mapped, total);
    /* the mapping is cached for the worker's lifetime: no per-batch munmap */
    return total;
}

uint64
fastpostgres_reduce_publish(const char *path, TupleDesc td, HeapTuple *tuples, uint64 nrows, const char *plan,
                            size_t capacity)
{
    char     *copy = pstrdup(plan);
    ReduceAcc accs[FASTPOSTGRES_REDUCE_MAX_OPS];
    int       nops;
    int       k;

    memset(accs, 0, sizeof(accs));
    nops = parse_plan(copy, accs);

    for (k = 0; k < nops; k++)
    {
        ReduceAcc *a = &accs[k];

        if (a->op == FP_AGG_COUNT_ALL)
        {
            a->column = -1;
            a->kind   = FASTPOSTGRES_KIND_INT64;
            continue;
        }
        if (a->by_name)
        {
            const char *name = copy + a->name_off;

            a->column = resolve_column(td, false, 0, name);
        }
        else
            a->column = resolve_column(td, true, a->column, NULL);

        a->kind   = fastpostgres_oid_to_kind((uint32_t)TupleDescAttr(td, a->column)->atttypid);
        a->pgtype = (uint32_t)TupleDescAttr(td, a->column)->atttypid;
        if (!kind_supported(a->kind))
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("reduce: this kind is not one the C path walks (numbers and bool only)")));
    }

    /* Only the planned columns are read: a sum over one column of a wide result
     * never deforms the rest. */
    for (k = 0; k < nops; k++)
    {
        ReduceAcc *a = &accs[k];

        if (a->op == FP_AGG_COUNT_ALL || a->column < 0) continue;

        for (uint64 r = 0; r < nrows; r++)
        {
            bool  isnull = false;
            Datum d      = heap_getattr(tuples[r], a->column + 1, td, &isnull);

            if (isnull) continue;

            if (kind_is_float(a->kind))
            {
                double v = float_value(d, a->kind, a->pgtype);

                switch (a->op)
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
            else
            {
                long long v = int_value(d, a->kind);

                switch (a->op)
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

    return publish(path, accs, nops, (uint64_t)nrows, capacity);
}
