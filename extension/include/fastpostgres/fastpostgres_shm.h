/* fastpostgres_shm.h — shared-memory result-segment format, agreed between the
 * PostgreSQL extension (writer) and the client (reader).
 *
 * Segment layout:
 *   [ FastpostgresShmHeader (64 bytes) ][ FPCOLUMN payload (header + columns) ]
 *
 * This is the single source of truth for the format: the extension, the client
 * and the benchmark generator all include this one header, and it carries the
 * codec helpers so the FPCOLUMN wire format lives in one place.
 *
 * The writer fills the payload, then publishes payload_size and bumps seq with
 * a memory barrier; the reader checks seq before/after copying the payload to
 * detect a concurrent write (seqlock-style).
 */
#ifndef FASTPOSTGRES_SHM_H
#define FASTPOSTGRES_SHM_H

#include <stdint.h>
#include <string.h>

#define FASTPOSTGRES_SHM_MAGIC       "FPQSHMEM"
#define FASTPOSTGRES_SHM_VERSION     1
#define FASTPOSTGRES_SHM_HEADER_SIZE 64

/* Default capacity for a data segment (lazily faulted: only what is written
 * costs memory). A client may ask for less - FastpostgresRing.data_bytes - as
 * long as it clears the header, a frame's own header and one row. */
#define FASTPOSTGRES_SEGMENT_BYTES (1024u * 1024u * 1024u)
#define FASTPOSTGRES_SEGMENT_MIN   (64u * 1024u)

typedef struct FastpostgresShmHeader {
    char     magic[8];     /* "FPQSHMEM" */
    uint32_t version;      /* 1 */
    uint32_t flags;        /* bit0: 1 = little-endian */
    uint64_t payload_size; /* valid FPCOLUMN bytes at offset FASTPOSTGRES_SHM_HEADER_SIZE */
    uint64_t capacity;     /* total mapped bytes */
    uint64_t seq;          /* bumped after each completed write */
    uint32_t reserved[6];  /* pad to 64 bytes */
} FastpostgresShmHeader;

#ifdef __cplusplus
static_assert(sizeof(FastpostgresShmHeader) == 64, "FastpostgresShmHeader must be 64 bytes");
#endif

/* FPCOLUMN payload:
 *   magic[8] "FPCOLUMN", version u32, flags u32, ncols u32, nrows u64,
 *   columns_off u64
 *   at columns_off: ncols × u64, the absolute offset of each column's header
 *   per column: name_len u32, name, kind u8, pgtype u32, nulls bitmap, then
 *   data_len u64 and data: fixed-width values, or (text) an int32 offset table
 *   followed by the raw bytes.
 *
 * Each column's data is padded to 8-byte alignment relative to the payload
 * base, so the client can view it in place as a typed array.
 *
 * v3 replaced the sequential walk with the offset table: a column header may
 * sit after a gap left by a reservation, so the writer never has to move data
 * to make the frame look compact. data_len is still exact and the reader still
 * sees one contiguous region per column.
 */
#define FASTPOSTGRES_ALIGN8(x)      (((uint64_t)(x) + 7) & ~(uint64_t)7)
#define FASTPOSTGRES_COLUMNAR_MAGIC "FPCOLUMN"
/* 2: the binary kinds (date/time/timestamp/timestamptz/uuid/bytea). A reader
 * that does not know them must refuse the payload rather than guess — the frame
 * is checked against this number, so v1 and v2 cannot be mixed silently.
 * 3: the column offset table (columns_off); the reader no longer relies on the
 * previous column's data_len to find the next header. */
#define FASTPOSTGRES_COLUMNAR_VERSION 3

/* Bytes the frame itself occupies: magic, version, flags, ncols, nrows, and the
 * offset of the column table. */
#define FASTPOSTGRES_COLUMNAR_FRAME (8u + 4u + 4u + 4u + 8u + 8u)

/* Bytes the ncols-entry header-offset table occupies, aligned to 8. */
#define FASTPOSTGRES_COLUMNAR_TABLE(ncols) FASTPOSTGRES_ALIGN8((uint64_t)(ncols) * 8)

/* ---------------------------------------------------------------------------
 * Wire type helpers — format plumbing, kept out of the codec's core logic.
 *
 * Each column is tagged with a compact `kind` (below) rather than a raw
 * PostgreSQL OID, so the client can decode without a type table. The mapping
 * (OID -> kind) and the widths live here, together, in one place.
 * ------------------------------------------------------------------------- */

