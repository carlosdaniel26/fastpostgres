/* rows.h — publish a fetch's result tuples as an FPROWS segment. */
#ifndef FASTPOSTGRES_ROWS_H
#define FASTPOSTGRES_ROWS_H

#include "postgres.h"

#include "access/tupdesc.h"
#include "utils/portal.h"

/* True when every attribute's storage is something the client can walk in
 * place: a fixed-width by-value value of its wire kind, or varlena text/bytea.
 * Anything else (numeric, jsonb, name, money, arrays, dropped attributes) needs
 * the type's output function or a different layout, so the batch goes out as
 * the columnar frame instead. */
bool fastpostgres_rows_native(TupleDesc td);

/* Fetch up to `batch` tuples from `portal` into /dev/shm/<path> as FPROWS and
 * publish the segment; returns the rows the batch carries. `capacity` fixes the
 * segment size the worker maps (0 = FASTPOSTGRES_SEGMENT_BYTES). A batch that
 * runs out of room ends there without losing the row that did not fit: it is
 * kept and delivered first by the next call. */
uint64 fastpostgres_rows_fetch(Portal portal, TupleDesc td, const char *path, int64 batch, size_t capacity);

#endif
