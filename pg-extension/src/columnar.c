/* columnar.c — serialize a result set into the FPCOLUMN payload. */
#include "postgres.h"

#include <string.h>
#include <sys/mman.h>

#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "varatt.h"

#include "fastpostgres_shm.h"

#include "columnar.h"
#include "shm.h"

bool fastpostgres_is_text_type(Oid typ)
{
    return typ == TEXTOID || typ == VARCHAROID || typ == BPCHAROID;
}

/* Text bytes come straight off the varlena; every other type goes through its
 * output function. */
static void text_cell(Datum d, bool isnull, Oid type, Oid typoutput, const char **data, int *len)
{
    if (isnull)
    {
        *data = NULL;
        *len  = 0;
        return;
    }
    if (fastpostgres_is_text_type(type))
    {
        struct varlena *v = pg_detoast_datum_packed((struct varlena *)DatumGetPointer(d));

        *data = VARDATA_ANY(v);
        *len  = VARSIZE_ANY_EXHDR(v);
    }
    else
    {
        char *s = OidOutputFunctionCall(typoutput, d);
        *data   = s ? s : "";
        *len    = s ? (int)strlen(s) : 0;
    }
}

/* One pass into [out, end); `data_len` is backpatched once the column's bytes
 * are known. Returns the payload size written. */
uint64 fastpostgres_columnar_build(HeapTuple *tuples, TupleDesc td, uint64 nrows, char *out, char *end)
{
    char  *p = out, *base = out;
    int    ncols      = td->natts;
    int    null_bytes = (int)((nrows + 7) / 8);
    int    c;
    uint64 r;
    Datum *vals;
    bool  *nulls_arr;

    /* Deform every tuple once. Reading attribute-by-attribute with heap_getattr
     * re-walks the variable-width offsets for each column, ~O(ncols^2) per row. */
    vals      = palloc(sizeof(Datum) * nrows * ncols);
    nulls_arr = palloc(sizeof(bool) * nrows * ncols);
    for (r = 0; r < nrows; r++)
        heap_deform_tuple(tuples[r], td, vals + r * ncols, nulls_arr + r * ncols);

    memcpy(p, FASTPOSTGRES_COLUMNAR_MAGIC, 8);
    p += 8;
    fastpostgres_put_u32(p, FASTPOSTGRES_COLUMNAR_VERSION);
    p += 4;
    fastpostgres_put_u32(p, 1);
    p += 4; /* flags: little-endian */
    fastpostgres_put_u32(p, (uint32)ncols);
    p += 4;
    fastpostgres_put_u64(p, nrows);
    p += 8;

    for (c = 0; c < ncols; c++)
    {
        Form_pg_attribute att       = TupleDescAttr(td, c);
        Oid               type      = att->atttypid;
        int               kind      = fastpostgres_oid_to_kind(type);
        int               width     = fastpostgres_kind_width((uint8_t)kind);
        Oid               typoutput = InvalidOid;
        size_t            name_len  = strlen(NameStr(att->attname));
        char             *nulls, *lenp;

        if (kind == FASTPOSTGRES_KIND_TEXT && !fastpostgres_is_text_type(type))
        {
            bool varlena;
            getTypeOutputInfo(type, &typoutput, &varlena);
        }

        if (p + 4 + name_len + 1 + 4 + (size_t)null_bytes + 8 + 8 > end)
            elog(ERROR, "fastpostgres: result exceeds the segment");

        fastpostgres_put_u32(p, (uint32)name_len);
        p += 4;
        memcpy(p, NameStr(att->attname), name_len);
        p += name_len;
        *p++ = (char)kind;
        fastpostgres_put_u32(p, (uint32)type);
        p += 4;
        nulls = p;
        memset(nulls, 0, null_bytes);
        p += null_bytes;
        lenp = p;
        p += 8;       /* data_len, backpatched below */
        while (((size_t)(p - base)) & 7)
            *p++ = 0; /* 8-byte aligned column data */

        if (kind == FASTPOSTGRES_KIND_TEXT)
        {
            uint32 *off;
            char   *bytes;

            if (p + (uint64)(nrows + 1) * 4 > end) elog(ERROR, "fastpostgres: result exceeds the segment");
            off = (uint32 *)p;
            p += (nrows + 1) * 4;
            bytes  = p;
            off[0] = 0;
            for (r = 0; r < nrows; r++)
            {
                bool        isnull = nulls_arr[r * ncols + c];
                const char *d;
                int         len;
                text_cell(vals[r * ncols + c], isnull, type, typoutput, &d, &len);
                if (isnull)
                {
                    nulls[r >> 3] |= (char)(1 << (r & 7));
                    off[r + 1] = off[r];
                }
                else
                {
                    if (p + len > end) elog(ERROR, "fastpostgres: result exceeds the segment");
                    memcpy(p, d, len);
                    p += len;
                    off[r + 1] = (uint32)(p - bytes);
                }
            }
            fastpostgres_put_u64(lenp, (uint64)(nrows + 1) * 4 + (uint64)(p - bytes));
        }
        else
        {
            if (p + (uint64)nrows * (uint64)width > end)
                elog(ERROR, "fastpostgres: result exceeds the segment");
            for (r = 0; r < nrows; r++)
            {
                bool  isnull = nulls_arr[r * ncols + c];
                Datum d      = vals[r * ncols + c];
                if (isnull)
                {
                    nulls[r >> 3] |= (char)(1 << (r & 7));
                    continue;
                }
                switch (kind)
                {
                    case FASTPOSTGRES_KIND_BOOL:
                        *p = DatumGetBool(d) ? 1 : 0;
                        break;
                    case FASTPOSTGRES_KIND_INT16: {
                        int16 v = DatumGetInt16(d);
                        memcpy(p, &v, 2);
                        break;
                    }
                    case FASTPOSTGRES_KIND_INT32: {
                        int32 v = DatumGetInt32(d);
                        memcpy(p, &v, 4);
                        break;
                    }
                    case FASTPOSTGRES_KIND_INT64: {
                        int64 v = DatumGetInt64(d);
                        memcpy(p, &v, 8);
                        break;
                    }
                    case FASTPOSTGRES_KIND_FLOAT4: {
                        float4 v = DatumGetFloat4(d);
                        memcpy(p, &v, 4);
                        break;
                    }
                    case FASTPOSTGRES_KIND_FLOAT8: {
                        double v = (type == NUMERICOID)
                                       ? DatumGetFloat8(DirectFunctionCall1(numeric_float8, d))
                                       : DatumGetFloat8(d);
                        memcpy(p, &v, 8);
                        break;
                    }
                }
                p += width;
            }
            fastpostgres_put_u64(lenp, (uint64)nrows * (uint64)width);
        }
    }

    return (uint64)(p - base);
}

uint64 fastpostgres_publish_columnar_shm(const char *path, TupleDesc td, HeapTuple *tuples, uint64 nrows,
                                         size_t capacity)
{
    size_t cap   = capacity ? capacity : FASTPOSTGRES_SEGMENT_BYTES;
    void  *map   = fastpostgres_shm_map_rw(path, cap);
    uint64 total = fastpostgres_columnar_build(tuples, td, nrows, (char *)map + FASTPOSTGRES_SHM_HEADER_SIZE,
                                               (char *)map + cap);

    fastpostgres_shm_publish(map, cap, total);
    munmap(map, cap);
    return total;
}
