/* columnar.h — encode a result set (tuples) into the FPCOLUMN payload. */
#ifndef FASTPOSTGRES_COLUMNAR_H
#define FASTPOSTGRES_COLUMNAR_H

#include "postgres.h"

#include "access/htup.h"
#include "access/tupdesc.h"

/* True for types whose bytes can be read straight off the varlena. */
bool fastpostgres_is_text_type(Oid typ);

/* Encode in ONE pass into [out, end); returns the bytes written. */
uint64 fastpostgres_columnar_build(HeapTuple *tuples, TupleDesc td, uint64 nrows, char *out, char *end);

/* Encode straight into /dev/shm/<path>; returns the payload size.
 * `capacity` fixes the segment size (0 = FASTPOSTGRES_SEGMENT_BYTES). */
uint64 fastpostgres_publish_columnar_shm(const char *path, TupleDesc td, HeapTuple *tuples, uint64 nrows,
                                         size_t capacity);

#endif
