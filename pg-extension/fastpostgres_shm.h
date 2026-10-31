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

/* Fixed capacity for a data segment (lazily faulted: only what is written
 * costs memory). */
#define FASTPOSTGRES_SEGMENT_BYTES (1024u * 1024u * 1024u)

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
 *   magic[8] "FPCOLUMN", version u32, flags u32, ncols u32, nrows u64
 *   per column: name_len u32, name, kind u8, pgtype u32, nulls bitmap, then
 *   data_len u64 and data: fixed-width values, or (text) an int32 offset table
 *   followed by the raw bytes.
 *
 * Each column's data is padded to 8-byte alignment relative to the payload
 * base, so the client can view it in place as a typed array.
 */
#define FASTPOSTGRES_ALIGN8(x)        (((uint64_t)(x) + 7) & ~(uint64_t)7)
#define FASTPOSTGRES_COLUMNAR_MAGIC   "FPCOLUMN"
/* 2: the binary kinds (date/time/timestamp/timestamptz/uuid/bytea). A reader
 * that does not know them must refuse the payload rather than guess — the frame
 * is checked against this number, so v1 and v2 cannot be mixed silently. */
#define FASTPOSTGRES_COLUMNAR_VERSION 2

/* ---------------------------------------------------------------------------
 * Wire type helpers — format plumbing, kept out of the codec's core logic.
 *
 * Each column is tagged with a compact `kind` (below) rather than a raw
 * PostgreSQL OID, so the client can decode without a type table. The mapping
 * (OID -> kind) and the widths live here, together, in one place.
 * ------------------------------------------------------------------------- */

enum FastpostgresKind
{
    FASTPOSTGRES_KIND_BOOL   = 1,
    FASTPOSTGRES_KIND_INT32  = 2,
    FASTPOSTGRES_KIND_INT64  = 3,
    FASTPOSTGRES_KIND_FLOAT8 = 4,
    FASTPOSTGRES_KIND_TEXT   = 5,
    FASTPOSTGRES_KIND_INT16  = 6,  /* verbatim 2-byte integer (int2) */
    FASTPOSTGRES_KIND_FLOAT4 = 7,  /* verbatim 4-byte float (float4) */
    FASTPOSTGRES_KIND_DATE        = 8,  /* int32 DateADT: days since 2000-01-01 */
    FASTPOSTGRES_KIND_TIME        = 9,  /* int64 TimeADT: microseconds since midnight */
    FASTPOSTGRES_KIND_TIMESTAMP   = 10, /* int64 Timestamp: microseconds since 2000-01-01 */
    FASTPOSTGRES_KIND_TIMESTAMPTZ = 11, /* the same, always UTC */
    FASTPOSTGRES_KIND_UUID        = 12, /* 16 raw bytes */
    FASTPOSTGRES_KIND_BYTEA       = 13  /* offset table + raw bytes, like text */
};

/* Kinds that carry an int32 offset table followed by raw bytes. */
static inline int fastpostgres_kind_is_varlena(uint8_t kind)
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
static inline uint8_t fastpostgres_oid_to_kind(uint32_t oid)
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
        case FASTPOSTGRES_OID_NUMERIC: /* numeric has no wire kind: converted, lossily */
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
static inline int fastpostgres_kind_width(uint8_t kind)
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

/* little-endian scalar readers/writers */
static inline void fastpostgres_put_u32(char *p, uint32_t v)
{
    memcpy(p, &v, 4);
}
static inline void fastpostgres_put_u64(char *p, uint64_t v)
{
    memcpy(p, &v, 8);
}
static inline uint32_t fastpostgres_rd_u32(const uint8_t **p)
{
    uint32_t v;
    memcpy(&v, *p, 4);
    *p += 4;
    return v;
}
static inline uint64_t fastpostgres_rd_u64(const uint8_t **p)
{
    uint64_t v;
    memcpy(&v, *p, 8);
    *p += 8;
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
    const uint8_t *base; /* payload base; column data is aligned relative to it */
    const uint8_t *p;
    const uint8_t *end;
    uint32_t       ncols;
    uint64_t       nrows;
} FastpostgresReader;

/* Point a reader at an FPCOLUMN payload; returns 0 if the magic or the version
 * is not ours. */
static inline int fastpostgres_reader_init(FastpostgresReader *r, const void *payload, size_t len)
{
    const uint8_t *p = (const uint8_t *)payload;
    uint32_t       version;

    if (len < 8 || memcmp(p, FASTPOSTGRES_COLUMNAR_MAGIC, 8) != 0) return 0;
    r->base = p;
    r->p    = p + 8;
    r->end  = p + len;
    version = fastpostgres_rd_u32(&r->p);
    if (version != FASTPOSTGRES_COLUMNAR_VERSION) return 0;
    (void)fastpostgres_rd_u32(&r->p); /* flags */
    r->ncols = fastpostgres_rd_u32(&r->p);
    r->nrows = fastpostgres_rd_u64(&r->p);
    return 1;
}

