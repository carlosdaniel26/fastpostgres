/* reduce.c — accumulate SUM/COUNT/MIN/MAX over a batch inside the worker.
 *
 * The columnar path ships every row to the client and lets its C++ reduce walk
 * the mapping. That is the right trade for a client that wants the bytes, and
 * the wrong one for a query that wants two numbers: the reduce can be done
 * where the tuples already are. This file does that and publishes FPREDUCE
 * (extension/include/fastpostgres/fastpostgres_shm.h) instead of a batch.
 *
 * Semantics are the server's, not a convenient approximation:
 *
 *   - SUM over integers accumulates in a signed 128-bit integer and is
 *     published as such. The server's sum(bigint) returns numeric, so a sum
 *     that leaves int64 must not wrap; 128 bits covers every batch a ring can
 *     fetch, and the client adds the per-batch int128s in arbitrary precision.
 *   - SUM/MIN/MAX over numeric use PostgreSQL's own numeric arithmetic, so the
 *     result is exact (and min/max is numeric order, not a float comparison).
 *   - MIN/MAX over text use varstr_cmp with the column's collation, exactly as
 *     the server's aggregate does — not memcmp over UTF-8.
 *   - MIN/MAX over date/time publish the wire integer and let the client build
 *     the same Python object the columnar decoder would.
 *
 * NULLs are skipped by SUM/MIN/MAX and not counted by COUNT. COUNT does not
 * read the value at all, so it works for every kind, including text. SUM over
 * text/date/time is refused rather than guessed.
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
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/fmgrprotos.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"
#include "varatt.h"

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
    int      op;
    int      column;   /* -1 for COUNT_ALL */
    bool     by_name;  /* column is a name at name_off, not an index yet */
    uint32_t name_off;
    uint8_t  kind;
    bool     is_numeric;
    bool     is_varlena; /* the Datum is text/varchar/bpchar/bytea, not an output-function fallback */
    Oid      collation;  /* text/bytea comparison */

    unsigned __int128 isum; /* SUM over the integer kinds: exact, 128 bits */
    double            fsum; /* SUM over the float kinds */
    Datum             nsum; /* SUM over numeric */
    Datum             nbest; /* MIN/MAX over numeric */
    long long         imin;
    long long         imax;
    double            fmin;
    double            fmax;
    const char       *best; /* MIN/MAX over text/bytea */
    int               best_len;
    unsigned long long count;
    bool               seen; /* at least one non-NULL value */
} ReduceAcc;

