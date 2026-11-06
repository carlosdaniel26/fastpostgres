/* columnar.cpp — serialize a result set into the FPCOLUMN payload. */
#include "pgxx.h"

#include <cstring>

extern "C" {
#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "catalog/pg_type.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "tcop/dest.h"
#include "tcop/pquery.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"
#include "varatt.h"

#include "fastpostgres/fastpostgres_shm.h"
}
#include "carry.h"
#include "columnar.h"
#include "shm.h"

bool
fp::is_text_type(Oid typ)
{
    return typ == TEXTOID || typ == VARCHAROID || typ == BPCHAROID;
}

namespace
{

/* Types whose bytes can be read straight off the varlena: text as itself, and
 * bytea as the raw bytes the wire kind wants (not its hex output). */
bool
varlena_bytes(Oid typ)
{
    return fp::is_text_type(typ) || typ == BYTEAOID;
}

/* Text and bytea bytes come straight off the varlena; every other type goes
 * through its output function. */
void
text_cell(Datum d, bool isnull, Oid type, Oid typoutput, const char **data, int *len)
{
    if (isnull)
    {
        *data = nullptr;
        *len  = 0;
        return;
    }
    if (varlena_bytes(type))
    {
        struct varlena *v = pg_detoast_datum_packed(reinterpret_cast<struct varlena *>(DatumGetPointer(d)));

        *data = VARDATA_ANY(v);
        *len  = VARSIZE_ANY_EXHDR(v);
    }
    else
    {
        char *s = OidOutputFunctionCall(typoutput, d);
        *data   = s != nullptr ? s : "";
        *len    = s != nullptr ? static_cast<int>(std::strlen(s)) : 0;
    }
}

/* One column's resolved layout, so the writer can reserve the whole frame
 * before it writes the first value. */
struct ColumnPlan {
    Form_pg_attribute att;
    uint8_t           kind;
    uint32_t          pgtype;
    int               width;
    Oid               typoutput;
    bool              varlena;    /* wire kind carries offsets + bytes */
    uint64            per_row;    /* varlena: bytes the sample allows for one row */
    uint64            data_len;   /* region reserved for this column */
    uint64            used;       /* its exact data_len after the write */
    uint64            hdr_plain;  /* name .. data_len, before the alignment pad */
    char             *header;     /* start of the column header */
    char             *data;       /* the reserved region */
    char             *nulls;      /* its null bitmap, zeroed by write_column */
    char             *text_p;     /* varlena: next free byte in the byte area */
    char             *bytes;      /* varlena: first byte after the offset table */
    char             *checkpoint; /* varlena: text_p before the row being written */
    uint32           *off;        /* varlena: the (nrows + 1) offset table */
};

/* How many rows the capacity estimate looks at. Eight is enough to tell a
 * short text column from a long one; the exact fallback handles the rest. */
constexpr int kColumnarSampleRows = 8;

void
columnar_setup(ColumnPlan *plans, TupleDesc td, bool *any_varlena)
{
    *any_varlena = false;
    for (int c = 0; c < td->natts; c++)
    {
        ColumnPlan       *pl  = &plans[c];
        Form_pg_attribute att = TupleDescAttr(td, c);

        pl->att      = att;
        pl->kind     = fastpostgres_oid_to_kind(att->atttypid);
        pl->pgtype   = static_cast<uint32>(att->atttypid);
        pl->width    = fastpostgres_kind_width(pl->kind);
        pl->varlena  = fastpostgres_kind_is_varlena(pl->kind) != 0;
        pl->per_row  = 0;
        pl->data_len = 0;
        pl->used     = 0;

        if (pl->varlena && !varlena_bytes(att->atttypid))
        {
            [[maybe_unused]] bool varlena;

            getTypeOutputInfo(att->atttypid, &pl->typoutput, &varlena);
        }
        if (pl->varlena) *any_varlena = true;
    }
}

/* How much byte area one row of a varlena column gets in the reservation: the
 * longest value in the sample, plus a floor for a NULL/short sample, plus room
 * to grow. Over-reserving costs no page faults (the gap is never written) and
 * no final bytes (the frame is compacted to the exact length before it is
 * published). */
void
columnar_estimate(ColumnPlan *plans, int ncols, HeapTuple *tuples, TupleDesc td, uint64 nsample,
                  Datum *rowvals, bool *rownulls)
{
    uint64 *maxlen =
        static_cast<uint64 *>(palloc0(sizeof(uint64) * static_cast<size_t>(ncols > 0 ? ncols : 1)));

    for (uint64 r = 0; r < nsample; r++)
    {
        heap_deform_tuple(tuples[r], td, rowvals, rownulls);
        for (int c = 0; c < ncols; c++)
        {
            ColumnPlan *pl  = &plans[c];
            const char *d   = nullptr;
            int         len = 0;

            if (!pl->varlena || rownulls[c]) continue;
            text_cell(rowvals[c], false, pl->att->atttypid, pl->typoutput, &d, &len);
            if (static_cast<uint64>(len) > maxlen[c]) maxlen[c] = static_cast<uint64>(len);
        }
    }

    for (int c = 0; c < ncols; c++)
    {
        ColumnPlan *pl = &plans[c];

        if (!pl->varlena) continue;
        pl->per_row = maxlen[c] + 16;
        if (pl->per_row < 24) pl->per_row = 24;
    }
    pfree(maxlen);
}

/* The per-column region sizes for `nrows` rows, from the sample. */
void
columnar_size_estimate(ColumnPlan *plans, int ncols, uint64 nrows)
{
    for (int c = 0; c < ncols; c++)
    {
        ColumnPlan *pl = &plans[c];

        pl->data_len = pl->varlena ? static_cast<uint64>(nrows + 1) * 4 + pl->per_row * nrows
                                   : static_cast<uint64>(nrows) * static_cast<uint64>(pl->width);
    }
}

/* Exact sizes, for the pull path's retry after a capacity estimate overruns.
 * This is the same walk the old writer did once; it keeps only lengths. */
void
columnar_size_exact(ColumnPlan *plans, int ncols, HeapTuple *tuples, TupleDesc td, uint64 nrows,
                    Datum *rowvals, bool *rownulls)
{
    for (int c = 0; c < ncols; c++)
        if (plans[c].varlena) plans[c].data_len = static_cast<uint64>(nrows + 1) * 4;

    for (uint64 r = 0; r < nrows; r++)
    {
        heap_deform_tuple(tuples[r], td, rowvals, rownulls);
        for (int c = 0; c < ncols; c++)
        {
            ColumnPlan *pl  = &plans[c];
            const char *d   = nullptr;
            int         len = 0;

            if (!pl->varlena || rownulls[c]) continue;
            text_cell(rowvals[c], false, pl->att->atttypid, pl->typoutput, &d, &len);
            pl->data_len += static_cast<uint64>(len);
        }
    }
}

/* Bytes the whole frame needs at `nrows`, matching columnar_reserve's walk. */
uint64
columnar_reserved_size(ColumnPlan *plans, int ncols, uint64 nrows)
{
    uint32 null_bytes = static_cast<uint32>((nrows + 7) / 8);
    uint64 off        = FASTPOSTGRES_COLUMNAR_FRAME + FASTPOSTGRES_COLUMNAR_TABLE(ncols);

    for (int c = 0; c < ncols; c++)
    {
        size_t name_len = std::strlen(NameStr(plans[c].att->attname));

        off = FASTPOSTGRES_ALIGN8(off + 4 + static_cast<uint64>(name_len) + 1 + 4 + null_bytes + 8);
        off += plans[c].data_len;
    }
    return off;
}

/* Reserve every column's header and its data region. Returns 0 when the
 * reserved frame does not fit; the caller retries with fewer rows. */
int
columnar_reserve(ColumnPlan *plans, int ncols, TupleDesc td, uint64 nrows, uint32 null_bytes, char *out,
                 char *end, FastpostgresColumnarWrite *w)
{
    fastpostgres_columnar_write_begin(w, out, static_cast<uint32>(ncols), nrows);

    for (int c = 0; c < ncols; c++)
    {
        ColumnPlan *pl       = &plans[c];
        size_t      name_len = std::strlen(NameStr(pl->att->attname));

        pl->header    = w->p;
        pl->hdr_plain = 4 + static_cast<uint64>(name_len) + 1 + 4 + static_cast<uint64>(null_bytes) + 8;
        if (w->p + pl->hdr_plain + 8 > end) return 0;

        char *data = fastpostgres_columnar_write_column(
            w, NameStr(pl->att->attname), static_cast<uint32>(name_len), pl->kind, pl->pgtype, null_bytes);
        pl->data  = data;
        pl->nulls = w->nulls;

        if (pl->varlena)
        {
            pl->off    = reinterpret_cast<uint32 *>(data);
            pl->bytes  = data + (nrows + 1) * 4;
            pl->text_p = pl->bytes;
            pl->off[0] = 0;
        }

        if (w->p + pl->data_len > end) return 0;
        fastpostgres_columnar_write_end(w, pl->data_len);
    }
    return 1;
}

/* Write one value in its column's region. Returns 0 when a varlena value does
 * not fit the reservation; the caller retries (pull path) or carries the row
 * (streaming). A failed row is rolled back: the byte-area bumps are undone and
 * its null bits are cleared, so the frame still ends exactly at the last
 * complete row. */
int
columnar_write_row(ColumnPlan *plans, int ncols, uint64 r, Datum *vals, bool *isnull)
{
    for (int c = 0; c < ncols; c++)
        if (plans[c].varlena) plans[c].checkpoint = plans[c].text_p;

    for (int c = 0; c < ncols; c++)
    {
        ColumnPlan *pl   = &plans[c];
        bool        null = isnull[c];

        if (null)
        {
            pl->nulls[r >> 3] |= static_cast<char>(1 << (r & 7));
            if (pl->varlena)
                pl->off[r + 1] = pl->off[r]; /* a null consumes no bytes */
            else
                std::memset(pl->data + r * static_cast<uint64>(pl->width), 0,
                            static_cast<size_t>(pl->width)); /* data_len == nrows * width */
            continue;
        }

        if (pl->varlena)
        {
            const char *d   = nullptr;
            int         len = 0;

            text_cell(vals[c], false, pl->att->atttypid, pl->typoutput, &d, &len);
            if (static_cast<uint64>(pl->text_p - pl->data) + static_cast<uint64>(len) > pl->data_len)
                goto overflow;
            std::memcpy(pl->text_p, d, static_cast<size_t>(len));
            pl->text_p += len;
            pl->off[r + 1] = static_cast<uint32>(pl->text_p - pl->bytes);
        }
        else
        {
            Datum d    = vals[c];
            char *slot = pl->data + r * static_cast<uint64>(pl->width);

            switch (pl->kind)
            {
                case FASTPOSTGRES_KIND_BOOL:
                    *slot = DatumGetBool(d) ? 1 : 0;
                    break;
                case FASTPOSTGRES_KIND_INT16: {
                    int16 v = DatumGetInt16(d);
                    std::memcpy(slot, &v, 2);
                    break;
                }
                case FASTPOSTGRES_KIND_INT32: {
                    int32 v = DatumGetInt32(d);
                    std::memcpy(slot, &v, 4);
                    break;
                }
                case FASTPOSTGRES_KIND_INT64: {
                    int64 v = DatumGetInt64(d);
                    std::memcpy(slot, &v, 8);
                    break;
                }
                case FASTPOSTGRES_KIND_FLOAT4: {
                    float4 v = DatumGetFloat4(d);
                    std::memcpy(slot, &v, 4);
                    break;
                }
                case FASTPOSTGRES_KIND_FLOAT8: {
                    double v = (pl->att->atttypid == NUMERICOID)
                                   ? DatumGetFloat8(DirectFunctionCall1(numeric_float8, d))
                                   : DatumGetFloat8(d);
                    std::memcpy(slot, &v, 8);
                    break;
                }
                /* The binary kinds: the Datum already holds the wire value,
                 * so these are a copy, not a conversion. */
                case FASTPOSTGRES_KIND_DATE: {
                    int32 v = DatumGetDateADT(d);
                    std::memcpy(slot, &v, 4);
                    break;
                }
                case FASTPOSTGRES_KIND_TIME: {
                    int64 v = DatumGetTimeADT(d);
                    std::memcpy(slot, &v, 8);
                    break;
                }
                case FASTPOSTGRES_KIND_TIMESTAMP:
                case FASTPOSTGRES_KIND_TIMESTAMPTZ: {
                    int64 v = DatumGetTimestamp(d);
                    std::memcpy(slot, &v, 8);
                    break;
                }
                case FASTPOSTGRES_KIND_UUID: {
                    pg_uuid_t *u = DatumGetUUIDP(d);
                    std::memcpy(slot, u->data, 16);
                    break;
                }
            }
        }
    }
    return 1;

overflow:
    for (int c = 0; c < ncols; c++)
    {
        if (plans[c].varlena) plans[c].text_p = plans[c].checkpoint;
        plans[c].nulls[r >> 3] &= static_cast<char>(~(1 << (r & 7)));
    }
    return 0;
}

/* Record the exact region length of each column. */
void
columnar_note_used(ColumnPlan *plans, int ncols, uint64 nrows)
{
    for (int c = 0; c < ncols; c++)
    {
        ColumnPlan *pl = &plans[c];

        pl->used = pl->varlena ? static_cast<uint64>(pl->text_p - pl->data)
                               : static_cast<uint64>(nrows) * static_cast<uint64>(pl->width);
    }
}

/* Close the gap the capacity estimate left in front of each column, so the
 * final bytes and data_len are the exact ones the frame always carried. A row
 * count below the reservation also shrinks each null bitmap and the varlena
 * offset stride (the last batch of a query is the normal case for that). The
 * move is leftwards only: a column's new data can never pass its old end, so
 * later columns are still readable when their turn comes. */
char *
columnar_compact(ColumnPlan *plans, int ncols, uint64 old_rows, uint64 new_rows, char *out)
{
    char  *table          = out + FASTPOSTGRES_COLUMNAR_FRAME;
    char  *dst            = out + FASTPOSTGRES_COLUMNAR_FRAME + FASTPOSTGRES_COLUMNAR_TABLE(ncols);
    uint32 new_null_bytes = static_cast<uint32>((new_rows + 7) / 8);

    for (int c = 0; c < ncols; c++)
    {
        ColumnPlan *pl        = &plans[c];
        size_t      name_len  = std::strlen(NameStr(pl->att->attname));
        uint64      dst_off   = static_cast<uint64>(dst - out);
        uint64      hdr_plain = 4 + static_cast<uint64>(name_len) + 1 + 4 + new_null_bytes + 8;
        uint64      data_off  = FASTPOSTGRES_ALIGN8(dst_off + hdr_plain);
        char       *data      = out + data_off;
        uint64      used      = pl->used;

        fastpostgres_put_u64(table + static_cast<size_t>(c) * 8, dst_off);
        if (pl->varlena && old_rows != new_rows)
            used = static_cast<uint64>(new_rows + 1) * 4 + (pl->used - static_cast<uint64>(old_rows + 1) * 4);
        else if (!pl->varlena)
            used = static_cast<uint64>(new_rows) * static_cast<uint64>(pl->width);

        if (old_rows == new_rows)
        {
            /* the common full batch: header and data only shrink by the gap */
            std::memmove(dst, pl->header, static_cast<size_t>(pl->hdr_plain));
            fastpostgres_put_u64(dst + pl->hdr_plain - 8, used);
            if (data_off > dst_off + pl->hdr_plain)
                std::memset(dst + pl->hdr_plain, 0, static_cast<size_t>(data_off - dst_off - pl->hdr_plain));
            if (used > 0) std::memmove(data, pl->data, static_cast<size_t>(used));
        }
        else
        {
            uint64 prefix = 4 + static_cast<uint64>(name_len) + 1 + 4;

            std::memmove(dst, pl->header, static_cast<size_t>(prefix));
            std::memcpy(dst + prefix, pl->nulls, new_null_bytes);
            std::memset(dst + hdr_plain, 0, static_cast<size_t>(data_off - dst_off - hdr_plain));
            fastpostgres_put_u64(dst + hdr_plain - 8, used);

            if (pl->varlena)
            {
                uint64 offsets = static_cast<uint64>(new_rows + 1) * 4;
                uint64 bytes   = pl->used - static_cast<uint64>(old_rows + 1) * 4;

                std::memmove(data, pl->data, static_cast<size_t>(offsets));
                if (bytes > 0)
                    std::memmove(data + offsets, pl->data + static_cast<uint64>(old_rows + 1) * 4,
                                 static_cast<size_t>(bytes));
            }
            else if (used > 0)
                std::memmove(data, pl->data, static_cast<size_t>(used));
        }
        dst = data + used;
    }
    return dst;
}

/* Finish a frame without moving anything: every column's data_len is patched to
 * the bytes actually used and the offset table already points at the reserved
 * header positions. The payload ends at the furthest used byte, so trailing
 * reservations cost nothing on the wire. */
char *
columnar_finalize(ColumnPlan *plans, int ncols, char *out)
{
    char *end = out + FASTPOSTGRES_COLUMNAR_FRAME + FASTPOSTGRES_COLUMNAR_TABLE(ncols);

    for (int c = 0; c < ncols; c++)
    {
        ColumnPlan *pl = &plans[c];

        fastpostgres_put_u64(pl->header + pl->hdr_plain - 8, pl->used);

        char *p = pl->header + pl->hdr_plain;
        if (p > end) end = p;
        p = pl->data + pl->used;
        if (p > end) end = p;
    }
    return end;
}

} // namespace

