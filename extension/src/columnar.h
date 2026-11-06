/* columnar.h — encode a result set (tuples) into the FPCOLUMN payload. */
#pragma once

extern "C" {
#include "postgres.h"

#include "access/htup.h"
#include "access/tupdesc.h"
#include "utils/portal.h"
}

namespace fp
{

/* True for types whose bytes can be read straight off the varlena. */
bool is_text_type(Oid typ);

/* Encode in ONE pass into [out, end); returns the bytes written. */
uint64 columnar_build(HeapTuple *tuples, TupleDesc td, uint64 nrows, char *out, char *end);

/* Fetch up to `batch` tuples from `portal` and stream them straight into
 * /dev/shm/<path> as FPCOLUMN, without SPI's tuptable and without a row-major
 * scratch; returns the rows the batch carries. A batch cut short by the segment
 * ends there without losing the row that did not fit: it is kept and delivered
 * first by the next call. */
uint64 columnar_fetch(Portal portal, TupleDesc td, const char *path, int64 batch, size_t capacity);

/* Encode straight into /dev/shm/<path>; returns the payload size.
 * `capacity` fixes the segment size (0 = FASTPOSTGRES_SEGMENT_BYTES). */
uint64 publish_columnar_shm(const char *path, TupleDesc td, HeapTuple *tuples, uint64 nrows, size_t capacity);

} // namespace fp