static bool
kind_is_int(uint8_t kind)
{
    switch (kind)
    {
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
kind_is_temporal(uint8_t kind)
{
    return kind == FASTPOSTGRES_KIND_DATE || kind == FASTPOSTGRES_KIND_TIME ||
           kind == FASTPOSTGRES_KIND_TIMESTAMP || kind == FASTPOSTGRES_KIND_TIMESTAMPTZ;
}

/* What the server can do with this kind, op by op. COUNT reads no value, so it
 * is legal for every kind; SUM is numbers only, including exact numeric. */
static bool
op_supported(int op, uint8_t kind, bool is_numeric, bool is_varlena)
{
    switch (op)
    {
        case FP_AGG_COUNT_ALL:
        case FP_AGG_COUNT:
            return true;
        case FP_AGG_SUM:
            return kind_is_int(kind) || kind_is_float(kind) || is_numeric;
        case FP_AGG_MIN:
        case FP_AGG_MAX:
            return kind_is_int(kind) || kind_is_float(kind) || is_numeric || is_varlena ||
                   kind_is_temporal(kind);
        default:
            return false;
    }
}

static const char *
op_name(int op)
{
    switch (op)
    {
        case FP_AGG_SUM: return "sum";
        case FP_AGG_MIN: return "min";
        case FP_AGG_MAX: return "max";
        default: return "aggregate";
    }
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
    char *p = copy;
    int   n = 0;

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
        case FASTPOSTGRES_KIND_DATE:
            return (long long)DatumGetDateADT(d);
        case FASTPOSTGRES_KIND_TIME:
            return (long long)DatumGetTimeADT(d);
        case FASTPOSTGRES_KIND_TIMESTAMP:
            return (long long)DatumGetTimestamp(d);
        case FASTPOSTGRES_KIND_TIMESTAMPTZ:
            return (long long)DatumGetTimestampTz(d);
        default:
            return 0;
    }
}

static double
float_value(Datum d, uint8_t kind)
{
    if (kind == FASTPOSTGRES_KIND_FLOAT4) return (double)DatumGetFloat4(d);
    return DatumGetFloat8(d);
}

/* Text/bytea bytes, detoasted. The pointer lives in the caller's memory
 * context for the rest of this batch. */
static void
varlena_value(Datum d, const char **data, int *len)
{
    struct varlena *v = pg_detoast_datum_packed((struct varlena *)DatumGetPointer(d));

    *data = VARDATA_ANY(v);
    *len  = (int)VARSIZE_ANY_EXHDR(v);
}

/* MIN/MAX ordering for the textual kinds. text uses the column's collation,
 * exactly like the server's aggregate; bytea uses memcmp, which is also what
 * its btree opclass does. Returns <0, 0, >0 for a against b. */
static int
textual_cmp(const char *a, int alen, const char *b, int blen, uint8_t kind, Oid collation)
{
    if (kind == FASTPOSTGRES_KIND_BYTEA)
    {
        int cmp = memcmp(a, b, Min(alen, blen));

        if (cmp != 0) return cmp;
        return alen < blen ? -1 : alen > blen ? 1 : 0;
    }
    return varstr_cmp(a, alen, b, blen, collation);
}

/* Write the scalar frame into the segment and publish it. */
static uint64
publish(const char *path, ReduceAcc *accs, int nops, uint64_t nrows, size_t capacity)
{
    size_t   cap    = capacity ? capacity : FASTPOSTGRES_SEGMENT_BYTES;
    size_t   mapped = 0;
    void    *map    = fastpostgres_shm_map_rw(path, cap, &mapped);
    char    *out;
    char    *p;
    uint64_t base_bytes;
    uint64_t aux_total = 0;
    uint64_t aux_off;
    uint64_t total;
    const char *aux_src[FASTPOSTGRES_REDUCE_MAX_OPS];
    uint32_t    aux_len[FASTPOSTGRES_REDUCE_MAX_OPS];
    int         i;

    if (map == NULL) elog(ERROR, "fastpostgres: cannot map reduce segment");

    /* First pass: the aux pool's size (numeric output and text values). */
    memset(aux_src, 0, sizeof(aux_src));
    memset(aux_len, 0, sizeof(aux_len));
    for (i = 0; i < nops; i++)
    {
        ReduceAcc *a = &accs[i];

        if (a->op == FP_AGG_SUM && a->is_numeric)
        {
            Datum n = a->seen ? a->nsum : DirectFunctionCall1(int8_numeric, Int64GetDatum(0));

            aux_src[i] = DatumGetCString(DirectFunctionCall1(numeric_out, n));
            aux_len[i] = (uint32_t)strlen(aux_src[i]);
        }
        else if ((a->op == FP_AGG_MIN || a->op == FP_AGG_MAX) && a->seen)
        {
            if (a->is_numeric)
            {
                aux_src[i] = DatumGetCString(DirectFunctionCall1(numeric_out, a->nbest));
                aux_len[i] = (uint32_t)strlen(aux_src[i]);
            }
            else if (a->is_varlena)
            {
                aux_src[i] = a->best;
                aux_len[i] = (uint32_t)a->best_len;
            }
        }
        aux_total += aux_len[i];
    }

    base_bytes = FASTPOSTGRES_REDUCE_FRAME + (uint64_t)nops * FASTPOSTGRES_REDUCE_SLOT;
    if (mapped < FASTPOSTGRES_SHM_HEADER_SIZE + base_bytes + aux_total)
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                        errmsg("reduce: result does not fit the segment (%llu bytes needed)",
                               (unsigned long long)(FASTPOSTGRES_SHM_HEADER_SIZE + base_bytes + aux_total))));

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

    aux_off = base_bytes;
    for (i = 0; i < nops; i++)
    {
        ReduceAcc *a      = &accs[i];
        uint8_t    rtype  = FASTPOSTGRES_RVAL_INT64;
        bool       is_null = false;
        uint64_t   lo     = 0;
        uint64_t   hi     = 0;
        uint32_t   this_off = 0;
        uint32_t   this_len = 0;

        switch (a->op)
        {
            case FP_AGG_COUNT_ALL:
                lo    = nrows;
                rtype = FASTPOSTGRES_RVAL_INT64;
                break;
            case FP_AGG_COUNT:
                lo    = a->count;
                rtype = FASTPOSTGRES_RVAL_INT64;
                break;
            case FP_AGG_SUM:
                if (a->is_numeric)
                {
                    rtype    = FASTPOSTGRES_RVAL_NUMERIC;
                    this_off = (uint32_t)aux_off;
                    this_len = aux_len[i];
                }
                else if (kind_is_float(a->kind))
                {
                    double v = a->fsum;

                    memcpy(&lo, &v, 8);
                    rtype = FASTPOSTGRES_RVAL_DOUBLE;
                }
                else
                {
                    lo    = (uint64_t)a->isum;
                    hi    = (uint64_t)(a->isum >> 64);
                    rtype = FASTPOSTGRES_RVAL_INT128;
                }
                break;
            case FP_AGG_MIN:
            case FP_AGG_MAX:
                if (!a->seen)
                {
                    is_null = true;
                    rtype   = FASTPOSTGRES_RVAL_INT64;
                }
                else if (a->is_numeric)
                {
                    rtype    = FASTPOSTGRES_RVAL_NUMERIC;
                    this_off = (uint32_t)aux_off;
                    this_len = aux_len[i];
                }
                else if (a->kind == FASTPOSTGRES_KIND_TEXT || a->kind == FASTPOSTGRES_KIND_BYTEA)
                {
                    rtype    = a->kind == FASTPOSTGRES_KIND_BYTEA ? FASTPOSTGRES_RVAL_BYTEA
                                                                  : FASTPOSTGRES_RVAL_TEXT;
                    this_off = (uint32_t)aux_off;
                    this_len = aux_len[i];
                }
                else if (a->kind == FASTPOSTGRES_KIND_DATE)
                {
                    lo    = (uint64_t)(a->op == FP_AGG_MIN ? a->imin : a->imax);
                    rtype = FASTPOSTGRES_RVAL_DATE;
                }
                else if (a->kind == FASTPOSTGRES_KIND_TIME)
                {
                    lo    = (uint64_t)(a->op == FP_AGG_MIN ? a->imin : a->imax);
                    rtype = FASTPOSTGRES_RVAL_TIME;
                }
                else if (a->kind == FASTPOSTGRES_KIND_TIMESTAMP)
                {
                    lo    = (uint64_t)(a->op == FP_AGG_MIN ? a->imin : a->imax);
                    rtype = FASTPOSTGRES_RVAL_TIMESTAMP;
                }
                else if (a->kind == FASTPOSTGRES_KIND_TIMESTAMPTZ)
                {
                    lo    = (uint64_t)(a->op == FP_AGG_MIN ? a->imin : a->imax);
                    rtype = FASTPOSTGRES_RVAL_TIMESTAMPTZ;
                }
                else if (kind_is_float(a->kind))
                {
                    double v = (a->op == FP_AGG_MIN) ? a->fmin : a->fmax;

                    memcpy(&lo, &v, 8);
                    rtype = FASTPOSTGRES_RVAL_DOUBLE;
                }
                else
                {
                    lo    = (uint64_t)((a->op == FP_AGG_MIN) ? a->imin : a->imax);
                    rtype = FASTPOSTGRES_RVAL_INT64;
                }
                break;
            default:
                break;
        }

        *p++ = is_null ? 1 : 0;
        *p++ = rtype;
        *p++ = 0;
        *p++ = 0;
        fastpostgres_put_u32(p, this_off);
        p += 4;
        fastpostgres_put_u32(p, this_len);
        p += 4;
        fastpostgres_put_u32(p, 0); /* pad: align lo/hi */
        p += 4;
        fastpostgres_put_u64(p, lo);
        p += 8;
        fastpostgres_put_u64(p, hi);
        p += 8;

        if (this_len != 0)
        {
            memcpy(out + aux_off, aux_src[i], this_len);
            aux_off += this_len;
        }
    }

    total = (uint64_t)(p - out) + aux_total;
    fastpostgres_shm_publish(map, mapped, total);
    /* the mapping is cached for the worker's lifetime: no per-batch munmap */
    return total;
}

