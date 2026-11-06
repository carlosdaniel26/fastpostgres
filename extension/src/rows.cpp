/*
 * rows.cpp — publish the executor's own tuple bytes as an FPROWS segment.
 *
 * The batch is fetched with this module's DestReceiver instead of
 * SPI_cursor_fetch, so no SPI tuptable is built on the way: receiveSlot()
 * copies each tuple straight out of the slot — for a plain scan that is the
 * shared buffer page itself — into the segment. One memcpy per row, no
 * per-value work, and no columnar transpose.
 *
 * The one thing a client cannot do in place is detoast, so a tuple carrying an
 * external or compressed value, or one written before an ADD COLUMN (short of
 * the descriptor's attributes), is rebuilt with its values resolved. That is
 * the slow path; the memcpy is the fast one.
 */
#include "pgxx.h"

#include <cstring>
#include <sys/mman.h>

extern "C" {
#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "access/tupmacs.h"
#include "catalog/pg_type.h"
#include "executor/tuptable.h"
#include "tcop/dest.h"
#include "tcop/pquery.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "varatt.h"

#include "fastpostgres/fastpostgres_shm.h"
}
#include "carry.h"
#include "columnar.h"
#include "rows.h"
#include "shm.h"

/* The reader mirrors these offsets and flags, so prove they are still ours. */
static_assert(offsetof(HeapTupleHeaderData, t_infomask2) == FASTPOSTGRES_HT_INFOMASK2,
              "fastpostgres: HeapTupleHeaderData moved");
static_assert(offsetof(HeapTupleHeaderData, t_infomask) == FASTPOSTGRES_HT_INFOMASK,
              "fastpostgres: HeapTupleHeaderData moved");
static_assert(offsetof(HeapTupleHeaderData, t_hoff) == FASTPOSTGRES_HT_HOFF,
              "fastpostgres: HeapTupleHeaderData moved");
static_assert(offsetof(HeapTupleHeaderData, t_bits) == FASTPOSTGRES_HT_BITS,
              "fastpostgres: HeapTupleHeaderData moved");
static_assert(HEAP_HASNULL == FASTPOSTGRES_HEAP_HASNULL, "fastpostgres: null flag moved");
static_assert(HEAP_NATTS_MASK == FASTPOSTGRES_HEAP_NATTS_MASK, "fastpostgres: natts mask moved");

/* The reader mirrors the varlena encoding, and varatt.h picks between two of
 * them by byte order: if the mirror guessed the other one, nothing it reads out
 * of a tuple would be trustworthy. */
#if defined(WORDS_BIGENDIAN)
#if !FASTPOSTGRES_BIG_ENDIAN
#error "fastpostgres: varlena mirror disagrees with PostgreSQL's byte order"
#endif
#else
#if FASTPOSTGRES_BIG_ENDIAN
#error "fastpostgres: varlena mirror disagrees with PostgreSQL's byte order"
#endif
#endif