/* Decode the next column; returns 0 at the end of the payload. */
static inline int fastpostgres_reader_next(FastpostgresReader *r, FastpostgresColumn *c)
{
    const uint8_t *p = r->p;
    if (p == NULL || p >= r->end) return 0;
    c->name_len = fastpostgres_rd_u32(&p);
    c->name     = (const char *)p;
    p += c->name_len;
    c->kind   = *p++;
    c->pgtype = fastpostgres_rd_u32(&p);
    c->nulls  = p;
    p += (r->nrows + 7) / 8;
    c->data_len = fastpostgres_rd_u64(&p);
    p           = r->base + FASTPOSTGRES_ALIGN8((uint64_t)(p - r->base)); /* data is 8-byte aligned */
    c->data     = p;
    c->bytes    = fastpostgres_kind_is_varlena(c->kind) ? p + (r->nrows + 1) * 4 : NULL;
    p += c->data_len;
    if (p > r->end) return 0;
    c->nrows = r->nrows;
    r->p     = p;
    return 1;
}

static inline int fastpostgres_col_isnull(const FastpostgresColumn *c, uint64_t row)
{
    return (c->nulls[row >> 3] & (1u << (row & 7))) != 0;
}

/* 1 when any bit is set in the first nbytes: a column with no nulls skips every
 * per-row null test. The scan lives in the header both sides include, so the
 * writer and the reader ask the same question of the same bytes. */
static inline int fastpostgres_nullmap_any(const uint8_t *bits, size_t nbytes)
{
    size_t i;

    for (i = 0; i < nbytes; i++)
        if (bits[i] != 0) return 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * FPCOLUMN writer — the frame emitted by the extension (from tuples) and by the
 * client's generators (from synthetic arrays). Both fill only the column data;
 * the layout, the padding and the backpatched data_len live here once.
 * ------------------------------------------------------------------------- */

/* Bytes the frame itself occupies: magic, version, flags, ncols, nrows. */
#define FASTPOSTGRES_COLUMNAR_FRAME (8u + 4u + 4u + 4u + 8u)

typedef struct FastpostgresColumnarWrite {
    char    *base;  /* payload base (8-byte aligned) */
    char    *p;     /* current write position */
    char    *nulls; /* the column being written: its null bitmap, for the caller to fill */
    char    *lenp;  /* the column being written: its data_len, backpatched by write_end */
    uint32_t ncols;
    uint64_t nrows;
} FastpostgresColumnarWrite;

/* Offset of the next column's header, given the current offset, this column's
 * frame and its data length — mirrors the padding write_column inserts. */
static inline uint64_t fastpostgres_columnar_next_offset(uint64_t off, uint32_t name_len, uint32_t null_bytes,
                                                         uint64_t data_len)
{
    return FASTPOSTGRES_ALIGN8(off + 4 + (uint64_t)name_len + 1 + 4 + null_bytes + 8) + data_len;
}

static inline void fastpostgres_columnar_write_begin(FastpostgresColumnarWrite *w, char *out, uint32_t ncols,
                                                     uint64_t nrows)
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

    w->base  = out;
    w->p     = p;
    w->nulls = NULL;
    w->lenp  = NULL;
    w->ncols = ncols;
    w->nrows = nrows;
}

/* Write one column's frame; returns its 8-byte aligned data pointer and points
 * `w->nulls` at the bitmap for the caller to fill. Finish with write_end(). */
static inline char *fastpostgres_columnar_write_column(FastpostgresColumnarWrite *w, const char *name,
                                                       uint32_t name_len, uint8_t kind, uint32_t pgtype,
                                                       uint32_t null_bytes)
{
    char *p = w->p;

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
    p += 8; /* data_len, backpatched by write_end */
    while (((size_t)(p - w->base)) & 7)
        *p++ = 0; /* 8-byte aligned column data */
    w->p = p;
    return p;
}

/* Backpatch the current column's data_len and advance past its data. */
static inline void fastpostgres_columnar_write_end(FastpostgresColumnarWrite *w, uint64_t data_len)
{
    fastpostgres_put_u64(w->lenp, data_len);
    w->p += data_len;
}

/* Fill in the segment header and publish it with a release barrier. */
static inline void fastpostgres_shm_publish(void *map, uint64_t capacity, uint64_t payload_size)
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
static inline int fastpostgres_shm_validate(const void *map, size_t size, uint64_t *payload_size)
{
    const FastpostgresShmHeader *h = (const FastpostgresShmHeader *)map;
    if (size < FASTPOSTGRES_SHM_HEADER_SIZE || memcmp(h->magic, FASTPOSTGRES_SHM_MAGIC, 8) != 0) return 0;
    *payload_size = h->payload_size;
    return 1;
}

#endif
