/* fpg_codec.h — the FPG cell of the codec/transport isolation experiment.
 *
 * Implements the contract in bench/internals/codec_transport/SPEC.md for one codec:
 *
 *   - "generate"           the shared dataset (id/sku/name/price/stock/active),
 *                          deterministic over g = 1..N;
 *   - "fpcolumn_encode"    this repo's columnar frame, written with the same
 *                          writer helpers (fastpostgres_columnar_*) the
 *                          extension uses in extension/src/columnar.c, so
 *                          the bytes are exactly what the shipping readers
 *                          parse in extension/fastpostgres_shm.h;
 *   - "fpcolumn_decode"    the shipping readers, aliasing the payload: the
 *                          column views point into the payload and the timed
 *                          aggregates read them where they lie (zero-copy);
 *   - "fprows_encode"      the optional row frame: PostgreSQL-shaped tuple
 *                          bytes inside the FPROWS row area, the shape
 *                          extension/src/rows.c produces and the client's
 *                          tuple walker (fp_decode_tuples) consumes;
 *   - "fprows_decode"      the shipping row readers (fastpostgres_rows_*);
 *   - "aggregate"          the six timed aggregates (rows, sum id, sum stock,
 *                          sum price, count(active), sum of text bytes);
 *   - "checksum"           the five-number logical checksum (SPEC.md), computed
 *                          once, outside the timed region.
 *
 * The timing, the CLI and the JSON line live in fpg_bench.cpp.
 */
#ifndef FPG_CODEC_H
#define FPG_CODEC_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "fastpostgres/fastpostgres_shm.h"

namespace fpg
{

/* The dataset's columns, in frame order (SPEC.md "Dataset"). */
enum DatasetColumn : int
{
    kColId = 0,
    kColSku,
    kColName,
    kColPrice,
    kColStock,
    kColActive,
    kDatasetColumns
};

/* Room for the six dataset columns plus any future ones, no allocation. */
constexpr uint32_t kMaxColumns = 16;

/* --------------------------------------------------------------------------
 * Dataset
 * ------------------------------------------------------------------------ */

struct Dataset {
    uint64_t              rows = 0;
    std::vector<int32_t>  id;       /* g                       */
    std::vector<int32_t>  stock;    /* g % 500                 */
    std::vector<double>   price;    /* g * 0.5                 */
    std::vector<uint8_t>  active;   /* g % 2 == 0              */
    std::vector<char>     sku;      /* "SKU-" + g, concatenated */
    std::vector<uint32_t> sku_off;  /* rows + 1 offsets into sku  */
    std::vector<char>     name;     /* "Product " + g, concatenated */
    std::vector<uint32_t> name_off; /* rows + 1 offsets into name */
};

/* Deterministic, identical for every cell: g = 1..rows. */
Dataset generate(uint64_t rows);

/* --------------------------------------------------------------------------
 * Decoded views — pointers into the payload, never a copy
 * ------------------------------------------------------------------------ */

struct FpColumnView {
    const char    *name     = nullptr;
    uint32_t       name_len = 0;
    uint8_t        kind     = 0;
    uint32_t       pgtype   = 0;
    const uint8_t *nulls    = nullptr;
    const uint8_t *data     = nullptr; /* fixed-width values, or the offset table */
    const uint8_t *bytes    = nullptr; /* text/bytea bytes, varlena kinds only   */
    uint64_t       data_len = 0;
};

enum class Frame
{
    fpcolumn,
    fprows
};

struct FpView {
    Frame          frame         = Frame::fpcolumn;
    const uint8_t *payload       = nullptr;
    uint64_t       payload_bytes = 0;
    uint32_t       ncols         = 0;
    uint64_t       nrows         = 0;
    FpColumnView   cols[kMaxColumns];
};

/* fpcolumn: payload written with fastpostgres_columnar_write_begin()/write_column()
 * /write_end() (the frame carries the six dataset columns in order). */
uint64_t fpcolumn_capacity(const Dataset &ds);
uint64_t fpcolumn_encode(const Dataset &ds, char *out, uint64_t capacity);
bool     fpcolumn_decode(const uint8_t *payload, uint64_t payload_bytes, FpView *out);

/* fprows: the optional row frame; the tuples are synthesized here, laid out by
 * the same rules the client's tuple walk mirrors (a 4-byte varlena header,
 * every value at its attribute alignment, zero padding). */
uint64_t fprows_capacity(const Dataset &ds);
uint64_t fprows_encode(const Dataset &ds, char *out, uint64_t capacity);
bool     fprows_decode(const uint8_t *payload, uint64_t payload_bytes, FpView *out);

/* --------------------------------------------------------------------------
 * The timed work and the proof
 * ------------------------------------------------------------------------ */

/* The six aggregates SPEC.md's timed region computes, over the decoded view.
 * Read-only: every input is an aliased payload byte. */
struct Aggregates {
    uint64_t rows       = 0;
    uint64_t sum_id     = 0;
    uint64_t sum_stock  = 0;
    double   sum_price  = 0.0;
    uint64_t trues      = 0;
    uint64_t text_bytes = 0; /* sum of len(sku) + len(name) */
};

/* The logical checksum (SPEC.md), computed once outside the timed region.
 * `text_bytes` is not part of the reported checksum; it is carried so the
 * harness can prove the timed aggregates and this pass saw the same data. */
struct Checksum {
    uint64_t sum_id         = 0;
    uint64_t sum_stock      = 0;
    uint64_t sum_price_bits = 0;
    uint64_t trues          = 0;
    uint64_t text_hash      = 0;
    uint64_t text_bytes     = 0;
};

Aggregates aggregate(const FpView &view);
Checksum   checksum(const FpView &view);

/* --------------------------------------------------------------------------
 * Copy accounting (SPEC.md "Copy accounting is part of the result")
 *
 * The unit is "passes that copy the dataset's bytes end to end in one
 * encode+decode run". The encode is the only pass that writes the values
 * somewhere new; the decode's readers only compute pointers (the decode
 * function rejects any view whose pointers leave the payload), and the
 * aggregate/checksum passes load scalars out of those pointers without
 * materializing anything.
 * ------------------------------------------------------------------------ */
constexpr int kCopiesValuesToPayload  = 1; /* encoder: dataset arrays -> frame            */
constexpr int kCopiesPayloadToViews   = 0; /* decode: readers alias the payload           */
constexpr int kCopiesViewsToAggregate = 0; /* timed pass: sums read the aliased bytes     */
constexpr int kCopies = kCopiesValuesToPayload + kCopiesPayloadToViews + kCopiesViewsToAggregate;

} /* namespace fpg */

#endif /* FPG_CODEC_H */
