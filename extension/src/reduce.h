/* reduce.h — the worker-side associative reduce (FPREDUCE). */
#ifndef FASTPOSTGRES_REDUCE_H
#define FASTPOSTGRES_REDUCE_H

#include "postgres.h"

#include "access/htup.h"
#include "access/tupdesc.h"

/* Accumulate `plan` over the batch's tuples and publish the scalar frame into
 * /dev/shm/<path>. The plan is
 *
 *     <op>[:<i|n><column>][,<op>[:...]]
 *
 * with op one of 1 COUNT_ALL, 2 COUNT, 3 SUM, 4 MIN, 5 MAX; an integer column
 * is `i<index>` (negative counts from the end) and a name is `n<name>`. A name
 * escapes ',' and '\' with a backslash. Returns the payload size.
 *
 * `capacity` fixes the segment size (0 = FASTPOSTGRES_SEGMENT_BYTES). */
uint64 fastpostgres_reduce_publish(const char *path, TupleDesc td, HeapTuple *tuples, uint64 nrows,
                                   const char *plan, size_t capacity);

#endif