enum FastpostgresKind
{
    FASTPOSTGRES_KIND_BOOL        = 1,
    FASTPOSTGRES_KIND_INT32       = 2,
    FASTPOSTGRES_KIND_INT64       = 3,
    FASTPOSTGRES_KIND_FLOAT8      = 4,
    FASTPOSTGRES_KIND_TEXT        = 5,
    FASTPOSTGRES_KIND_INT16       = 6,  /* verbatim 2-byte integer (int2) */
    FASTPOSTGRES_KIND_FLOAT4      = 7,  /* verbatim 4-byte float (float4) */
    FASTPOSTGRES_KIND_DATE        = 8,  /* int32 DateADT: days since 2000-01-01 */
    FASTPOSTGRES_KIND_TIME        = 9,  /* int64 TimeADT: microseconds since midnight */
    FASTPOSTGRES_KIND_TIMESTAMP   = 10, /* int64 Timestamp: microseconds since 2000-01-01 */
    FASTPOSTGRES_KIND_TIMESTAMPTZ = 11, /* the same, always UTC */
    FASTPOSTGRES_KIND_UUID        = 12, /* 16 raw bytes */
    FASTPOSTGRES_KIND_BYTEA       = 13  /* offset table + raw bytes, like text */
};

/* Kinds that carry an int32 offset table followed by raw bytes. */
static inline int
fastpostgres_kind_is_varlena(uint8_t kind)
{
    return kind == FASTPOSTGRES_KIND_TEXT || kind == FASTPOSTGRES_KIND_BYTEA;
}

/* PostgreSQL builtin type OIDs. Bootstrap-assigned, hence fixed for a given
 * PostgreSQL major version — pinned here so both sides share one mapping. */
#define FASTPOSTGRES_OID_BOOL        16
#define FASTPOSTGRES_OID_BYTEA       17
#define FASTPOSTGRES_OID_CHAR        18
#define FASTPOSTGRES_OID_NAME        19
#define FASTPOSTGRES_OID_INT8        20
#define FASTPOSTGRES_OID_INT2        21
#define FASTPOSTGRES_OID_INT4        23
#define FASTPOSTGRES_OID_TEXT        25
#define FASTPOSTGRES_OID_OID         26
#define FASTPOSTGRES_OID_JSON        114
#define FASTPOSTGRES_OID_FLOAT4      700
#define FASTPOSTGRES_OID_FLOAT8      701
#define FASTPOSTGRES_OID_MONEY       790
#define FASTPOSTGRES_OID_BPCHAR      1042
#define FASTPOSTGRES_OID_VARCHAR     1043
#define FASTPOSTGRES_OID_DATE        1082
#define FASTPOSTGRES_OID_TIME        1083
#define FASTPOSTGRES_OID_TIMESTAMP   1114
#define FASTPOSTGRES_OID_TIMESTAMPTZ 1184
#define FASTPOSTGRES_OID_NUMERIC     1700
#define FASTPOSTGRES_OID_UUID        2950
#define FASTPOSTGRES_OID_JSONB       3802

/* PostgreSQL type OID -> wire kind. The binary kinds are the point of the
 * exercise: the extension already holds these values in binary (a DateADT, a
 * Timestamp, a pg_uuid_t, the raw bytea), so shipping them as binary leaves the
 * client wrapping them in a Python object instead of parsing digits and hex. */
static inline uint8_t
fastpostgres_oid_to_kind(uint32_t oid)
{
    switch (oid)
    {
        case FASTPOSTGRES_OID_BOOL:
            return FASTPOSTGRES_KIND_BOOL;
        case FASTPOSTGRES_OID_INT2:
            return FASTPOSTGRES_KIND_INT16; /* copied verbatim, not widened */
        case FASTPOSTGRES_OID_INT4:
            return FASTPOSTGRES_KIND_INT32;
        case FASTPOSTGRES_OID_INT8:
            return FASTPOSTGRES_KIND_INT64;
        case FASTPOSTGRES_OID_FLOAT4:
            return FASTPOSTGRES_KIND_FLOAT4; /* copied verbatim, not widened */
        case FASTPOSTGRES_OID_FLOAT8:
        case FASTPOSTGRES_OID_NUMERIC:       /* numeric has no wire kind: converted, lossily */
            return FASTPOSTGRES_KIND_FLOAT8;
        case FASTPOSTGRES_OID_DATE:
            return FASTPOSTGRES_KIND_DATE;
        case FASTPOSTGRES_OID_TIME:
            return FASTPOSTGRES_KIND_TIME;
        case FASTPOSTGRES_OID_TIMESTAMP:
            return FASTPOSTGRES_KIND_TIMESTAMP;
        case FASTPOSTGRES_OID_TIMESTAMPTZ:
            return FASTPOSTGRES_KIND_TIMESTAMPTZ;
        case FASTPOSTGRES_OID_UUID:
            return FASTPOSTGRES_KIND_UUID;
        case FASTPOSTGRES_OID_BYTEA:
            return FASTPOSTGRES_KIND_BYTEA;
        default:
            return FASTPOSTGRES_KIND_TEXT; /* varlena text, or an output-function fallback */
    }
}