namespace
{

struct RowReceiver {
    DestReceiver          pub;
    TupleDesc             tdesc;
    int                  *varlena; /* 0-based attribute numbers of the varlena columns */
    int                   nvarlena;
    Datum                *values;  /* the build path reuses these for every row */
    bool                 *isnull;
    int                   hoff;    /* the built tuple's data offset */
    bool                  hasnull;
    MemoryContext         tmpctx;  /* what a row needs while it is being written */
    FastpostgresRowsWrite w;
    char                 *end;
    fp::Carry            *carry; /* this portal's carried rows, if any */
};

/* Values that have to be resolved before they can be walked in place: one
 * stored out of line, or one compressed inside the tuple. VARATT_IS_EXTENDED is
 * deliberately not the test — it is also true of the short 1-byte-header form,
 * which the reader handles as it is. */
inline bool
row_needs_detoast(const char *val)
{
    return VARATT_IS_EXTERNAL(val) || VARATT_IS_COMPRESSED(val);
}

/* Can this tuple go out as it lies? An external or compressed value has to be
 * resolved, and neither can be detected from the tuple header alone
 * (compression has no header flag), so the varlena values themselves are peeked
 * at — only up to the last varlena column, and only until one needs work. A
 * short (1-byte header) value is not one of those: the reader handles both
 * header forms. */
bool
row_needs_build(RowReceiver *rx, HeapTuple tup)
{
    HeapTupleHeader th = tup->t_data;
    uint32          off;

    if (HeapTupleHeaderHasExternal(th)) return true;
    if ((th->t_infomask & HEAP_HASOID_OLD) != 0) return true; /* pre-9.2 oid attribute */
    if (HeapTupleHeaderGetNatts(th) < static_cast<uint16>(rx->tdesc->natts)) return true;
    if (rx->nvarlena == 0) return false;

    const int last = rx->varlena[rx->nvarlena - 1];
    off            = static_cast<uint32>(th->t_hoff);
    for (int i = 0; i <= last; i++)
    {
        CompactAttribute *att = TupleDescCompactAttr(rx->tdesc, i);

        if ((th->t_infomask & HEAP_HASNULL) != 0 && att_isnull(i, th->t_bits))
            continue; /* a null stores nothing, so the offset does not move */

        if (att->attlen == -1)
        {
            off = static_cast<uint32>(
                att_pointer_alignby(off, att->attalignby, -1, reinterpret_cast<char *>(th) + off));
            if (row_needs_detoast(reinterpret_cast<char *>(th) + off)) return true;
            off = static_cast<uint32>(att_addlength_pointer(off, -1, reinterpret_cast<char *>(th) + off));
        }
        else
            off = static_cast<uint32>(att_nominal_alignby(off, att->attalignby)) +
                  static_cast<uint32>(att->attlen);
    }
    return false;
}

/* Deform, detoast what is extended, and return the rebuilt tuple's length.
 *
 * The values are copied into the receiver's own arrays first: the ones handed in
 * belong to the executor (a virtual slot's values are the projection's output),
 * and resolving a value means replacing it. */
uint32
row_prepare(RowReceiver *rx, const Datum *values, const bool *isnull)
{
    TupleDesc td = rx->tdesc;

    std::memcpy(rx->values, values, sizeof(Datum) * td->natts);
    std::memcpy(rx->isnull, isnull, sizeof(bool) * td->natts);
    for (int i = 0; i < rx->nvarlena; i++)
    {
        int a = rx->varlena[i];

        if (!rx->isnull[a] && row_needs_detoast(static_cast<char *>(DatumGetPointer(rx->values[a]))))
            rx->values[a] = PointerGetDatum(
                pg_detoast_datum_packed(reinterpret_cast<struct varlena *>(DatumGetPointer(rx->values[a]))));
    }

    rx->hasnull = false;
    for (int i = 0; i < td->natts; i++)
        if (rx->isnull[i])
        {
            rx->hasnull = true;
            break;
        }

    rx->hoff = MAXALIGN(SizeofHeapTupleHeader + (rx->hasnull ? BITMAPLEN(td->natts) : 0));
    return static_cast<uint32>(rx->hoff + heap_compute_data_size(td, rx->values, rx->isnull));
}

/* Write the tuple row_prepare measured: the same header members heap_form_tuple
 * sets, then PostgreSQL's own filler for the data area. The whole row is zeroed
 * first: the header because xmin/xmax/ctid are not the reader's business, and
 * the data area because heap_fill_tuple leaves the alignment padding it inserts
 * unwritten - heap_form_tuple gets away with that only because its palloc0 has
 * already zeroed the tuple - while the reader's att_align_pointer mirror reads a
 * zero pad as "align this value". The segment's bytes are stale from the
 * previous batch, so a nonzero pad here decodes a >126-byte varlena after an
 * unaligned offset through its own 4-byte header. */
void
row_build(RowReceiver *rx, char *out, uint32 len)
{
    HeapTupleHeader th = reinterpret_cast<HeapTupleHeader>(out);

    std::memset(out, 0, static_cast<size_t>(len));
    HeapTupleHeaderSetNatts(th, static_cast<uint16>(rx->tdesc->natts));
    th->t_hoff = static_cast<uint8>(rx->hoff);
    heap_fill_tuple(rx->tdesc, rx->values, rx->isnull, out + rx->hoff, static_cast<Size>(len - rx->hoff),
                    &th->t_infomask, rx->hasnull ? th->t_bits : nullptr);
}

/* The row the last batch had no room for is in carry.c, keyed by portal name:
 * a row carried from stream A must never be written into stream B's batch, and
 * the name (unlike the Portal pointer) survives a savepoint abort. */

bool
row_receive(TupleTableSlot *slot, DestReceiver *self)
{
    RowReceiver  *rx  = reinterpret_cast<RowReceiver *>(self);
    MemoryContext old = MemoryContextSwitchTo(rx->tmpctx);
    HeapTuple     tup = nullptr;
    char         *row;
    uint32        len;
    bool          ok = true;

    /* A slot holding a heap tuple already in the shape the reader expects costs
     * one memcpy — straight out of the buffer page, for a plain scan. A slot
     * without one (virtual and minimal slots both answer NULL here, so a
     * projection or a tuplestore) or one whose values need resolving writes its
     * values instead: materialising a heap tuple first would copy every value
     * twice, and allocate for the privilege. */
    if (slot->tts_ops->get_heap_tuple != nullptr) tup = ExecFetchSlotHeapTuple(slot, false, nullptr);

    if (tup != nullptr && !row_needs_build(rx, tup))
        len = static_cast<uint32>(tup->t_len);
    else
    {
        slot_getallattrs(slot); /* a no-op for a virtual slot */
        len = row_prepare(rx, slot->tts_values, slot->tts_isnull);
        tup = nullptr;          /* row_build writes this one */
    }

    row = fastpostgres_rows_reserve(&rx->w, len, rx->end);
    if (row != nullptr)
    {
        if (tup != nullptr)
            std::memcpy(row, tup->t_data, static_cast<size_t>(len));
        else
            row_build(rx, row, len);
    }
    else if (rx->w.nrows == 0)
        elog(ERROR, "fastpostgres: result row exceeds the segment");
    else
    {
        /* The batch is full. The tuple is already the executor's to drop, so
         * keep it (the next batch writes it first) and stop the scan here. */
        if (tup != nullptr)
            fp::carry_put(rx->carry, reinterpret_cast<const char *>(tup->t_data), len);
        else
        {
            char *tmp = static_cast<char *>(palloc(len));

            row_build(rx, tmp, len);
            fp::carry_put(rx->carry, tmp, len);
        }
        ok = false; /* the batch is full: the client asks again for the rest */
    }

    MemoryContextSwitchTo(old);
    MemoryContextReset(rx->tmpctx);
    return ok;
}

void
row_startup([[maybe_unused]] DestReceiver *self, [[maybe_unused]] int operation,
            [[maybe_unused]] TupleDesc typeinfo)
{
}

void
row_shutdown([[maybe_unused]] DestReceiver *self)
{
}

void
row_destroy([[maybe_unused]] DestReceiver *self)
{
}

} // namespace

