/* fpg_codec.cpp — dataset, the two FPG frames, and the zero-copy reads.
 *
 * FPCOLUMN is the shipping frame: it is built with the writer helpers in
 * extension/fastpostgres_shm.h (the ones extension/src/columnar.c uses
 * on the server) and read back with the reader helpers in the same header (the
 * ones the client includes). Nothing in this file invents a byte of the wire
 * format; it only fills in the values, so the payload is provably what
 * fastpostgres_reader_* parses.
 *
 * FPROWS is the optional row frame: the row area holds PostgreSQL-shaped tuple
 * bytes, synthesized here the way extension/src/rows.c writes them (23-byte
 * HeapTupleHeaderData, data area laid out by each attribute's alignment) and
 * walked in place the way the client's fp_decode_tuples() walks a real one.
 * The text values carry the plain 4-byte varlena header, placed on the
 * attribute's 4-byte alignment, which is the form the client's mirror reads.
 *
 * Decode is zero-copy in both frames: the readers compute pointers into the
 * payload and the aggregates read the values through those pointers. There is
 * no per-value allocation and no per-value object; the only typed loads are
 * the scalar reads that produce the sums, and text is hashed/measured where
 * it lies.
 */
#include "fpg_codec.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__x86_64__)
#include <immintrin.h> /* _mm256_stream_si256, _mm_sfence: copy_nt's streamed stores */
#endif

namespace fpg
{
namespace
{

[[noreturn]] void
fail(const char *what)
{
    std::fprintf(stderr, "fpg_codec: %s\n", what);
    std::exit(2);
}

/* The dataset's columns as they go on the wire, in frame order. `kind` is the
 * wire kind (fastpostgres_shm.h), `pgtype` the PostgreSQL OID the column would
 * carry from the server. */
struct ColumnSpec {
    const char *name;
    uint32_t    name_len;
    uint8_t     kind;
    uint32_t    pgtype;
};

constexpr ColumnSpec kColumns[kDatasetColumns] = {
    {"id", 2, FASTPOSTGRES_KIND_INT32, FASTPOSTGRES_OID_INT4},
    {"sku", 3, FASTPOSTGRES_KIND_TEXT, FASTPOSTGRES_OID_TEXT},
    {"name", 4, FASTPOSTGRES_KIND_TEXT, FASTPOSTGRES_OID_TEXT},
    {"price", 5, FASTPOSTGRES_KIND_FLOAT8, FASTPOSTGRES_OID_FLOAT8},
    {"stock", 5, FASTPOSTGRES_KIND_INT32, FASTPOSTGRES_OID_INT4},
    {"active", 6, FASTPOSTGRES_KIND_BOOL, FASTPOSTGRES_OID_BOOL},
};

/* HEAP_HASVARWIDTH (access/htup_details.h): the synthesized tuples carry
 * varlena attributes, so the honest tuple header sets this bit. The client's
 * walker does not read it (it only asks HEAP_HASNULL), but the bytes should be
 * what a real tuple would be. */
constexpr uint16_t kHeapHasVarwidth = 0x0002;

/* ---- typed reads of aliased payload bytes --------------------------------
 *
 * The readers hand back `const uint8_t *` into the payload, and the timed
 * aggregate reads scalars through those pointers. may_alias tells the compiler
 * the char bytes may be viewed as int32/uint64/double; build_fpg.sh also
 * passes -fno-strict-aliasing, so the read is a plain load from the aliased
 * payload and never a copy. (Column data is 8-byte aligned relative to the
 * payload base in both frames; the harness allocates the buffer 64-byte
 * aligned, and /dev/shm places the payload at offset 64 of a page.) */
typedef uint32_t fpu32 __attribute__((may_alias));
typedef int32_t  fpi32 __attribute__((may_alias));
typedef uint64_t fpu64 __attribute__((may_alias));
typedef double   fpf64 __attribute__((may_alias));

inline int32_t
rd_i32(const uint8_t *p)
{
    return *reinterpret_cast<const fpi32 *>(p);
}

inline uint64_t
rd_u32(const uint8_t *p)
{
    return *reinterpret_cast<const fpu32 *>(p);
}

inline double
rd_f64(const uint8_t *p)
{
    return *reinterpret_cast<const fpf64 *>(p);
}

inline uint64_t
rd_u64(const uint8_t *p)
{
    return *reinterpret_cast<const fpu64 *>(p);
}

/* FNV-1a 64: the checksum's text_hash (SPEC.md). The hash state is threaded
 * through one byte-at-a-time loop over the aliased text bytes. */
constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

inline uint64_t
fnv1a(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = static_cast<const uint8_t *>(p);

    for (size_t i = 0; i < n; i++)
    {
        h ^= b[i];
        h *= kFnvPrime;
    }
    return h;
}

inline uint64_t
fnv1a_u32le(uint64_t h, uint32_t v)
{
    const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};