/* Build the frame directly into its final column regions from a materialised
 * batch. Kept for callers that already have HeapTuples (and for the exact-size
 * retry): the streaming path below is what a cursor uses.
 *
 * The fixed-width sizes are exact, the varlena areas are reserved from a sample
 * of the first rows, every column's header and region is laid out up front, and
 * each value is written once, in place. A sampled text column that grows past
 * its reservation falls back to exact sizing and one more pass. */
uint64
fp::columnar_build(HeapTuple *tuples, TupleDesc td, uint64 nrows, char *out, char *end)
{
    int    ncols      = td->natts;
    uint32 null_bytes = static_cast<uint32>((nrows + 7) / 8);
    int    nalloc     = ncols > 0 ? ncols : 1;

    auto *plans    = static_cast<ColumnPlan *>(palloc0(sizeof(ColumnPlan) * static_cast<size_t>(nalloc)));
    auto *rowvals  = static_cast<Datum *>(palloc(sizeof(Datum) * static_cast<size_t>(nalloc)));
    auto *rownulls = static_cast<bool *>(palloc(sizeof(bool) * static_cast<size_t>(nalloc)));

    bool any_varlena;

    columnar_setup(plans, td, &any_varlena);

    /* Detoast copies and output-function strings are chunk temporaries, not
     * frame bytes: give them a context this build owns and delete it at the
     * end instead of leaving them in the SPI context. */
    MemoryContext tmp_ctx = nullptr;
    MemoryContext old_ctx = CurrentMemoryContext;

    if (any_varlena)
    {
        uint64 sample = nrows < static_cast<uint64>(kColumnarSampleRows)
                            ? nrows
                            : static_cast<uint64>(kColumnarSampleRows);

        tmp_ctx =
            AllocSetContextCreate(CurrentMemoryContext, "fastpostgres columnar", ALLOCSET_DEFAULT_SIZES);
        old_ctx = MemoryContextSwitchTo(tmp_ctx);
        columnar_estimate(plans, ncols, tuples, td, sample, rowvals, rownulls);
        MemoryContextReset(tmp_ctx);
    }

    FastpostgresColumnarWrite w;

    columnar_size_estimate(plans, ncols, nrows);
    if (!columnar_reserve(plans, ncols, td, nrows, null_bytes, out, end, &w))
    {
        if (!any_varlena) elog(ERROR, "fastpostgres: result exceeds the segment");

        /* The estimate was too small (or the reserved frame did not fit):
         * size the text areas exactly and fill once more. */
        MemoryContextReset(tmp_ctx);
        columnar_size_exact(plans, ncols, tuples, td, nrows, rowvals, rownulls);
        MemoryContextReset(tmp_ctx);

        if (!columnar_reserve(plans, ncols, td, nrows, null_bytes, out, end, &w))
            elog(ERROR, "fastpostgres: result exceeds the segment");
    }

    for (uint64 r = 0; r < nrows; r++)
    {
        heap_deform_tuple(tuples[r], td, rowvals, rownulls);
        if (!columnar_write_row(plans, ncols, r, rowvals, rownulls))
        {
            elog(ERROR, "fastpostgres: result exceeds the segment");
        }
    }

    columnar_note_used(plans, ncols, nrows);
    char *final = columnar_finalize(plans, ncols, out);

    if (tmp_ctx != nullptr)
    {
        MemoryContextSwitchTo(old_ctx);
        MemoryContextDelete(tmp_ctx);
    }

    return static_cast<uint64>(final - out);
}

