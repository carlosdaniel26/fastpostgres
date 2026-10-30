/*
 * write.c — ingest a columnar segment (FPCOLUMN) into a table via
 * table_multi_insert (the COPY fast path: no per-row SQL, no text round-trip).
 * Driven by the ring worker (worker.c).
 */
#include "postgres.h"

#include <string.h>
#include <sys/mman.h>

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/tupdesc.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/tuptable.h"
#include "nodes/makefuncs.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "fastpostgres_shm.h"

#include "shm.h"
#include "write.h"

typedef struct WCol {
    int            kind;
    Oid            atttypid;
    const uint8_t *nulls;
    const uint8_t *data;  /* fixed-width values, or offsets for text */
    const uint8_t *bytes; /* text bytes (NULL for fixed-width) */
} WCol;

static Datum col_datum(WCol *col, uint64_t r, bool *isnull, char **arena)
{
    *isnull = (col->nulls[r >> 3] & (1 << (r & 7))) != 0;
    if (*isnull) return (Datum)0;

    switch (col->kind)
    {
        case FASTPOSTGRES_KIND_BOOL:
            return BoolGetDatum(col->data[r] != 0);

        case FASTPOSTGRES_KIND_INT16: {
            int16 v;
            memcpy(&v, col->data + r * 2, 2);
            switch (col->atttypid)
            {
                case INT4OID:
                    return Int32GetDatum((int32)v);
                case INT8OID:
                    return Int64GetDatum((int64)v);
                case FLOAT4OID:
                    return Float4GetDatum((float4)v);
                case FLOAT8OID:
                    return Float8GetDatum((double)v);
                default:
                    return Int16GetDatum(v);
            }
        }

        case FASTPOSTGRES_KIND_FLOAT4: {
            float4 v;
            memcpy(&v, col->data + r * 4, 4);
            switch (col->atttypid)
            {
                case INT2OID:
                    return Int16GetDatum((int16)v);
                case INT4OID:
                    return Int32GetDatum((int32)v);
                case INT8OID:
                    return Int64GetDatum((int64)v);
                case FLOAT8OID:
                    return Float8GetDatum((double)v);
                default:
                    return Float4GetDatum(v);
            }
        }

        case FASTPOSTGRES_KIND_INT32: {
            int32 v;
            memcpy(&v, col->data + r * 4, 4);
            switch (col->atttypid)
            {
                case INT2OID:
                    return Int16GetDatum((int16)v);
                case INT8OID:
                    return Int64GetDatum((int64)v);
                case FLOAT4OID:
                    return Float4GetDatum((float4)v);
                case FLOAT8OID:
                    return Float8GetDatum((double)v);
                default:
                    return Int32GetDatum(v);
            }
        }

        case FASTPOSTGRES_KIND_INT64: {
            int64 v;
            memcpy(&v, col->data + r * 8, 8);
            switch (col->atttypid)
            {
                case INT2OID:
                    return Int16GetDatum((int16)v);
                case INT4OID:
                    return Int32GetDatum((int32)v);
                case FLOAT4OID:
                    return Float4GetDatum((float4)v);
                case FLOAT8OID:
                    return Float8GetDatum((double)v);
                default:
                    return Int64GetDatum(v);
            }
        }

        case FASTPOSTGRES_KIND_FLOAT8: {
            double v;
            memcpy(&v, col->data + r * 8, 8);
            switch (col->atttypid)
            {
                case INT2OID:
                    return Int16GetDatum((int16)v);
                case INT4OID:
                    return Int32GetDatum((int32)v);
                case INT8OID:
                    return Int64GetDatum((int64)v);
                case FLOAT4OID:
                    return Float4GetDatum((float4)v);
                default:
                    return Float8GetDatum(v);
            }
        }

        case FASTPOSTGRES_KIND_TEXT: {
            const uint32_t *off = (const uint32_t *)col->data;
            uint32_t        s = off[r], e = off[r + 1];
            size_t          len = (size_t)(e - s);
            text           *t   = (text *)*arena; /* bump-allocate from the batch arena */

            SET_VARSIZE(t, len + VARHDRSZ);
            memcpy(VARDATA(t), col->bytes + s, len);
            *arena += MAXALIGN(len + VARHDRSZ);
            return PointerGetDatum(t);
        }

        default:
            return (Datum)0;
    }
}