    return fnv1a(h, b, 4);
}

/* The frame must be the dataset this cell claims to measure: the same columns,
 * in the same order, with the same wire kinds. A frame that fails this is a
 * bug, not a measurement. */
bool
frame_matches_dataset(const FpView &v)
{
    if (v.ncols != (uint32_t)kDatasetColumns) return false;
    for (uint32_t c = 0; c < (uint32_t)kDatasetColumns; c++)
    {
        const ColumnSpec &s = kColumns[c];

        if (v.cols[c].name_len != s.name_len) return false;
        if (std::memcmp(v.cols[c].name, s.name, s.name_len) != 0) return false;
        if (v.cols[c].kind != s.kind || v.cols[c].pgtype != s.pgtype) return false;
    }
    return true;
}

/* ---- FPROWS tuple layout -------------------------------------------------
 *
 * One row's tuple, by offset from the tuple's first byte. The header is
 * HeapTupleHeaderData without a null bitmap (the dataset has no NULLs):
 * t_infomask2 at 18, t_infomask at 20, t_hoff at 22, data at
 * MAXALIGN(23) = 24. Each value then sits at its attribute alignment:
 * int32 -> 4, float8 -> 8, bool -> 1, and text -> a 4-byte varlena header at a
 * 4-byte boundary. The client's walk applies the same rule (att_align for the
 * fixed widths, att_align_pointer for the varlena kinds), so the bytes decode
 * where they were written. */
struct RowLayout {
    uint32_t hoff;
    uint32_t id;
    uint32_t sku;
    uint32_t name;
    uint32_t price;
    uint32_t stock;
    uint32_t active;
    uint32_t total;
};

inline RowLayout
row_layout(const Dataset &ds, uint64_t r)
{
    RowLayout l{};
    uint32_t  l1  = ds.sku_off[r + 1] - ds.sku_off[r];
    uint32_t  l2  = ds.name_off[r + 1] - ds.name_off[r];
    uint32_t  off = 24; /* MAXALIGN(sizeof(HeapTupleHeaderData)) */

    l.hoff = off;
    l.id   = off;
    off += 4; /* off is 4-aligned here: every varlena starts where it should */
    l.sku = off;
    off += 4 + l1;
    off    = (off + 3) & ~3u;
    l.name = off;
    off += 4 + l2;
    off     = (off + 7) & ~7u;
    l.price = off;
    off += 8;
    off     = (off + 3) & ~3u;
    l.stock = off;
    off += 4;
    l.active = off;
    off += 1;
    l.total = off;
    return l;
}

} /* namespace */

/* --------------------------------------------------------------------------
 * Dataset
 * ------------------------------------------------------------------------ */

Dataset
generate(uint64_t rows)
{
    Dataset ds;

    ds.rows = rows;
    ds.id.resize(rows);
    ds.stock.resize(rows);
    ds.price.resize(rows);
    ds.active.resize(rows);
    ds.sku_off.resize(rows + 1);
    ds.name_off.resize(rows + 1);
    ds.sku.reserve((size_t)rows * 12 + 32);
    ds.name.reserve((size_t)rows * 16 + 32);
    ds.sku_off[0]  = 0;
    ds.name_off[0] = 0;

    for (uint64_t g = 1; g <= rows; g++)
    {
        char     buf[32];
        uint64_t r  = g - 1;
        int      n1 = std::snprintf(buf, sizeof(buf), "SKU-%" PRIu64, g);

        ds.sku.insert(ds.sku.end(), buf, buf + n1);
        ds.sku_off[r + 1] = (uint32_t)ds.sku.size();

        int n2 = std::snprintf(buf, sizeof(buf), "Product %" PRIu64, g);

        ds.name.insert(ds.name.end(), buf, buf + n2);
        ds.name_off[r + 1] = (uint32_t)ds.name.size();

        ds.id[r]     = (int32_t)g;
        ds.stock[r]  = (int32_t)(g % 500);
        ds.price[r]  = (double)g * 0.5;
        ds.active[r] = (uint8_t)((g % 2) == 0);
    }
    return ds;
}