bool
fp::rows_native(TupleDesc td)
{
    for (int i = 0; i < td->natts; i++)
    {
        CompactAttribute *att   = TupleDescCompactAttr(td, i);
        Oid               typ   = TupleDescAttr(td, i)->atttypid;
        uint8_t           kind  = fastpostgres_oid_to_kind(static_cast<uint32>(typ));
        int               width = fastpostgres_kind_width(kind);

        if (att->attisdropped) return false;

        if (width > 0)
        {
            /* the reader reads attlen bytes at the rule's alignment; that is
             * what the walk uses, by-value or not (uuid is a by-reference
             * fixed-width type, and it is read the same way) */
            if (att->attlen != width) return false;
            if (att->attalignby != fastpostgres_kind_alignby(kind)) return false;
        }
        else
        {
            /* varlena: the reader slices text/bytea bytes out of the tuple.
             * numeric, jsonb, arrays, name, money and the rest need their
             * output function, so those go out as the columnar frame. */
            if (att->attlen != -1) return false;
            if (att->attalignby != fastpostgres_kind_alignby(kind)) return false;
            if (kind == FASTPOSTGRES_KIND_TEXT && !fp::is_text_type(typ)) return false;
            if (kind == FASTPOSTGRES_KIND_BYTEA && typ != BYTEAOID) return false;
        }
    }
    return true;
}