/* Width of a wire kind in bytes (0 for the varlena-shaped kinds). */
static inline int
fastpostgres_kind_width(uint8_t kind)
{
    switch (kind)
    {
        case FASTPOSTGRES_KIND_BOOL:
            return 1;
        case FASTPOSTGRES_KIND_INT16:
            return 2;
        case FASTPOSTGRES_KIND_INT32:
            return 4;
        case FASTPOSTGRES_KIND_FLOAT4:
            return 4;
        case FASTPOSTGRES_KIND_DATE:
            return 4;
        case FASTPOSTGRES_KIND_INT64:
            return 8;
        case FASTPOSTGRES_KIND_FLOAT8:
            return 8;
        case FASTPOSTGRES_KIND_TIME:
            return 8;
        case FASTPOSTGRES_KIND_TIMESTAMP:
            return 8;
        case FASTPOSTGRES_KIND_TIMESTAMPTZ:
            return 8;
        case FASTPOSTGRES_KIND_UUID:
            return 16;
        default:
            return 0;
    }
}

/* Alignment a kind demands in a tuple. Must equal the type's typalign for every
 * attribute the row segment carries, and the writer checks that it does. */
static inline int
fastpostgres_kind_alignby(uint8_t kind)
{
    switch (kind)
    {
        case FASTPOSTGRES_KIND_BOOL:
        case FASTPOSTGRES_KIND_UUID:
            return 1;
        case FASTPOSTGRES_KIND_INT16:
            return 2;
        case FASTPOSTGRES_KIND_INT32:
        case FASTPOSTGRES_KIND_FLOAT4:
        case FASTPOSTGRES_KIND_DATE:
        case FASTPOSTGRES_KIND_TEXT: /* varlena: the 4-byte alignment, with the
                                      * 1-byte-header form exempted */
        case FASTPOSTGRES_KIND_BYTEA:
            return 4;
        case FASTPOSTGRES_KIND_INT64:
        case FASTPOSTGRES_KIND_FLOAT8:
        case FASTPOSTGRES_KIND_TIME:
        case FASTPOSTGRES_KIND_TIMESTAMP:
        case FASTPOSTGRES_KIND_TIMESTAMPTZ:
            return 8;
        default:
            return 0;
    }
}

/* little-endian scalar readers/writers */
static inline void
fastpostgres_put_u32(char *p, uint32_t v)
{
    memcpy(p, &v, 4);
}
static inline void
fastpostgres_put_u64(char *p, uint64_t v)
{
    memcpy(p, &v, 8);
}
static inline uint32_t
fastpostgres_rd_u32(const uint8_t **p)
{
    uint32_t v;
    memcpy(&v, *p, 4);
    *p += 4;
    return v;
}
static inline uint64_t
fastpostgres_rd_u64(const uint8_t **p)
{
    uint64_t v;
    memcpy(&v, *p, 8);
    *p += 8;
    return v;
}

/* Native-order scalar read: the tuple bytes are PostgreSQL's own layout, which
 * is why the varlena headers below are read in host order rather than as the
 * little-endian scalars the frames use. */
static inline uint32_t
fastpostgres_peek_u32(const uint8_t *p)
{
    uint32_t v;

    memcpy(&v, p, 4);
    return v;
}

/* One decoded column. `data` points at the value bytes; for text it is the
 * int32 offset table and `bytes` the raw payload. `name` is not NUL-terminated
 * (use name_len). */
typedef struct FastpostgresColumn {
    const char    *name;
    uint32_t       name_len;
    uint8_t        kind;
    uint32_t       pgtype;
    const uint8_t *nulls;
    const uint8_t *data;
    const uint8_t *bytes;
    uint64_t       data_len;
    uint64_t       nrows;
} FastpostgresColumn;

typedef struct FastpostgresReader {
    const uint8_t *base;  /* payload base; column data is aligned relative to it */
    const uint8_t *p;     /* end of the column just decoded */
    const uint8_t *end;
    const uint8_t *table; /* v3: ncols absolute header offsets */
    uint32_t       next;  /* v3: the column reader_next will return */
    uint32_t       ncols;
    uint64_t       nrows;
} FastpostgresReader;

/* Point a reader at an FPCOLUMN payload; returns 0 if the magic or the version
 * is not ours. */