/* --------------------------------------------------------------------------
 * FPCOLUMN — the shipping columnar frame
 * ------------------------------------------------------------------------ */

namespace
{

uint64_t
column_data_len(const Dataset &ds, int c)
{
    const uint64_t n = ds.rows;

    switch (c)
    {
        case kColId:
        case kColStock:
            return n * 4;
        case kColPrice:
            return n * 8;
        case kColActive:
            return n;
        case kColSku:
            return (n + 1) * 4 + ds.sku.size();
        case kColName:
            return (n + 1) * 4 + ds.name.size();
        default:
            fail("unknown dataset column");
    }
}

/* ---- the bulk copy into the payload: memcpy, or streaming stores ----------
 *
 * fpcolumn_encode moves the dataset's contiguous arrays straight into the
 * payload. Ordinary stores make their destination lines a read-for-ownership
 * first, so a 49.5 MB column frame moves ~99 MB through DRAM and the encode is
 * bandwidth-bound. Streaming stores write whole lines without that read and
 * cost one fence per copy.
 *
 * The column data pointer is 8-byte aligned (FASTPOSTGRES_ALIGN8), not 32, so
 * the copy is a head/interior/tail move: ordinary stores up to the first
 * 32-byte boundary in the destination, _mm256_stream_si256 for the aligned
 * interior, ordinary stores for the trailing < 32 bytes. Exactly `len` bytes
 * are written and each of them is the byte std::memcpy would have written: only
 * the store instruction differs, which is what makes the flagged and unflagged
 * payloads (checksum included) identical.
 */

#if defined(__x86_64__)

/* Short copies are a handful of cache lines, which the caches move more cheaply
 * than the streaming path pays for its fence. */
constexpr size_t kNtStoreMinBytes = 256;

/* AVX — not baseline SSE2 — is what carries the 32-byte store, and the build
 * passes no -march (build_fpg.sh), so the intrinsic lives in a function with a
 * target attribute and is only reached once the CPU has reported the feature.
 * x86-64 only; every other target gets copy_nt's memcpy. */
__attribute__((target("avx"))) void
stream_copy_avx(char *dst, const void *src, size_t len)
{
    const char *s    = static_cast<const char *>(src);
    size_t      head = (size_t)((32u - ((uintptr_t)dst & 31u)) & 31u); /* up to the first whole line */

    if (head > len) head = len;
    std::memcpy(dst, s, head);

    size_t i = head;
    for (; i + 32 <= len; i += 32)
        _mm256_stream_si256(reinterpret_cast<__m256i *>(dst + i),
                            _mm256_loadu_si256(reinterpret_cast<const __m256i *>(s + i)));
    if (i < len) std::memcpy(dst + i, s + i, len - i);

    /* Streamed stores are weakly ordered: the fence drains them so that whoever
     * reads the payload next — the decode that follows the encode, or
     * fpg_bench's --emit-payload file write — sees the bytes complete. (SFENCE
     * is SSE; the _mm256_sfence spelling some toolchains carried is gone from
     * GCC 12 on, and it was the same instruction.) */
    _mm_sfence();
}

inline bool
avx_available()
{
#if defined(__clang__)
    __builtin_cpu_init(); /* clang does not populate its feature table by itself */
#endif
    static const bool ok = __builtin_cpu_supports("avx") != 0;

    return ok;
}

#endif /* __x86_64__ */

/* The encoder's bulk copy, for the copies long enough to be worth streaming:
 * streaming stores where the CPU has them, std::memcpy otherwise (and on every
 * target that is not x86-64). */
void
copy_nt(char *dst, const void *src, size_t len)
{
#if defined(__x86_64__)
    if (len >= kNtStoreMinBytes && avx_available())
    {
        stream_copy_avx(dst, src, len);
        return;
    }
#endif
    std::memcpy(dst, src, len);
}

} /* namespace */

uint64_t
fpcolumn_capacity(const Dataset &ds)
{
    const uint32_t null_bytes = (uint32_t)((ds.rows + 7) / 8);
    uint64_t       off        = FASTPOSTGRES_COLUMNAR_FRAME;

    for (int c = 0; c < kDatasetColumns; c++)
        off =
            fastpostgres_columnar_next_offset(off, kColumns[c].name_len, null_bytes, column_data_len(ds, c));
    return off;
}