uint64 fastpostgres_write_segment(const char *seg, const char *tab)
{
    char              *path = fastpostgres_shm_path(seg);
    size_t             size;
    void              *map = fastpostgres_shm_map_ro(path, "fastpostgres_write_segment", &size);
    uint64_t           payload_len;
    FastpostgresReader rd;
    FastpostgresColumn col;
    uint32_t           ncols;
    uint64_t           nrows;
    WCol              *wcols;
    Relation           rel;
    TupleDesc          tupdesc;
    CommandId          cid;
    BulkInsertState    bistate;
    TupleTableSlot   **slots;
    MemoryContext      batch_ctx;
    const int          BATCH = 1000;
    uint64_t           start;
    uint32_t           c;

    if (!fastpostgres_shm_validate(map, size, &payload_len) ||
        !fastpostgres_reader_init(&rd, (const uint8_t *)map + FASTPOSTGRES_SHM_HEADER_SIZE, payload_len))
    {
        munmap(map, size);
        elog(ERROR, "fastpostgres_write_segment: bad segment");
    }
    ncols = rd.ncols;
    nrows = rd.nrows;

    rel     = table_openrv(makeRangeVar(NULL, unconstify(char *, tab), -1), RowExclusiveLock);
    tupdesc = RelationGetDescr(rel);
    if (tupdesc->natts != (int)ncols)
    {
        table_close(rel, RowExclusiveLock);
        munmap(map, size);
        elog(ERROR, "fastpostgres_write_segment: column count mismatch (%d vs %u)", tupdesc->natts, ncols);
    }

    wcols = palloc0(sizeof(WCol) * ncols);
    for (c = 0; c < ncols; c++)
    {
        if (!fastpostgres_reader_next(&rd, &col))
        {
            table_close(rel, RowExclusiveLock);
            munmap(map, size);
            elog(ERROR, "fastpostgres_write_segment: truncated columnar payload");
        }
        wcols[c].kind     = (int)col.kind;
        wcols[c].nulls    = col.nulls;
        wcols[c].data     = col.data;
        wcols[c].bytes    = col.bytes;
        wcols[c].atttypid = TupleDescAttr(tupdesc, c)->atttypid;
    }

    cid   = GetCurrentCommandId(false);
    slots = palloc(sizeof(TupleTableSlot *) * BATCH);
    for (c = 0; c < (uint32)BATCH; c++)
        slots[c] = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
    batch_ctx =
        AllocSetContextCreate(CurrentMemoryContext, "fastpostgres_write_segment", ALLOCSET_DEFAULT_SIZES);
    bistate = GetBulkInsertState();

    for (start = 0; start < nrows; start += BATCH)
    {
        int           n          = (int)Min((uint64)BATCH, nrows - start);
        MemoryContext old        = MemoryContextSwitchTo(batch_ctx);
        size_t        text_bytes = 0, ntext = 0;
        char         *arena, *arena_p;
        int           i;

        /* one allocation for every text datum in this batch */
        for (c = 0; c < ncols; c++)
        {
            if (wcols[c].kind == FASTPOSTGRES_KIND_TEXT)
            {
                const uint32_t *off = (const uint32_t *)wcols[c].data;
                text_bytes += (size_t)(off[start + n] - off[start]);
                ntext += (size_t)n;
            }
        }
        arena   = MemoryContextAlloc(batch_ctx, text_bytes + ntext * (VARHDRSZ + MAXIMUM_ALIGNOF));
        arena_p = arena;

        for (i = 0; i < n; i++)
        {
            uint64_t        r    = start + i;
            TupleTableSlot *slot = slots[i];

            ExecClearTuple(slot);
            for (c = 0; c < ncols; c++)
                slot->tts_values[c] = col_datum(&wcols[c], r, &slot->tts_isnull[c], &arena_p);
            ExecStoreVirtualTuple(slot);
        }
        table_multi_insert(rel, slots, n, cid, 0, bistate);
        MemoryContextSwitchTo(old);
        MemoryContextReset(batch_ctx);
    }

    FreeBulkInsertState(bistate);
    for (c = 0; c < (uint32)BATCH; c++)
        ExecDropSingleTupleTableSlot(slots[c]);

    table_close(rel, RowExclusiveLock);
    pfree(wcols);
    pfree(slots);
    MemoryContextDelete(batch_ctx);
    munmap(map, size);

    return nrows;
}