static inline int
fastpostgres_reader_init(FastpostgresReader *r, const void *payload, size_t len)
{
    const uint8_t *p = (const uint8_t *)payload;
    uint32_t       version;
    uint64_t       columns_off;

    if (len < FASTPOSTGRES_COLUMNAR_FRAME || memcmp(p, FASTPOSTGRES_COLUMNAR_MAGIC, 8) != 0) return 0;
    r->base = p;
    r->p    = p + 8;
    r->end  = p + len;
    version = fastpostgres_rd_u32(&r->p);
    if (version != FASTPOSTGRES_COLUMNAR_VERSION) return 0;
    (void)fastpostgres_rd_u32(&r->p); /* flags */
    r->ncols = fastpostgres_rd_u32(&r->p);
    r->nrows = fastpostgres_rd_u64(&r->p);
    columns_off = fastpostgres_rd_u64(&r->p);
    if (columns_off > len || (uint64_t)r->ncols * 8 > len - columns_off) return 0;
    r->table = p + columns_off;
    r->next  = 0;
    return 1;
}

/* Decode the next column; returns 0 at the end of the payload. The table entry
 * is the authority for where the header starts, so reservations may leave gaps
 * between columns without the reader knowing or caring. */
static inline int
fastpostgres_reader_next(FastpostgresReader *r, FastpostgresColumn *c)
{
    const uint8_t *p;
    uint64_t       off;

    if (r->next >= r->ncols) return 0;
    memcpy(&off, r->table + (size_t)r->next * 8, 8);
    if (off >= (uint64_t)(r->end - r->base)) return 0;
    p = r->base + off;

    if (p + 9 > r->end) return 0;
    c->name_len = fastpostgres_rd_u32(&p);
    c->name     = (const char *)p;
    p += c->name_len;
    if (p + 5 > r->end) return 0;
    c->kind   = *p++;
    c->pgtype = fastpostgres_rd_u32(&p);
    c->nulls  = p;
    p += (r->nrows + 7) / 8;
    if (p + 8 > r->end) return 0;
    c->data_len = fastpostgres_rd_u64(&p);
    p           = r->base + FASTPOSTGRES_ALIGN8((uint64_t)(p - r->base)); /* data is 8-byte aligned */
    c->data     = p;
    c->bytes    = fastpostgres_kind_is_varlena(c->kind) ? p + (r->nrows + 1) * 4 : NULL;
    p += c->data_len;
    if (p > r->end) return 0;
    c->nrows = r->nrows;
    r->p     = p;
    r->next++;
    return 1;
}

static inline int
fastpostgres_col_isnull(const FastpostgresColumn *c, uint64_t row)
{
    return (c->nulls[row >> 3] & (1u << (row & 7))) != 0;
}

/* 1 when any bit is set in the first nbytes: a column with no nulls skips every
 * per-row null test. The scan lives in the header both sides include, so the
 * writer and the reader ask the same question of the same bytes. */
static inline int
fastpostgres_nullmap_any(const uint8_t *bits, size_t nbytes)
{
    size_t i;

    for (i = 0; i < nbytes; i++)
        if (bits[i] != 0) return 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Row segments (FPROWS) — the result tuples, as PostgreSQL packed them.
 *
 * FPCOLUMN is the client's column-oriented product (flat arrays, zero-copy
 * views, a transpose on the server). FPROWS is the row-oriented one: the worker
 * copies each result tuple's bytes into the segment — one memcpy per row and no
 * per-value work at all — and the client walks that tuple layout in place, so
 * the only copy left is the one into the Python object.
 *
 * The reader has no PostgreSQL headers to include, so the layout rules it needs
 * are mirrored here (from access/tupmacs.h, access/htup_details.h, varatt.h)
 * and the writer asserts them against PostgreSQL's own macros at build time.
 * Only inline, uncompressed values are ever written: the worker detoasts what
 * arrives extended, because the client could not do it in place.
 *
 * Payload:
 *   "FPROWS\0\0", version u32, flags u32, ncols u32, nrows u64, rows_off u64,
 *   per column: name_len u32, name, kind u8, pgtype u32,
 *   at rows_off: per row a len u32, 4 bytes of padding, the tuple bytes, padded
 *   to 8. The padding is what keeps a tuple MAXALIGNed: heap_fill_tuple() aligns
 *   a datum by its address, not by its offset, so a tuple that does not start on
 *   an 8-byte boundary would be written with a layout heap_deform_tuple() could
 *   not read back.
 * ------------------------------------------------------------------------- */
#define FASTPOSTGRES_ROWS_MAGIC   "FPROWS\0\0"
#define FASTPOSTGRES_ROWS_VERSION 1
#define FASTPOSTGRES_ROWS_FRAME   (8u + 4u + 4u + 4u + 8u + 8u)

/* HeapTupleHeaderData fields, by offset from the tuple's first byte. */
#define FASTPOSTGRES_HT_INFOMASK2    18u
#define FASTPOSTGRES_HT_INFOMASK     20u
#define FASTPOSTGRES_HT_HOFF         22u
#define FASTPOSTGRES_HT_BITS         23u /* offsetof(HeapTupleHeaderData, t_bits) */
#define FASTPOSTGRES_HEAP_HASNULL    0x0001u
#define FASTPOSTGRES_HEAP_NATTS_MASK 0x07FFu