/* fpcolumn_encode with the store instruction selectable: `nt_store` routes
 * every bulk copy of column data through copy_nt() (streaming stores on
 * x86-64), `false` is the shipping behaviour — std::memcpy throughout. The
 * payload is the same bytes either way, so payload_bytes and the checksum do
 * not move. fpg_bench.cpp declares this overload; fpg_codec.h is untouched. */
uint64_t
fpcolumn_encode(const Dataset &ds, char *out, uint64_t capacity, bool nt_store)
{
    const uint64_t            n          = ds.rows;
    const uint32_t            null_bytes = (uint32_t)((n + 7) / 8);
    FastpostgresColumnarWrite w;

    fastpostgres_columnar_write_begin(&w, out, (uint32_t)kDatasetColumns, n);

    /* Guard the two regions write_column/write_end touch. The dataset has no
     * NULLs, so the bitmap written by write_column (all zero) is correct as-is.
     * The header guard covers the bytes before the data pointer only; the pad
     * write_column inserts is bounded by the 8-byte alignment of that pointer,
     * and the data is covered by need_data below. */
    auto need_header = [&](const ColumnSpec &s) {
        uint64_t need = 4 + s.name_len + 1 + 4 + null_bytes + 8;

        if ((uint64_t)(w.p - w.base) + need > capacity) fail("fpcolumn: header exceeds capacity");
    };
    auto need_data = [&](const char *data, uint64_t len) {
        if ((uint64_t)(data - out) + len > capacity) fail("fpcolumn: data exceeds capacity");
    };
    /* The one place the store instruction is chosen: every bulk copy of column
     * data goes through here, streaming when the flag asked for it and memcpy
     * otherwise. */
    auto put = [nt_store](char *dst, const void *src, size_t len) {
        if (nt_store)
            copy_nt(dst, src, len);
        else
            std::memcpy(dst, src, len);
    };

    { /* id: int32, g */
        need_header(kColumns[kColId]);
        char *data =
            fastpostgres_columnar_write_column(&w, kColumns[kColId].name, kColumns[kColId].name_len,
                                               kColumns[kColId].kind, kColumns[kColId].pgtype, null_bytes);

        need_data(data, n * 4);
        /* one bulk copy: the payload's column data *is* the dataset's array */
        put(data, ds.id.data(), (size_t)n * 4);
        fastpostgres_columnar_write_end(&w, n * 4);
    }

    { /* sku: text, offset table then the bytes */
        need_header(kColumns[kColSku]);
        char *data =
            fastpostgres_columnar_write_column(&w, kColumns[kColSku].name, kColumns[kColSku].name_len,
                                               kColumns[kColSku].kind, kColumns[kColSku].pgtype, null_bytes);
        uint64_t off_bytes = (n + 1) * 4;
        char    *bytes     = data + off_bytes;

        need_data(data, off_bytes + ds.sku.size());
        /* Two bulk copies, the way a serializer holding contiguous buffers does
         * it: the offsets table, then the concatenated bytes. The per-value loop
         * this replaces was 2M small memcpys across the two text columns, and it
         * was the whole reason another format's encoder looked faster. */
        fastpostgres_put_u32(data, 0);
        put(data + 4, ds.sku_off.data() + 1, (size_t)n * 4);
        put(bytes, ds.sku.data(), ds.sku.size());
        fastpostgres_columnar_write_end(&w, off_bytes + ds.sku.size());
    }

    { /* name: text */
        need_header(kColumns[kColName]);
        char *data = fastpostgres_columnar_write_column(&w, kColumns[kColName].name,
                                                        kColumns[kColName].name_len, kColumns[kColName].kind,
                                                        kColumns[kColName].pgtype, null_bytes);
        uint64_t off_bytes = (n + 1) * 4;
        char    *bytes     = data + off_bytes;

        need_data(data, off_bytes + ds.name.size());
        fastpostgres_put_u32(data, 0);
        put(data + 4, ds.name_off.data() + 1, (size_t)n * 4);
        put(bytes, ds.name.data(), ds.name.size());
        fastpostgres_columnar_write_end(&w, off_bytes + ds.name.size());
    }

    { /* price: float64, g * 0.5 */
        need_header(kColumns[kColPrice]);
        char *data = fastpostgres_columnar_write_column(
            &w, kColumns[kColPrice].name, kColumns[kColPrice].name_len, kColumns[kColPrice].kind,
            kColumns[kColPrice].pgtype, null_bytes);

        need_data(data, n * 8);
        put(data, ds.price.data(), (size_t)n * 8);
        fastpostgres_columnar_write_end(&w, n * 8);
    }

    { /* stock: int32, g % 500 */
        need_header(kColumns[kColStock]);
        char *data = fastpostgres_columnar_write_column(
            &w, kColumns[kColStock].name, kColumns[kColStock].name_len, kColumns[kColStock].kind,
            kColumns[kColStock].pgtype, null_bytes);

        need_data(data, n * 4);
        put(data, ds.stock.data(), (size_t)n * 4);
        fastpostgres_columnar_write_end(&w, n * 4);
    }

    { /* active: bool, g % 2 == 0 */
        need_header(kColumns[kColActive]);
        char *data = fastpostgres_columnar_write_column(
            &w, kColumns[kColActive].name, kColumns[kColActive].name_len, kColumns[kColActive].kind,
            kColumns[kColActive].pgtype, null_bytes);

        need_data(data, n);
        /* 1 B/row, so copy_nt's threshold is what decides for this column: at 1M
         * rows the 1 MB streams with the rest (uniform rule, byte for byte), at
         * small row counts it stays a memcpy. */
        put(data, ds.active.data(), (size_t)n);
        fastpostgres_columnar_write_end(&w, n);
    }

    return (uint64_t)(w.p - w.base);
}

