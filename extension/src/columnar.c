/* columnar.c — serialize a result set into the FPCOLUMN payload. */
#include "postgres.h"

#include <string.h>
#include <sys/mman.h>

#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"
#include "varatt.h"

#include "fastpostgres/fastpostgres_shm.h"

#include "columnar.h"
#include "shm.h"

bool
fastpostgres_is_text_type(Oid typ)
{
    return typ == TEXTOID || typ == VARCHAROID || typ == BPCHAROID;
}

/* Types whose bytes can be read straight off the varlena: text as itself, and
 * bytea as the raw bytes the wire kind wants (not its hex output). */
static bool
varlena_bytes(Oid typ)
{
    return fastpostgres_is_text_type(typ) || typ == BYTEAOID;
}

/* Text and bytea bytes come straight off the varlena; every other type goes
 * through its output function. */
static void
text_cell(Datum d, bool isnull, Oid type, Oid typoutput, const char **data, int *len)
{
    if (isnull)
    {
        *data = NULL;
        *len  = 0;
        return;
    }
    if (varlena_bytes(type))
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

/* One column's resolved layout, so the writer can reserve the whole frame
 * before it writes the first value. */
typedef struct ColumnPlan
{
    Form_pg_attribute att;
    uint8_t           kind;
    uint32_t          pgtype;
    int               width;
    Oid               typoutput;
    bool              varlena;   /* wire kind carries offsets + bytes */
    uint64            data_len;  /* region reserved for this column */
    uint64            used;      /* its exact data_len after the write */
    uint64            hdr_plain; /* name .. data_len, before the alignment pad */
    char             *header;    /* start of the column header */
    char             *data;      /* the reserved region */
    char             *nulls;     /* its null bitmap, zeroed by write_column */
    char             *text_p;    /* varlena: next free byte in the byte area */
    uint32           *off;       /* varlena: the (nrows + 1) offset table */
} ColumnPlan;

/* How many rows the capacity estimate looks at. Eight is enough to tell a
 * short text column from a long one; the exact fallback handles the rest. */
#define COLUMNAR_SAMPLE_ROWS 8

static void
columnar_setup(ColumnPlan *plans, TupleDesc td, uint64 nrows, bool *any_varlena)
{
    int c;

    *any_varlena = false;
    for (c = 0; c < td->natts; c++)
    {
        ColumnPlan       *pl = &plans[c];
        Form_pg_attribute att = TupleDescAttr(td, c);

        pl->att     = att;
        pl->kind    = fastpostgres_oid_to_kind(att->atttypid);
        pl->pgtype  = (uint32)att->atttypid;
        pl->width   = fastpostgres_kind_width(pl->kind);
        pl->varlena = fastpostgres_kind_is_varlena(pl->kind) != 0;
        pl->data_len = pl->varlena ? (uint64)(nrows + 1) * 4 : (uint64)nrows * (uint64)pl->width;

        if (pl->varlena && !varlena_bytes(att->atttypid))
        {
            bool varlena;
            getTypeOutputInfo(att->atttypid, &pl->typoutput, &varlena);
        }
        if (pl->varlena) *any_varlena = true;
    }
}

/* Estimate each varlena column's byte area from the first few rows, so the
 * common case needs one write pass and no sizing pass. `data_len` is only a
 * reservation: columnar_fill reports an overrun and the caller retries with
 * exact sizes. */
static void
columnar_estimate(ColumnPlan *plans, int ncols, HeapTuple *tuples, TupleDesc td, uint64 nrows,
                  Datum *rowvals, bool *rownulls)
{
    uint64  sample = nrows < COLUMNAR_SAMPLE_ROWS ? nrows : COLUMNAR_SAMPLE_ROWS;
    uint64 *maxlen = palloc0(sizeof(uint64) * (ncols > 0 ? ncols : 1));
    uint64  r;
    int     c;

    for (r = 0; r < sample; r++)
    {
        heap_deform_tuple(tuples[r], td, rowvals, rownulls);
        for (c = 0; c < ncols; c++)
        {
            ColumnPlan *pl = &plans[c];
            const char *d;
            int         len;

            if (!pl->varlena || rownulls[c]) continue;
            text_cell(rowvals[c], false, pl->att->atttypid, pl->typoutput, &d, &len);
            if ((uint64)len > maxlen[c]) maxlen[c] = (uint64)len;
        }
    }

    for (c = 0; c < ncols; c++)
    {
        ColumnPlan *pl = &plans[c];
        uint64      per_row;

        if (!pl->varlena) continue;
        /* One sample's longest value plus a floor that covers tiny/NULL-heavy
         * samples and the "+ 16 B" is the per-row allowance for growth. */
        per_row = maxlen[c] + 16;
        if (per_row < 24) per_row = 24;
        pl->data_len = (uint64)(nrows + 1) * 4 + per_row * nrows;
    }
}

/* Exact sizes, for the retry after a capacity estimate overruns. This is the
 * same walk the old writer did once; it keeps only lengths. */
static void
columnar_size_exact(ColumnPlan *plans, int ncols, HeapTuple *tuples, TupleDesc td, uint64 nrows,
                    Datum *rowvals, bool *rownulls)
{
    uint64 r;
    int    c;

    for (c = 0; c < ncols; c++)
        if (plans[c].varlena) plans[c].data_len = (uint64)(nrows + 1) * 4;

    for (r = 0; r < nrows; r++)
    {
        heap_deform_tuple(tuples[r], td, rowvals, rownulls);
        for (c = 0; c < ncols; c++)
        {
            ColumnPlan *pl = &plans[c];
            const char *d;
            int         len;

            if (!pl->varlena || rownulls[c]) continue;
            text_cell(rowvals[c], false, pl->att->atttypid, pl->typoutput, &d, &len);
            pl->data_len += (uint64)len;
        }
    }
}

/* Reserve every column's header and its data region, then write each value
 * once, in place. Returns 0 when a column overruns its reservation or the
 * reserved frame does not fit; the caller retries with exact sizes. */
static int
columnar_fill(ColumnPlan *plans, int ncols, HeapTuple *tuples, TupleDesc td, uint64 nrows,
              uint32 null_bytes, char *out, char *end, Datum *rowvals, bool *rownulls)
{
    FastpostgresColumnarWrite w;
    uint64                    r;
    int                       c;

    fastpostgres_columnar_write_begin(&w, out, (uint32)ncols, nrows);

    for (c = 0; c < ncols; c++)
    {
        ColumnPlan *pl       = &plans[c];
        size_t      name_len = strlen(NameStr(pl->att->attname));
        char       *data;

        pl->header    = w.p;
        pl->hdr_plain = 4 + (uint64)name_len + 1 + 4 + (uint64)null_bytes + 8;
        if (w.p + pl->hdr_plain + 8 > end) return 0;

        data      = fastpostgres_columnar_write_column(&w, NameStr(pl->att->attname), (uint32)name_len,
                                                       pl->kind, pl->pgtype, null_bytes);
        pl->data  = data;
        pl->nulls = w.nulls;

        if (pl->varlena)
        {
            pl->off    = (uint32 *)data;
            pl->text_p = data + (nrows + 1) * 4;
            pl->off[0] = 0;
        }

        if (w.p + pl->data_len > end) return 0;
        fastpostgres_columnar_write_end(&w, pl->data_len);
    }

    for (r = 0; r < nrows; r++)
    {
        heap_deform_tuple(tuples[r], td, rowvals, rownulls);
        for (c = 0; c < ncols; c++)
        {
            ColumnPlan *pl     = &plans[c];
            bool        isnull = rownulls[c];

            if (isnull)
            {
                pl->nulls[r >> 3] |= (char)(1 << (r & 7));
                if (pl->varlena)
                    pl->off[r + 1] = pl->off[r]; /* a null consumes no bytes */
                else
                    memset(pl->data + r * (uint64)pl->width, 0,
                           (size_t)pl->width); /* data_len == nrows * width */
                continue;
            }

            if (pl->varlena)
            {
                const char *d;
                int         len;

                text_cell(rowvals[c], false, pl->att->atttypid, pl->typoutput, &d, &len);
                if ((uint64)(pl->text_p - pl->data) + (uint64)len > pl->data_len) return 0;
                memcpy(pl->text_p, d, (size_t)len);
                pl->text_p += len;
                pl->off[r + 1] = (uint32)(pl->text_p - (pl->data + (nrows + 1) * 4));
            }
            else
            {
                Datum d    = rowvals[c];
                char *slot = pl->data + r * (uint64)pl->width;

                switch (pl->kind)
                {
                    case FASTPOSTGRES_KIND_BOOL:
                        *slot = DatumGetBool(d) ? 1 : 0;
                        break;
                    case FASTPOSTGRES_KIND_INT16: {
                        int16 v = DatumGetInt16(d);
                        memcpy(slot, &v, 2);
                        break;
                    }
                    case FASTPOSTGRES_KIND_INT32: {
                        int32 v = DatumGetInt32(d);
                        memcpy(slot, &v, 4);
                        break;
                    }
                    case FASTPOSTGRES_KIND_INT64: {
                        int64 v = DatumGetInt64(d);
                        memcpy(slot, &v, 8);
                        break;
                    }
                    case FASTPOSTGRES_KIND_FLOAT4: {
                        float4 v = DatumGetFloat4(d);
                        memcpy(slot, &v, 4);
                        break;
                    }
                    case FASTPOSTGRES_KIND_FLOAT8: {
                        double v = (pl->att->atttypid == NUMERICOID)
                                       ? DatumGetFloat8(DirectFunctionCall1(numeric_float8, d))
                                       : DatumGetFloat8(d);
                        memcpy(slot, &v, 8);
                        break;
                    }
                    /* The binary kinds: the Datum already holds the wire value,
                     * so these are a copy, not a conversion. */
                    case FASTPOSTGRES_KIND_DATE: {
                        int32 v = DatumGetDateADT(d);
                        memcpy(slot, &v, 4);
                        break;
                    }
                    case FASTPOSTGRES_KIND_TIME: {
                        int64 v = DatumGetTimeADT(d);
                        memcpy(slot, &v, 8);
                        break;
                    }
                    case FASTPOSTGRES_KIND_TIMESTAMP:
                    case FASTPOSTGRES_KIND_TIMESTAMPTZ: {
                        int64 v = DatumGetTimestamp(d);
                        memcpy(slot, &v, 8);
                        break;
                    }
                    case FASTPOSTGRES_KIND_UUID: {
                        pg_uuid_t *u = DatumGetUUIDP(d);
                        memcpy(slot, u->data, 16);
                        break;
                    }
                }
            }
        }
    }

    for (c = 0; c < ncols; c++)
        plans[c].used = plans[c].varlena ? (uint64)(plans[c].text_p - plans[c].data)
                                          : plans[c].data_len;
    return 1;
}

/* Close the gap the capacity estimate left in front of each column, so the
 * final bytes and data_len are the exact ones the frame always carried. The
 * move is leftwards only: a column's new data can never pass its old end, so
 * later columns are still readable when their turn comes. */
static char *
columnar_compact(ColumnPlan *plans, int ncols, char *out)
{
    char *dst = out + FASTPOSTGRES_COLUMNAR_FRAME;
    int   c;

    for (c = 0; c < ncols; c++)
    {
        ColumnPlan *pl       = &plans[c];
        uint64      dst_off  = (uint64)(dst - out);
        uint64      data_off = FASTPOSTGRES_ALIGN8(dst_off + pl->hdr_plain);
        char       *data     = out + data_off;

        if (dst != pl->header) memmove(dst, pl->header, (size_t)pl->hdr_plain);
        if (data_off > dst_off + pl->hdr_plain)
            memset(dst + pl->hdr_plain, 0, (size_t)(data_off - dst_off - pl->hdr_plain));
        fastpostgres_put_u64(dst + pl->hdr_plain - 8, pl->used); /* exact data_len */

        if (pl->used > 0 && data != pl->data) memmove(data, pl->data, (size_t)pl->used);
        dst = data + pl->used;
    }
    return dst;
}

/* Build the frame directly into its final column regions.
 *
 * The old shape palloc'd a row-major Datum/bool scratch (8 B + 1 B per value)
 * and transposed it into the payload, so every value was touched twice and
 * every chunk paid a fresh multi-MiB allocation and its page faults. Here the
 * fixed-width sizes are exact, the varlena areas are reserved from a sample of
 * the first rows, every column's header and region is laid out up front, and
 * each value is written once, in place. A sampled text column that grows past
 * its reservation falls back to exact sizing and one more pass; the common
 * case takes neither.
 *
 * The frame comes from the shared writer, so the layout matches the client's
 * generators exactly. Returns the payload size. */
uint64
fastpostgres_columnar_build(HeapTuple *tuples, TupleDesc td, uint64 nrows, char *out, char *end)
{
    int                       ncols      = td->natts;
    uint32                    null_bytes = (uint32)((nrows + 7) / 8);
    int                       nalloc     = ncols > 0 ? ncols : 1;
    ColumnPlan               *plans;
    Datum                    *rowvals;
    bool                     *rownulls;
    bool                      any_varlena;
    char                     *final;
    MemoryContext             tmp_ctx = NULL;
    MemoryContext             old_ctx = CurrentMemoryContext;

    plans    = palloc0(sizeof(ColumnPlan) * nalloc);
    rowvals  = palloc(sizeof(Datum) * nalloc);
    rownulls = palloc(sizeof(bool) * nalloc);

    columnar_setup(plans, td, nrows, &any_varlena);

    /* Detoast copies and output-function strings are chunk temporaries, not
     * frame bytes: give them a context this build owns and delete it at the
     * end instead of leaving them in the SPI context. */
    if (any_varlena)
    {
        tmp_ctx = AllocSetContextCreate(CurrentMemoryContext, "fastpostgres columnar", ALLOCSET_DEFAULT_SIZES);
        old_ctx = MemoryContextSwitchTo(tmp_ctx);
        columnar_estimate(plans, ncols, tuples, td, nrows, rowvals, rownulls);
        MemoryContextReset(tmp_ctx);
    }

    if (!columnar_fill(plans, ncols, tuples, td, nrows, null_bytes, out, end, rowvals, rownulls))
    {
        if (!any_varlena) elog(ERROR, "fastpostgres: result exceeds the segment");

        /* The estimate was too small (or the reserved frame did not fit):
         * size the text areas exactly and fill once more. */
        MemoryContextReset(tmp_ctx);
        columnar_size_exact(plans, ncols, tuples, td, nrows, rowvals, rownulls);
        MemoryContextReset(tmp_ctx);

        if (!columnar_fill(plans, ncols, tuples, td, nrows, null_bytes, out, end, rowvals, rownulls))
            elog(ERROR, "fastpostgres: result exceeds the segment");
    }

    final = columnar_compact(plans, ncols, out);

    if (tmp_ctx != NULL)
    {
        MemoryContextSwitchTo(old_ctx);
        MemoryContextDelete(tmp_ctx);
    }

    return (uint64)(final - out);
}

uint64
fastpostgres_publish_columnar_shm(const char *path, TupleDesc td, HeapTuple *tuples, uint64 nrows,
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