uint64
fp::rows_fetch(Portal portal, TupleDesc td, const char *path, int64 batch, size_t capacity)
{
    size_t cap    = capacity ? capacity : FASTPOSTGRES_SEGMENT_BYTES;
    size_t mapped = 0;
    void  *map    = fp::shm_map_rw(path, cap, &mapped);
    char  *payload;

    if (map == nullptr) elog(ERROR, "fastpostgres: cannot map data segment");
    payload = static_cast<char *>(map) + FASTPOSTGRES_SHM_HEADER_SIZE;

    RowReceiver            rx{};
    FastpostgresRowsWrite *w     = &rx.w;
    fp::Carry             *carry = fp::carry_for(portal, true);
    uint32                 len;

    rx.carry   = carry;
    rx.tdesc   = td;
    rx.end     = static_cast<char *>(map) + mapped;
    rx.tmpctx  = AllocSetContextCreate(CurrentMemoryContext, "fastpostgres_rows_fetch", ALLOCSET_SMALL_SIZES);
    rx.varlena = static_cast<int *>(palloc(sizeof(int) * td->natts));
    rx.values  = static_cast<Datum *>(palloc(sizeof(Datum) * td->natts));
    rx.isnull  = static_cast<bool *>(palloc(sizeof(bool) * td->natts));
    for (int i = 0; i < td->natts; i++)
        if (TupleDescCompactAttr(td, i)->attlen == -1) rx.varlena[rx.nvarlena++] = i;

    fastpostgres_rows_write_begin(w, payload, static_cast<uint32>(td->natts));
    for (int i = 0; i < td->natts; i++)
    {
        Form_pg_attribute att  = TupleDescAttr(td, i);
        const char       *name = NameStr(att->attname);

        fastpostgres_rows_write_column(w, name, static_cast<uint32>(std::strlen(name)),
                                       fastpostgres_oid_to_kind(static_cast<uint32>(att->atttypid)),
                                       static_cast<uint32>(att->atttypid));
    }
    fastpostgres_rows_write_rows_begin(w);

    rx.pub.receiveSlot = row_receive;
    rx.pub.rStartup    = row_startup;
    rx.pub.rShutdown   = row_shutdown;
    rx.pub.rDestroy    = row_destroy;
    rx.pub.mydest      = DestNone;

    /* a row the previous batch had no room for goes first, in the slot it would
     * have had: the batch that carried it stopped one row short of the data */
    while (fp::carry_peek(carry, &len) != nullptr)
    {
        char *carried = fp::carry_peek(carry, &len);
        char *row     = fastpostgres_rows_reserve(w, len, rx.end);

        if (row == nullptr) elog(ERROR, "fastpostgres: result row exceeds the segment");
        std::memcpy(row, carried, len);
        fp::carry_pop(carry);
    }

    PortalRunFetch(portal, FETCH_FORWARD, static_cast<long>(batch), &rx.pub);

    /* A batch cut short by the segment is not the end of the data, but a fetch
     * shorter than it asked for means "no more rows" to the portal
     * (PortalRunSelect sets atEnd when nprocessed < count), which would end the
     * stream with the carried row and the rest still to come. The executor
     * stopped between rows rather than running out, so it can be run again. */
    if (carry->count > 0) portal->atEnd = false;

    fastpostgres_rows_write_end(w);
    fastpostgres_shm_publish(map, mapped, static_cast<uint64>(w->p - payload));

    pfree(rx.varlena);
    pfree(rx.values);
    pfree(rx.isnull);
    MemoryContextDelete(rx.tmpctx);
    /* the mapping is cached for the worker's lifetime: no per-batch munmap */
    return w->nrows; /* the rows the batch carries, carried row included */
}