/* The header's entry point (fpg_codec.h is not this change's file): the
 * shipping encoder, ordinary stores, exactly the bytes it always wrote and the
 * behaviour every existing caller keeps. */
uint64_t
fpcolumn_encode(const Dataset &ds, char *out, uint64_t capacity)
{
    return fpcolumn_encode(ds, out, capacity, false);
}

bool
fpcolumn_decode(const uint8_t *payload, uint64_t payload_bytes, FpView *out)
{
    FastpostgresReader rd;

    *out = FpView{};
    if (!fastpostgres_reader_init(&rd, payload, (size_t)payload_bytes)) return false;
    if (rd.ncols != (uint32_t)kDatasetColumns) return false;

    for (uint32_t c = 0; c < rd.ncols; c++)
    {
        FastpostgresColumn col;

        if (!fastpostgres_reader_next(&rd, &col)) return false;

        /* Zero-copy, demonstrated: the reader only computed pointers into the
         * payload. Every byte the aggregate pass will read must lie inside
         * [payload, payload + payload_bytes) — a decoder that copied values
         * out (as the client's tuple construction does) would point elsewhere.
         * The reader's own bounds check makes the fixed-width half redundant;
         * the check is kept as the explicit claim. */
        const uint8_t *end = payload + payload_bytes;

        if (col.data < payload || col.data + col.data_len > end) return false;
        if (fastpostgres_kind_is_varlena(col.kind))
        {
            uint64_t off_bytes = (rd.nrows + 1) * 4; /* the int32 offset table */

            if (col.data_len < off_bytes) return false;
            if (col.bytes < payload || col.bytes + (col.data_len - off_bytes) > end) return false;
        }
        /* The dataset carries no NULLs and the aggregate loop relies on it. */
        if (fastpostgres_nullmap_any(col.nulls, (size_t)((rd.nrows + 7) / 8))) return false;

        out->cols[c].name     = col.name;
        out->cols[c].name_len = col.name_len;
        out->cols[c].kind     = col.kind;
        out->cols[c].pgtype   = col.pgtype;
        out->cols[c].nulls    = col.nulls;
        out->cols[c].data     = col.data;
        out->cols[c].bytes    = col.bytes;
        out->cols[c].data_len = col.data_len;
    }

    out->frame         = Frame::fpcolumn;
    out->payload       = payload;
    out->payload_bytes = payload_bytes;
    out->ncols         = rd.ncols;
    out->nrows         = rd.nrows;
    return frame_matches_dataset(*out);
}

/* --------------------------------------------------------------------------
 * FPROWS — the optional row frame
 * ------------------------------------------------------------------------ */