/* ---- the streaming receiver --------------------------------------------- */

namespace
{

struct ColumnarReceiver {
    DestReceiver              pub;
    TupleDesc                 td;
    int                       ncols;
    uint32                    null_bytes;
    uint64                    want;  /* the rows the batch asked for */
    uint64                    cap;   /* the rows the reservation was made for */
    uint64                    nrows; /* rows written so far */
    bool                      laid_out;
    ColumnPlan               *plans;
    Datum                    *rowvals;
    bool                     *rownulls;
    HeapTuple                 samples[kColumnarSampleRows];
    int                       nsample;
    MemoryContext             ctx;    /* sample/carry copies: reset once the layout exists */
    MemoryContext             rowctx; /* detoast/output temporaries: reset per row */
    fp::Carry                *carry;
    char                     *out;
    char                     *end;
    FastpostgresColumnarWrite w;
};

/* Copy the slot's tuple into `ctx` (or build one when the slot cannot hand its
 * own bytes over). A copy is what lets the receiver sample a few rows and then
 * write them after the layout is known. */
HeapTuple
columnar_slot_tuple(ColumnarReceiver *rx, TupleTableSlot *slot)
{
    MemoryContext old = MemoryContextSwitchTo(rx->ctx);
    HeapTuple     src = nullptr;
    HeapTuple     out;

    slot_getallattrs(slot);
    if (slot->tts_ops->get_heap_tuple != nullptr) src = ExecFetchSlotHeapTuple(slot, false, nullptr);

    if (src != nullptr)
    {
        out         = static_cast<HeapTuple>(palloc0(sizeof(HeapTupleData)));
        out->t_len  = src->t_len;
        out->t_data = reinterpret_cast<HeapTupleHeader>(palloc(src->t_len));
        std::memcpy(out->t_data, src->t_data, src->t_len);
    }
    else
        out = heap_form_tuple(rx->td, slot->tts_values, slot->tts_isnull);

    MemoryContextSwitchTo(old);
    return out;
}

void
columnar_enqueue_slot(ColumnarReceiver *rx, TupleTableSlot *slot)
{
    HeapTuple tup = columnar_slot_tuple(rx, slot);

    fp::carry_put(rx->carry, reinterpret_cast<const char *>(tup->t_data), tup->t_len);
}

/* Write rows the previous batch already consumed, in order, until the frame is
 * full. A row that still does not fit stays queued for the next batch. */
int
columnar_drain_carry(ColumnarReceiver *rx)
{
    while (rx->nrows < rx->cap)
    {
        uint32 len;
        char  *row = fp::carry_peek(rx->carry, &len);

        if (row == nullptr) return 1;

        HeapTupleData tup;

        tup.t_len  = len;
        tup.t_data = reinterpret_cast<HeapTupleHeader>(row);

        MemoryContext old = MemoryContextSwitchTo(rx->rowctx);

        heap_deform_tuple(&tup, rx->td, rx->rowvals, rx->rownulls);
        bool ok = columnar_write_row(rx->plans, rx->ncols, rx->nrows, rx->rowvals, rx->rownulls) != 0;
        MemoryContextSwitchTo(old);
        MemoryContextReset(rx->rowctx);

        if (!ok) return 0; /* the reservation is exhausted: the row stays queued */
        fp::carry_pop(rx->carry);
        rx->nrows++;
    }
    return 1;
}

/* Write the sampled rows (and anything already queued) into the reserved frame.
 * Rows past the capacity the segment could take go back to the carry queue. */
void
columnar_flush_samples(ColumnarReceiver *rx)
{
    for (int i = 0; i < rx->nsample; i++)
    {
        if (rx->nrows < rx->cap)
        {
            MemoryContext old = MemoryContextSwitchTo(rx->rowctx);

            heap_deform_tuple(rx->samples[i], rx->td, rx->rowvals, rx->rownulls);
            if (columnar_write_row(rx->plans, rx->ncols, rx->nrows, rx->rowvals, rx->rownulls))
            {
                rx->nrows++;
                MemoryContextSwitchTo(old);
                MemoryContextReset(rx->rowctx);
                continue;
            }
            MemoryContextSwitchTo(old);
            MemoryContextReset(rx->rowctx);
        }
        fp::carry_put(rx->carry, reinterpret_cast<const char *>(rx->samples[i]->t_data),
                      rx->samples[i]->t_len);
    }
    rx->nsample = 0;
    MemoryContextReset(rx->ctx);
    columnar_drain_carry(rx);
}

/* Pick the largest row count at or below `want` whose reserved frame fits, lay
 * the frame out for it, and flush the sample into it. */
void
columnar_layout(ColumnarReceiver *rx, uint64 want)
{
    uint64 avail = static_cast<uint64>(rx->end - rx->out);

    /* The sample decides the varlena reservation. It is also what the pull
     * path has always used; the difference is that these rows are already in
     * memory and are written into the frame below, not read again from a
     * tuplestore. */
    if (rx->nsample > 0)
    {
        MemoryContext old = MemoryContextSwitchTo(rx->ctx);

        columnar_estimate(rx->plans, rx->ncols, rx->samples, rx->td, static_cast<uint64>(rx->nsample),
                          rx->rowvals, rx->rownulls);
        MemoryContextSwitchTo(old);
    }

    if (want == 0)
    {
        rx->cap        = 0;
        rx->null_bytes = 0;
        columnar_size_estimate(rx->plans, rx->ncols, 0);
    }
    else
    {
        uint64 cap = want;

        for (;;)
        {
            rx->null_bytes = static_cast<uint32>((cap + 7) / 8);
            columnar_size_estimate(rx->plans, rx->ncols, cap);
            uint64 need = columnar_reserved_size(rx->plans, rx->ncols, cap);
            if (need <= avail) break;
            if (cap <= 1) elog(ERROR, "fastpostgres: result exceeds the segment");

            uint64 next = cap * avail / need;

            if (next >= cap) next = cap - 1;
            if (next < 1) next = 1;
            cap = next;
        }
        rx->cap = cap;
    }

    if (!columnar_reserve(rx->plans, rx->ncols, rx->td, rx->cap, rx->null_bytes, rx->out, rx->end, &rx->w))
        elog(ERROR, "fastpostgres: result exceeds the segment");
    rx->laid_out = true;

    columnar_flush_samples(rx);
}

/* Does writing this row need a temporary context? Only a value that has to be
 * detoasted, or one whose type goes through its output function, allocates;
 * plain inline text and every fixed-width value are copied straight from the
 * slot. Avoids a memory-context switch and reset per row on the common shape. */
bool
columnar_row_needs_tmp(ColumnarReceiver *rx, Datum *vals, bool *isnull)
{
    for (int c = 0; c < rx->ncols; c++)
    {
        ColumnPlan *pl = &rx->plans[c];

        if (!pl->varlena || isnull[c]) continue;
        if (pl->typoutput != InvalidOid) return true;
        if (VARATT_IS_EXTERNAL(DatumGetPointer(vals[c])) || VARATT_IS_COMPRESSED(DatumGetPointer(vals[c])))
            return true;
    }
    return false;
}

bool
columnar_receive(TupleTableSlot *slot, DestReceiver *self)
{
    ColumnarReceiver *rx  = reinterpret_cast<ColumnarReceiver *>(self);
    MemoryContext     old = nullptr;
    bool              tmp = false;

    if (!rx->laid_out && rx->nsample < kColumnarSampleRows)
    {
        rx->samples[rx->nsample++] = columnar_slot_tuple(rx, slot);
        if (rx->nsample == kColumnarSampleRows) columnar_layout(rx, rx->want);
        return true;
    }

    if (rx->laid_out)
    {
        if (!columnar_drain_carry(rx))
        {
            /* a queued row still cannot fit: it must keep its place, so this
             * newly consumed row joins the tail and the batch ends here */
            columnar_enqueue_slot(rx, slot);
            return false;
        }
        if (rx->nrows >= rx->cap)
        {
            /* the frame is full: keep this row (it is already the executor's to
             * drop) and stop the scan, so the next request continues from it */
            columnar_enqueue_slot(rx, slot);
            return false;
        }

        slot_getallattrs(slot);
        tmp = columnar_row_needs_tmp(rx, slot->tts_values, slot->tts_isnull);
        if (tmp) old = MemoryContextSwitchTo(rx->rowctx);

        if (!columnar_write_row(rx->plans, rx->ncols, rx->nrows, slot->tts_values, slot->tts_isnull))
        {
            if (tmp) MemoryContextSwitchTo(old);
            columnar_enqueue_slot(rx, slot);
            return false;
        }
        rx->nrows++;
    }

    if (tmp)
    {
        MemoryContextSwitchTo(old);
        if (MemoryContextMemAllocated(rx->rowctx, false) > 0) MemoryContextReset(rx->rowctx);
    }
    return true;
}

void
columnar_rx_startup([[maybe_unused]] DestReceiver *self, [[maybe_unused]] int operation,
                    [[maybe_unused]] TupleDesc typeinfo)
{
}

void
columnar_rx_shutdown([[maybe_unused]] DestReceiver *self)
{
}

void
columnar_rx_destroy([[maybe_unused]] DestReceiver *self)
{
}

/* Finish the frame: shrink a partial last batch to its real row count, then
 * return the payload size. */
uint64
columnar_receiver_finish(ColumnarReceiver *rx)
{
    if (!rx->laid_out)
        columnar_layout(rx, static_cast<uint64>(rx->nsample));
    else
        columnar_drain_carry(rx);

    columnar_note_used(rx->plans, rx->ncols, rx->nrows);

    /* A full batch needs no move: reserve wrote every header where the table
     * points, so this only patches the exact data_len. The last (partial)
     * batch still compacts, which is one pass over that batch's bytes. */
    char *final;

    if (rx->nrows == rx->cap)
        final = columnar_finalize(rx->plans, rx->ncols, rx->out);
    else
        final = columnar_compact(rx->plans, rx->ncols, rx->cap, rx->nrows, rx->out);

    /* The header was written for the reservation; the frame carries the rows
     * that actually arrived. */
    fastpostgres_put_u64(rx->out + 20, rx->nrows);
    return static_cast<uint64>(final - rx->out);
}

} // namespace