static inline uint32_t
fastpostgres_tuple_natts(const uint8_t *t)
{
    uint16_t v;

    memcpy(&v, t + FASTPOSTGRES_HT_INFOMASK2, 2);
    return (uint32_t)(v & FASTPOSTGRES_HEAP_NATTS_MASK);
}

static inline int
fastpostgres_tuple_hasnull(const uint8_t *t)
{
    uint16_t v;

    memcpy(&v, t + FASTPOSTGRES_HT_INFOMASK, 2);
    return (v & FASTPOSTGRES_HEAP_HASNULL) != 0;
}

static inline uint32_t
fastpostgres_tuple_hoff(const uint8_t *t)
{
    return (uint32_t)t[FASTPOSTGRES_HT_HOFF];
}

/* The null bitmap lives at t_bits, right after t_hoff — heap_form_tuple() sizes
 * the header as MAXALIGN(t_bits + BITMAPLEN(natts)) and fills the bitmap from
 * t_bits, so the padding sits *after* the bitmap, not before it. The bits mark
 * the attributes that are *not* null (fill_val sets one for every value it
 * writes), which is why PostgreSQL's own att_isnull() negates the test. */
static inline int
fastpostgres_tuple_isnull(const uint8_t *t, uint32_t att)
{
    return (t[FASTPOSTGRES_HT_BITS + (att >> 3)] & (1u << (att & 7))) == 0;
}

/* varlena headers (varatt.h). PostgreSQL encodes the 1-byte form differently on
 * big- and little-endian hosts — the tag bit is the low bit on little-endian,
 * the high bit on big-endian — and these bytes are PostgreSQL's own, laid out by
 * the server it will be read on, so the mirror follows the host's byte order.
 * The writer resolves every external or compressed value before it leaves, so
 * the reader only ever sees the plain 4-byte form and the short 1-byte one. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define FASTPOSTGRES_BIG_ENDIAN 1
#else
#define FASTPOSTGRES_BIG_ENDIAN 0
#endif

#if FASTPOSTGRES_BIG_ENDIAN
#define FASTPOSTGRES_VARATT_IS_1B(p)   (((*(const uint8_t *)(p)) & 0x80) == 0x80)
#define FASTPOSTGRES_VARATT_IS_1B_E(p) ((*(const uint8_t *)(p)) == 0x80)
#define FASTPOSTGRES_VARSIZE_4B(p)     (fastpostgres_peek_u32(p) & 0x3FFFFFFFu)
#define FASTPOSTGRES_VARSIZE_1B(p)     ((uint32_t)((*(const uint8_t *)(p)) & 0x7Fu))
#else
#define FASTPOSTGRES_VARATT_IS_1B(p)   (((*(const uint8_t *)(p)) & 0x01) == 0x01)
#define FASTPOSTGRES_VARATT_IS_1B_E(p) ((*(const uint8_t *)(p)) == 0x01)
#define FASTPOSTGRES_VARSIZE_4B(p)     (fastpostgres_peek_u32(p) >> 2)
#define FASTPOSTGRES_VARSIZE_1B(p)     ((uint32_t)(*(const uint8_t *)(p)) >> 1)
#endif

static inline uint32_t
fastpostgres_varsize_any(const uint8_t *p)
{
    if (FASTPOSTGRES_VARATT_IS_1B(p)) return FASTPOSTGRES_VARSIZE_1B(p);
    return FASTPOSTGRES_VARSIZE_4B(p);
}

/* Where the value's bytes start: one byte into a short header, four into a full
 * one (VARDATA_ANY). */
static inline uint32_t
fastpostgres_vardata_off(const uint8_t *p)
{
    return FASTPOSTGRES_VARATT_IS_1B(p) ? 1u : 4u;
}

static inline const uint8_t *
fastpostgres_vardata_any(const uint8_t *p)
{
    return p + fastpostgres_vardata_off(p);
}

/* The value's bytes, excluding its header (VARSIZE_ANY_EXHDR). */
static inline uint32_t
fastpostgres_vardata_len(const uint8_t *p)
{
    return fastpostgres_varsize_any(p) - fastpostgres_vardata_off(p);
}

/* att_pointer_alignby() / att_addlength_pointer() (access/tupmacs.h): a value
 * whose first byte is non-zero is a 1-byte header (or a 4-byte header whose
 * length byte is set, which the writer already aligned), so it is not aligned
 * again — a pad byte is always zero. */
static inline uint32_t
fastpostgres_att_align(uint32_t off, uint32_t alignby)
{
    return (off + alignby - 1) & ~(alignby - 1);
}

static inline uint32_t
fastpostgres_att_pointer_align(uint32_t off, uint32_t alignby, const uint8_t *ptr)
{
    if (*ptr != 0) return off;
    return fastpostgres_att_align(off, alignby);
}