uint64_t
fprows_capacity(const Dataset &ds)
{
    uint64_t off = FASTPOSTGRES_ROWS_FRAME;

    for (int c = 0; c < kDatasetColumns; c++)
        off += 4 + kColumns[c].name_len + 1 + 4;
    off = FASTPOSTGRES_ALIGN8(off); /* rows_begin aligns the row area */

    for (uint64_t r = 0; r < ds.rows; r++)
        off += 8 + FASTPOSTGRES_ALIGN8(row_layout(ds, r).total);
    return off;
}

uint64_t
fprows_encode(const Dataset &ds, char *out, uint64_t capacity)
{
    const uint64_t        n = ds.rows;
    FastpostgresRowsWrite w;

    fastpostgres_rows_write_begin(&w, out, (uint32_t)kDatasetColumns);
    for (int c = 0; c < kDatasetColumns; c++)
        fastpostgres_rows_write_column(&w, kColumns[c].name, kColumns[c].name_len, kColumns[c].kind,
                                       kColumns[c].pgtype);
    fastpostgres_rows_write_rows_begin(&w);

    char *end = out + capacity;

    for (uint64_t r = 0; r < n; r++)
    {
        RowLayout l   = row_layout(ds, r);
        uint32_t  l1  = ds.sku_off[r + 1] - ds.sku_off[r];
        uint32_t  l2  = ds.name_off[r + 1] - ds.name_off[r];
        char     *row = fastpostgres_rows_reserve(&w, l.total, end);

        if (row == nullptr) fail("fprows: row does not fit the capacity");

        /* Zero the tuple first: the header padding is not the reader's
         * business, and the alignment pad between values must read as zero
         * (att_align_pointer treats a zero byte as "align me"). */
        std::memset(row, 0, l.total);

        uint16_t natts = (uint16_t)kDatasetColumns;
        std::memcpy(row + FASTPOSTGRES_HT_INFOMASK2, &natts, 2);
        std::memcpy(row + FASTPOSTGRES_HT_INFOMASK, &kHeapHasVarwidth, 2);
        row[FASTPOSTGRES_HT_HOFF] = (char)l.hoff;

        std::memcpy(row + l.id, &ds.id[r], 4);

        uint32_t vs = (uint32_t)(4 + l1) << 2; /* 4-byte varlena header: size << 2 */
        std::memcpy(row + l.sku, &vs, 4);
        std::memcpy(row + l.sku + 4, ds.sku.data() + ds.sku_off[r], l1);

        vs = (uint32_t)(4 + l2) << 2;
        std::memcpy(row + l.name, &vs, 4);
        std::memcpy(row + l.name + 4, ds.name.data() + ds.name_off[r], l2);

        std::memcpy(row + l.price, &ds.price[r], 8);
        std::memcpy(row + l.stock, &ds.stock[r], 4);
        row[l.active] = (char)ds.active[r];
    }

    fastpostgres_rows_write_end(&w);
    if ((uint64_t)(w.p - out) > capacity) fail("fprows: payload exceeds capacity");
    return (uint64_t)(w.p - out);
}

bool
fprows_decode(const uint8_t *payload, uint64_t payload_bytes, FpView *out)
{
    FastpostgresRowsReader rd;

    *out = FpView{};
    if (!fastpostgres_rows_init(&rd, payload, (size_t)payload_bytes)) return false;
    if (rd.ncols != (uint32_t)kDatasetColumns) return false;

    FastpostgresRowsDesc d;
    fastpostgres_rows_desc_begin(&d, payload, (uint64_t)(rd.p - rd.base));
    for (uint32_t c = 0; c < rd.ncols; c++)
    {
        FastpostgresColumn col;

        if (!fastpostgres_rows_desc_next(&d, &col)) return false;
        out->cols[c].name     = col.name;
        out->cols[c].name_len = col.name_len;
        out->cols[c].kind     = col.kind;
        out->cols[c].pgtype   = col.pgtype;
        /* The row frame's values live in the row area; the walkers in this
         * file find them there (the descriptor's layout fields are unused). */
        out->cols[c].nulls = col.nulls;
        out->cols[c].data  = col.data;
        out->cols[c].bytes = col.bytes;
    }

    out->frame         = Frame::fprows;
    out->payload       = payload;
    out->payload_bytes = payload_bytes;
    out->ncols         = rd.ncols;
    out->nrows         = rd.left; /* the frame's nrows; rows_next decrements its own copy */
    return frame_matches_dataset(*out);
}