uint64
fp::columnar_fetch(Portal portal, TupleDesc td, const char *path, int64 batch, size_t capacity)
{
    size_t cap    = capacity ? capacity : FASTPOSTGRES_SEGMENT_BYTES;
    size_t mapped = 0;
    void  *map    = fp::shm_map_rw(path, cap, &mapped);

    if (map == nullptr) elog(ERROR, "fastpostgres: cannot map data segment");
    char *payload = static_cast<char *>(map) + FASTPOSTGRES_SHM_HEADER_SIZE;

    ColumnarReceiver rx;

    std::memset(&rx, 0, sizeof(rx));

    rx.td    = td;
    rx.ncols = td->natts;
    rx.want  = static_cast<uint64>(batch);
    rx.out   = payload;
    rx.end   = static_cast<char *>(map) + mapped;
    rx.carry = fp::carry_for(portal, true);
    rx.plans = static_cast<ColumnPlan *>(
        palloc0(sizeof(ColumnPlan) * static_cast<size_t>(rx.ncols > 0 ? rx.ncols : 1)));
    rx.rowvals =
        static_cast<Datum *>(palloc(sizeof(Datum) * static_cast<size_t>(rx.ncols > 0 ? rx.ncols : 1)));
    rx.rownulls =
        static_cast<bool *>(palloc(sizeof(bool) * static_cast<size_t>(rx.ncols > 0 ? rx.ncols : 1)));
    rx.ctx =
        AllocSetContextCreate(CurrentMemoryContext, "fastpostgres columnar sample", ALLOCSET_SMALL_SIZES);
    rx.rowctx =
        AllocSetContextCreate(CurrentMemoryContext, "fastpostgres columnar row", ALLOCSET_SMALL_SIZES);

    [[maybe_unused]] bool any_varlena;

    columnar_setup(rx.plans, td, &any_varlena);

    /* Rows the previous batch could not take are this batch's first rows (and
     * its sample: they cost nothing to look at again). */
    uint32 len;

    while (rx.nsample < kColumnarSampleRows && fp::carry_peek(rx.carry, &len) != nullptr)
    {
        char     *row = fp::carry_peek(rx.carry, &len);
        HeapTuple tup = static_cast<HeapTuple>(palloc0(sizeof(HeapTupleData)));

        tup->t_len  = len;
        tup->t_data = reinterpret_cast<HeapTupleHeader>(palloc(len));
        std::memcpy(tup->t_data, row, len);
        rx.samples[rx.nsample++] = tup;
        fp::carry_pop(rx.carry);
    }

    rx.pub.receiveSlot = columnar_receive;
    rx.pub.rStartup    = columnar_rx_startup;
    rx.pub.rShutdown   = columnar_rx_shutdown;
    rx.pub.rDestroy    = columnar_rx_destroy;
    rx.pub.mydest      = DestNone;

    if (rx.nsample == kColumnarSampleRows) columnar_layout(&rx, rx.want);

    PortalRunFetch(portal, FETCH_FORWARD, static_cast<long>(batch), &rx.pub);

    uint64 total = columnar_receiver_finish(&rx);

    /* A batch cut short by the segment is not the end of the data, but a fetch
     * shorter than it asked for means "no more rows" to the portal
     * (PortalRunSelect sets atEnd when nprocessed < count). The executor
     * stopped between rows rather than running out, so it can be run again. */
    if (rx.carry->count > 0) portal->atEnd = false;

    fastpostgres_shm_publish(map, mapped, total);

    pfree(rx.plans);
    pfree(rx.rowvals);
    pfree(rx.rownulls);
    MemoryContextDelete(rx.ctx);
    MemoryContextDelete(rx.rowctx);
    /* the mapping is cached for the worker's lifetime: no per-batch munmap */
    return rx.nrows;
}

uint64
fp::publish_columnar_shm(const char *path, TupleDesc td, HeapTuple *tuples, uint64 nrows, size_t capacity)
{
    size_t cap    = capacity ? capacity : FASTPOSTGRES_SEGMENT_BYTES;
    size_t mapped = 0;
    void  *map    = fp::shm_map_rw(path, cap, &mapped);

    if (map == nullptr) elog(ERROR, "fastpostgres: cannot map data segment");
    uint64 total =
        fp::columnar_build(tuples, td, nrows, static_cast<char *>(map) + FASTPOSTGRES_SHM_HEADER_SIZE,
                           static_cast<char *>(map) + mapped);

    fastpostgres_shm_publish(map, mapped, total);
    /* the mapping is cached for the worker's lifetime: no per-batch munmap */
    return total;
}