static inline uint32_t
fastpostgres_att_addlength(uint32_t off, int32_t attlen, const uint8_t *ptr)
{
    if (attlen > 0) return off + (uint32_t)attlen;
    return off + fastpostgres_varsize_any(ptr);
}

/* The column descriptors: same {name, kind, pgtype} triple as FPCOLUMN, so the
 * client's codecs read either frame. The layout fields are unused here. */
typedef struct FastpostgresRowsDesc {
    const uint8_t *p;
    const uint8_t *end;
} FastpostgresRowsDesc;

static inline void
fastpostgres_rows_desc_begin(FastpostgresRowsDesc *d, const uint8_t *base, uint64_t rows_off)
{
    d->p   = base + FASTPOSTGRES_ROWS_FRAME;
    d->end = base + rows_off;
}

static inline int
fastpostgres_rows_desc_next(FastpostgresRowsDesc *d, FastpostgresColumn *c)
{
    const uint8_t *p = d->p;

    if (p + 9 > d->end) return 0;
    c->name_len = fastpostgres_rd_u32(&p);
    c->name     = (const char *)p;
    p += c->name_len;
    if (p + 5 > d->end) return 0;
    c->kind   = *p++;
    c->pgtype = fastpostgres_rd_u32(&p);
    c->nulls = c->data = c->bytes = NULL;
    c->data_len = c->nrows = 0;
    d->p                   = p;
    return 1;
}

typedef struct FastpostgresRowsReader {
    const uint8_t *base;
    const uint8_t *p; /* the row about to be read */
    const uint8_t *end;
    uint64_t       left;
    uint32_t       ncols;
} FastpostgresRowsReader;

static inline int
fastpostgres_rows_init(FastpostgresRowsReader *r, const void *payload, size_t len)
{
    const uint8_t *p = (const uint8_t *)payload;
    uint64_t       rows_off;

    if (len < FASTPOSTGRES_ROWS_FRAME || memcmp(p, FASTPOSTGRES_ROWS_MAGIC, 8) != 0) return 0;
    p += 8;
    if (fastpostgres_rd_u32(&p) != FASTPOSTGRES_ROWS_VERSION) return 0;
    (void)fastpostgres_rd_u32(&p); /* flags */
    r->ncols = fastpostgres_rd_u32(&p);
    r->left  = fastpostgres_rd_u64(&p);
    rows_off = fastpostgres_rd_u64(&p);
    if (rows_off > len) return 0;
    r->base = (const uint8_t *)payload;
    r->p    = r->base + rows_off;
    r->end  = r->base + len;
    return 1;
}

/* The next tuple in the segment; 0 at the end. The bytes are not copied: the
 * caller walks them where they lie, MAXALIGNed as PostgreSQL laid them out. */
static inline int
fastpostgres_rows_next(FastpostgresRowsReader *r, const uint8_t **row, uint32_t *len)
{
    const uint8_t *p = r->p;

    if (r->left == 0 || p + 8 > r->end) return 0;
    *len = fastpostgres_rd_u32(&p);
    p += 4; /* padding */
    if (p + FASTPOSTGRES_ALIGN8(*len) > r->end) return 0;
    *row = p;
    r->p = p + FASTPOSTGRES_ALIGN8(*len);
    r->left--;
    return 1;
}

/* Writer, the counterpart of the reader above (the frames are written by the
 * extension, never by the client). */
typedef struct FastpostgresRowsWrite {
    char    *base;
    char    *p;
    uint64_t nrows;
} FastpostgresRowsWrite;

static inline void
fastpostgres_rows_write_begin(FastpostgresRowsWrite *w, char *out, uint32_t ncols)
{
    char *p = out;

    memcpy(p, FASTPOSTGRES_ROWS_MAGIC, 8);
    p += 8;
    fastpostgres_put_u32(p, FASTPOSTGRES_ROWS_VERSION);
    p += 4;
    fastpostgres_put_u32(p, 1); /* flags: little-endian */
    p += 4;
    fastpostgres_put_u32(p, ncols);
    p += 4;
    fastpostgres_put_u64(p, 0); /* nrows, backpatched by write_end */
    p += 8;
    fastpostgres_put_u64(p, 0); /* rows_off, patched by rows_begin */
    p += 8;

    w->base  = out;
    w->p     = p;
    w->nrows = 0;
}

static inline void
fastpostgres_rows_write_column(FastpostgresRowsWrite *w, const char *name, uint32_t name_len, uint8_t kind,
                               uint32_t pgtype)
{
    char *p = w->p;

    fastpostgres_put_u32(p, name_len);
    p += 4;
    memcpy(p, name, name_len);
    p += name_len;
    *p++ = (char)kind;
    fastpostgres_put_u32(p, pgtype);
    p += 4;
    w->p = p;
}