/* --------------------------------------------------------------------------
 * The walks — both sinks read the aliased bytes and nothing else
 * ------------------------------------------------------------------------ */

namespace
{

/* The six timed aggregates: every method is a load plus an add, no allocation,
 * no value materialized. */
struct AggregateSink {
    Aggregates a;

    inline void id(const uint8_t *p)
    {
        a.sum_id += (uint64_t)(int64_t)rd_i32(p);
    }
    inline void stock(const uint8_t *p)
    {
        a.sum_stock += (uint64_t)(int64_t)rd_i32(p);
    }
    inline void price(const uint8_t *p)
    {
        a.sum_price += rd_f64(p);
    }
    inline void active(const uint8_t *p)
    {
        a.trues += (*p != 0) ? 1u : 0u;
    }
    inline void text(const uint8_t *p, uint32_t n)
    {
        (void)p; /* the aggregate only needs the length */
        a.text_bytes += n;
    }
};

/* The logical checksum: the same walk plus the price bits and the FNV over the
 * text bytes, both read where they lie. */
struct ChecksumSink {
    Checksum c;

    inline void id(const uint8_t *p)
    {
        c.sum_id += (uint64_t)(int64_t)rd_i32(p);
    }
    inline void stock(const uint8_t *p)
    {
        c.sum_stock += (uint64_t)(int64_t)rd_i32(p);
    }
    inline void price(const uint8_t *p)
    {
        c.sum_price_bits += rd_u64(p);
    }
    inline void active(const uint8_t *p)
    {
        c.trues += (*p != 0) ? 1u : 0u;
    }
    inline void text(const uint8_t *p, uint32_t n)
    {
        c.text_hash = fnv1a_u32le(c.text_hash, n);
        c.text_hash = fnv1a(c.text_hash, p, n);
        c.text_bytes += n;
    }
};

/* Columnar frame, one specialized loop per column: the kind dispatch runs once
 * per column, not once per value. */
template <typename Sink>
void
visit_columnar(const FpView &v, Sink &sink)
{
    const uint64_t n = v.nrows;

    {
        const uint8_t *p = v.cols[kColId].data;
        for (uint64_t r = 0; r < n; r++)
            sink.id(p + r * 4);
    }
    {
        const uint8_t *tab = v.cols[kColSku].data;
        const uint8_t *txt = v.cols[kColSku].bytes;
        for (uint64_t r = 0; r < n; r++)
        {
            uint32_t s0 = (uint32_t)rd_u32(tab + r * 4);
            uint32_t s1 = (uint32_t)rd_u32(tab + (r + 1) * 4);

            sink.text(txt + s0, s1 - s0);
        }
    }
    {
        const uint8_t *tab = v.cols[kColName].data;
        const uint8_t *txt = v.cols[kColName].bytes;
        for (uint64_t r = 0; r < n; r++)
        {
            uint32_t s0 = (uint32_t)rd_u32(tab + r * 4);
            uint32_t s1 = (uint32_t)rd_u32(tab + (r + 1) * 4);

            sink.text(txt + s0, s1 - s0);
        }
    }
    {
        const uint8_t *p = v.cols[kColPrice].data;
        for (uint64_t r = 0; r < n; r++)
            sink.price(p + r * 8);
    }
    {
        const uint8_t *p = v.cols[kColStock].data;
        for (uint64_t r = 0; r < n; r++)
            sink.stock(p + r * 4);
    }
    {
        const uint8_t *p = v.cols[kColActive].data;
        for (uint64_t r = 0; r < n; r++)
            sink.active(p + r);
    }
}

/* Row frame: the client's tuple walk, over the aliased rows. Every offset is
 * derived from the tuple bytes themselves (hoff, per-attribute alignment), so
 * nothing is copied to find a value. */
template <typename Sink>
void
visit_rows(const FpView &v, Sink &sink)
{
    FastpostgresRowsReader rd;

    if (!fastpostgres_rows_init(&rd, v.payload, (size_t)v.payload_bytes))
        fail("fprows: cannot re-init the reader");

    const uint8_t *row = nullptr;
    uint32_t       len = 0;
    uint64_t       r   = 0;

    while (fastpostgres_rows_next(&rd, &row, &len))
    {
        /* The row aliases the payload; nothing was materialized to walk it. */
        if (row < v.payload || row + len > v.payload + v.payload_bytes)
            fail("fprows: row outside the payload");

        uint32_t off     = fastpostgres_tuple_hoff(row);
        uint32_t natts   = fastpostgres_tuple_natts(row);
        bool     hasnull = fastpostgres_tuple_hasnull(row) != 0;

        if (off < FASTPOSTGRES_HT_BITS || off > len || natts < v.ncols) fail("fprows: bad tuple header");
        if (hasnull) fail("fprows: NULLs are not part of the dataset");

        for (uint32_t c = 0; c < v.ncols; c++)
        {
            uint8_t  kind    = v.cols[c].kind;
            uint32_t alignby = (uint32_t)fastpostgres_kind_alignby(kind);
            int      width   = fastpostgres_kind_width(kind);

            if (width == 0)
            {
                if (!fastpostgres_kind_is_varlena(kind)) fail("fprows: unknown wire kind");
                off               = fastpostgres_att_pointer_align(off, alignby, row + off);
                const uint8_t *p  = row + off;
                uint32_t       vs = fastpostgres_varsize_any(p);

                if (off + vs > len) fail("fprows: varlena runs past the row");
                if (c == kColSku || c == kColName)
                    sink.text(fastpostgres_vardata_any(p), fastpostgres_vardata_len(p));
                else
                    fail("fprows: unexpected varlena column");
                off = fastpostgres_att_addlength(off, -1, p);
            }
            else
            {
                off = fastpostgres_att_align(off, alignby);
                if (off + (uint32_t)width > len) fail("fprows: value runs past the row");
                const uint8_t *p = row + off;

                switch (c)
                {
                    case kColId:
                        sink.id(p);
                        break;
                    case kColPrice:
                        sink.price(p);
                        break;
                    case kColStock:
                        sink.stock(p);
                        break;
                    case kColActive:
                        sink.active(p);
                        break;
                    default:
                        fail("fprows: unexpected fixed-width column");
                }
                off += (uint32_t)width;
            }
        }
        r++;
    }
    if (r != v.nrows) fail("fprows: the walk did not see every row");
}

/* The checksum must hash the text in row order (SPEC.md): len(sku), sku,
 * len(name), name. The columnar frame stores the two texts in separate
 * columns, so this walk interleaves them per row; the loads are still the
 * aliased bytes. */
void
checksum_columnar(const FpView &v, ChecksumSink &sink)
{
    const uint8_t *id_tab   = v.cols[kColId].data;
    const uint8_t *sku_tab  = v.cols[kColSku].data;
    const uint8_t *sku_txt  = v.cols[kColSku].bytes;
    const uint8_t *name_tab = v.cols[kColName].data;
    const uint8_t *name_txt = v.cols[kColName].bytes;
    const uint8_t *price    = v.cols[kColPrice].data;
    const uint8_t *stock    = v.cols[kColStock].data;
    const uint8_t *active   = v.cols[kColActive].data;

    for (uint64_t r = 0; r < v.nrows; r++)
    {
        uint32_t s0, s1;

        sink.id(id_tab + r * 4);
        s0 = (uint32_t)rd_u32(sku_tab + r * 4);
        s1 = (uint32_t)rd_u32(sku_tab + (r + 1) * 4);
        sink.text(sku_txt + s0, s1 - s0);
        s0 = (uint32_t)rd_u32(name_tab + r * 4);
        s1 = (uint32_t)rd_u32(name_tab + (r + 1) * 4);
        sink.text(name_txt + s0, s1 - s0);
        sink.price(price + r * 8);
        sink.stock(stock + r * 4);
        sink.active(active + r);
    }
}

} /* namespace */

Aggregates
aggregate(const FpView &view)
{
    AggregateSink sink;

    /* Columnar: one specialized pass per column (the natural reading of a
     * columnar frame). Rows: one pass over the tuples. */
    if (view.frame == Frame::fpcolumn)
        visit_columnar(view, sink);
    else
        visit_rows(view, sink);
    sink.a.rows = view.nrows;
    return sink.a;
}

Checksum
checksum(const FpView &view)
{
    ChecksumSink sink;

    sink.c.text_hash = kFnvBasis;
    if (view.frame == Frame::fpcolumn)
        checksum_columnar(view, sink);
    else
        visit_rows(view, sink);
    return sink.c;
}

} /* namespace fpg */