uint64
fastpostgres_reduce_publish(const char *path, TupleDesc td, HeapTuple *tuples, uint64 nrows, const char *plan,
                            size_t capacity)
{
    char      *copy = pstrdup(plan);
    ReduceAcc  accs[FASTPOSTGRES_REDUCE_MAX_OPS];
    int        nops;
    int        k;

    memset(accs, 0, sizeof(accs));
    nops = parse_plan(copy, accs);

    for (k = 0; k < nops; k++)
    {
        ReduceAcc        *a = &accs[k];
        Form_pg_attribute att;

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

        att     = TupleDescAttr(td, a->column);
        a->kind = fastpostgres_oid_to_kind((uint32_t)att->atttypid);
        /* numeric has no wire kind (the columnar path widens it to float8), but
         * this path holds the raw Datum, so it can stay exact. */
        a->is_numeric = att->atttypid == FASTPOSTGRES_OID_NUMERIC;
        a->is_varlena = att->atttypid == FASTPOSTGRES_OID_TEXT || att->atttypid == FASTPOSTGRES_OID_VARCHAR ||
                        att->atttypid == FASTPOSTGRES_OID_BPCHAR || att->atttypid == FASTPOSTGRES_OID_BYTEA;
        a->collation  = OidIsValid(att->attcollation) ? att->attcollation : DEFAULT_COLLATION_OID;

        if (!op_supported(a->op, a->kind, a->is_numeric, a->is_varlena))
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("reduce: %s is not supported for this column type", op_name(a->op))));
    }

    /* Only the planned columns are read: a sum over one column of a wide result
     * never deforms the rest. */
    for (k = 0; k < nops; k++)
    {
        ReduceAcc *a = &accs[k];

        if (a->op == FP_AGG_COUNT_ALL || a->column < 0 || a->op == FP_AGG_COUNT)
        {
            if (a->op == FP_AGG_COUNT)
                for (uint64 r = 0; r < nrows; r++)
                {
                    bool isnull = false;

                    (void)heap_getattr(tuples[r], a->column + 1, td, &isnull);
                    if (!isnull) a->count++;
                }
            continue;
        }

        for (uint64 r = 0; r < nrows; r++)
        {
            bool  isnull = false;
            Datum d      = heap_getattr(tuples[r], a->column + 1, td, &isnull);
            bool  better;

            if (isnull) continue;

            if (a->op == FP_AGG_SUM)
            {
                if (a->is_numeric)
                {
                    /* Keep the numeric exactly as the server does: numeric_add,
                     * not a cast through double. */
                    a->nsum = a->seen ? DirectFunctionCall2(numeric_add, a->nsum, d) : d;
                    a->seen = true;
                }
                else if (kind_is_float(a->kind))
                    a->fsum += float_value(d, a->kind);
                else
                    a->isum += (unsigned __int128)(__int128)int_value(d, a->kind);
                continue;
            }

            /* MIN/MAX */
            if (a->is_numeric)
            {
                int cmp = a->seen ? DatumGetInt32(DirectFunctionCall2(numeric_cmp, d, a->nbest)) : 0;

                better = !a->seen || (a->op == FP_AGG_MIN ? cmp < 0 : cmp > 0);
                if (better) a->nbest = d;
                a->seen = true;
            }
            else if (a->is_varlena)
            {
                const char *data;
                int         len;
                int         cmp = 0;

                varlena_value(d, &data, &len);
                if (a->seen) cmp = textual_cmp(data, len, a->best, a->best_len, a->kind, a->collation);
                better = !a->seen || (a->op == FP_AGG_MIN ? cmp < 0 : cmp > 0);
                if (better)
                {
                    a->best     = data;
                    a->best_len = len;
                }
                a->seen = true;
            }
            else if (kind_is_float(a->kind))
            {
                double v = float_value(d, a->kind);

                better = !a->seen || (a->op == FP_AGG_MIN ? v < a->fmin : v > a->fmax);
                if (better)
                {
                    if (a->op == FP_AGG_MIN)
                        a->fmin = v;
                    else
                        a->fmax = v;
                }
                a->seen = true;
            }
            else
            {
                long long v = int_value(d, a->kind);

                better = !a->seen || (a->op == FP_AGG_MIN ? v < a->imin : v > a->imax);
                if (better)
                {
                    if (a->op == FP_AGG_MIN)
                        a->imin = v;
                    else
                        a->imax = v;
                }
                a->seen = true;
            }
        }
    }

    return publish(path, accs, nops, (uint64_t)nrows, capacity);
}