/* Align the row area and record its offset; returns where the rows start. */
static inline char *
fastpostgres_rows_write_rows_begin(FastpostgresRowsWrite *w)
{
    uint64_t off = (uint64_t)(w->p - w->base);

    while (((uint64_t)(w->p - w->base)) & 7)
        *w->p++ = 0;
    fastpostgres_put_u64(w->base + 28, FASTPOSTGRES_ALIGN8(off));
    return w->p;
}

static inline void
fastpostgres_rows_write_end(FastpostgresRowsWrite *w)
{
    fastpostgres_put_u64(w->base + 20, w->nrows);
}

/* Reserve one row for `len` bytes of tuple data; NULL when it does not fit, so
 * the caller can end the batch instead of the query. The tuple starts at the
 * next 8-byte boundary after the length word. */
static inline char *
fastpostgres_rows_reserve(FastpostgresRowsWrite *w, uint32_t len, const char *end)
{
    char *row = w->p + 8;

    if (row + FASTPOSTGRES_ALIGN8(len) > end) return NULL;
    fastpostgres_put_u32(w->p, len);
    fastpostgres_put_u32(w->p + 4, 0); /* padding: the tuple is MAXALIGNed */
    w->p = row + FASTPOSTGRES_ALIGN8(len);
    w->nrows++;
    return row;
}

/* ---------------------------------------------------------------------------
 * FPREDUCE — an aggregate result, computed by the worker and never the rows.
 *
 * A reduce request names a plan (op + column) and the worker accumulates over
 * the fetched batch itself; what lands in the segment is one slot per op, not
 * the batch. The frame is tiny (32 + 16*nops bytes) and identical on both
 * sides, like the columnar and row frames above.
 *
 * Payload:
 *   magic[8] "FPREDUCE", version u32, flags u32, nops u32, pad u32, nrows u64,
 *   per op: is_null u8, is_float u8, pad[2], value u64 (int64 or double bits).
 *
 * nrows is the batch's row count, so COUNT_ALL needs no slot of its own; the
 * slot is still emitted (the client's plan is positional). `is_null` is what
 * an empty MIN/MAX carries; SUM/COUNT write their identity instead.
 * ------------------------------------------------------------------------- */
#define FASTPOSTGRES_REDUCE_MAGIC   "FPREDUCE"
#define FASTPOSTGRES_REDUCE_VERSION 1
#define FASTPOSTGRES_REDUCE_FRAME   (8u + 4u + 4u + 4u + 4u + 8u)
#define FASTPOSTGRES_REDUCE_SLOT    12u /* 4-byte header + 8-byte value */
#define FASTPOSTGRES_REDUCE_MAX_OPS 64

typedef struct FastpostgresReduceSlot {
    uint8_t  is_null;
    uint8_t  is_float;
    uint64_t bits; /* int64 bits, or the IEEE double's bits */
} FastpostgresReduceSlot;

typedef struct FastpostgresReduceReader {
    const uint8_t *p;
    const uint8_t *end;
    uint32_t       nops;
    uint64_t       nrows;
} FastpostgresReduceReader;

static inline int
fastpostgres_reduce_init(FastpostgresReduceReader *r, const void *payload, size_t len)
{
    const uint8_t *p = (const uint8_t *)payload;

    if (len < FASTPOSTGRES_REDUCE_FRAME || memcmp(p, FASTPOSTGRES_REDUCE_MAGIC, 8) != 0) return 0;
    p += 8;
    if (fastpostgres_rd_u32(&p) != FASTPOSTGRES_REDUCE_VERSION) return 0;
    (void)fastpostgres_rd_u32(&p); /* flags */
    r->nops = fastpostgres_rd_u32(&p);
    (void)fastpostgres_rd_u32(&p); /* pad */
    r->nrows = fastpostgres_rd_u64(&p);
    r->end   = (const uint8_t *)payload + len;
    if (r->nops > FASTPOSTGRES_REDUCE_MAX_OPS ||
        (uint64_t)(r->end - p) < (uint64_t)r->nops * FASTPOSTGRES_REDUCE_SLOT)
        return 0;
    r->p = p;
    return 1;
}

static inline int
fastpostgres_reduce_next(FastpostgresReduceReader *r, FastpostgresReduceSlot *s)
{
    if (r->p == NULL || (uint64_t)(r->end - r->p) < FASTPOSTGRES_REDUCE_SLOT) return 0;
    s->is_null  = *r->p++;
    s->is_float = *r->p++;
    r->p += 2; /* pad */
    memcpy(&s->bits, r->p, 8);
    r->p += 8;
    return 1;
}

/* ---------------------------------------------------------------------------
 * FPCOLUMN writer — the frame emitted by the extension (from tuples) and by the
 * client's generators (from synthetic arrays). Both fill only the column data;
 * the layout, the padding and the backpatched data_len live here once. The
 * frame/table sizes are macros beside the version, above.
 * ------------------------------------------------------------------------- */

