/* columnar.h — encode a result set (tuples) into the NPCOLUMN payload. */
#ifndef NP_COLUMNAR_H
#define NP_COLUMNAR_H

#include "postgres.h"

#include "access/htup.h"
#include "access/tupdesc.h"

/* PostgreSQL type OID -> wire kind (see np_shm.h for the kinds and widths). */
int np_oid_to_kind(Oid typ);

/* True for types whose bytes can be read straight off the varlena. */
bool np_is_text_type(Oid typ);

/* Encode in ONE pass into [out, end); returns the bytes written. */
uint64 np_columnar_build(HeapTuple *tuples, TupleDesc td, uint64 nrows,
                         char *out, char *end);

/* Encode straight into /dev/shm/<path>; returns the payload size.
 * `capacity` fixes the segment size (0 = NP_SEGMENT_BYTES). */
uint64 np_publish_columnar_shm(const char *path, TupleDesc td, HeapTuple *tuples,
                               uint64 nrows, size_t capacity);

#endif