typedef struct FastpostgresColumnarWrite {
    char    *base;  /* payload base (8-byte aligned) */
    char    *p;     /* current write position */
    char    *table; /* v3: ncols absolute header offsets, filled by write_column */
    char    *nulls; /* the column being written: its null bitmap, for the caller to fill */
    char    *lenp;  /* the column being written: its data_len, backpatched by write_end */
    uint32_t ncols;
    uint32_t written;
    uint64_t nrows;
} FastpostgresColumnarWrite;

/* Offset of the next column's header, given the current offset, this column's
 * frame and its data length — mirrors the padding write_column inserts. */
static inline uint64_t
fastpostgres_columnar_next_offset(uint64_t off, uint32_t name_len, uint32_t null_bytes, uint64_t data_len)
{
    return FASTPOSTGRES_ALIGN8(off + 4 + (uint64_t)name_len + 1 + 4 + null_bytes + 8) + data_len;
}

static inline void
fastpostgres_columnar_write_begin(FastpostgresColumnarWrite *w, char *out, uint32_t ncols, uint64_t nrows)
{
    char *p = out;

    memcpy(p, FASTPOSTGRES_COLUMNAR_MAGIC, 8);
    p += 8;
    fastpostgres_put_u32(p, FASTPOSTGRES_COLUMNAR_VERSION);
    p += 4;
    fastpostgres_put_u32(p, 1); /* flags: little-endian */
    p += 4;
    fastpostgres_put_u32(p, ncols);
    p += 4;
    fastpostgres_put_u64(p, nrows);
    p += 8;
    fastpostgres_put_u64(p, (uint64_t)(p - out) + 8); /* columns_off: the table follows */
    p += 8;

    w->base    = out;
    w->table   = p;
    w->ncols   = ncols;
    w->written = 0;
    w->nrows   = nrows;
    w->nulls   = NULL;
    w->lenp    = NULL;
    memset(p, 0, (size_t)FASTPOSTGRES_COLUMNAR_TABLE(ncols));
    w->p = out + FASTPOSTGRES_COLUMNAR_FRAME + FASTPOSTGRES_COLUMNAR_TABLE(ncols);
}

/* Write one column's frame; returns its 8-byte aligned data pointer and points
 * `w->nulls` at the bitmap for the caller to fill. Finish with write_end(). */
static inline char *
fastpostgres_columnar_write_column(FastpostgresColumnarWrite *w, const char *name, uint32_t name_len,
                                   uint8_t kind, uint32_t pgtype, uint32_t null_bytes)
{
    char *p = w->p;

    if (w->written < w->ncols)
        fastpostgres_put_u64(w->table + (size_t)w->written * 8, (uint64_t)(p - w->base));
    w->written++;

    fastpostgres_put_u32(p, name_len);
    p += 4;
    memcpy(p, name, name_len);
    p += name_len;
    *p++ = (char)kind;
    fastpostgres_put_u32(p, pgtype);
    p += 4;
    w->nulls = p;
    memset(p, 0, null_bytes);
    p += null_bytes;
    w->lenp = p;
    p += 8;       /* data_len, backpatched by write_end */
    while (((size_t)(p - w->base)) & 7)
        *p++ = 0; /* 8-byte aligned column data */
    w->p = p;
    return p;
}

/* Backpatch the current column's data_len and advance past its data. */
static inline void
fastpostgres_columnar_write_end(FastpostgresColumnarWrite *w, uint64_t data_len)
{
    fastpostgres_put_u64(w->lenp, data_len);
    w->p += data_len;
}

/* Fill in the segment header and publish it with a release barrier. */
static inline void
fastpostgres_shm_publish(void *map, uint64_t capacity, uint64_t payload_size)
{
    FastpostgresShmHeader *h = (FastpostgresShmHeader *)map;
    memcpy(h->magic, FASTPOSTGRES_SHM_MAGIC, 8);
    h->version  = FASTPOSTGRES_SHM_VERSION;
    h->flags    = 1; /* little-endian */
    h->capacity = capacity;
    __sync_synchronize();
    h->payload_size = payload_size;
    __sync_synchronize();
    h->seq++;
}

/* Validate a mapped segment header; returns payload size via *payload_size. */
static inline int
fastpostgres_shm_validate(const void *map, size_t size, uint64_t *payload_size)
{
    const FastpostgresShmHeader *h = (const FastpostgresShmHeader *)map;
    if (size < FASTPOSTGRES_SHM_HEADER_SIZE || memcmp(h->magic, FASTPOSTGRES_SHM_MAGIC, 8) != 0) return 0;
    *payload_size = h->payload_size;
    return 1;
}

#endif
