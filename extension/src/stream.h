#ifndef FASTPOSTGRES_STREAM_H
#define FASTPOSTGRES_STREAM_H

#include "postgres.h"

/* Pull-driven cursor batches, driven by the ring worker. The cursor is keyed by
 * `stream` and lives in TopPortalContext (it is dropped at COMMIT), so the worker
 * keeps a transaction open across fastpostgres_stream_next() calls and commits
 * after fastpostgres_stream_close().
 *
 * Each batch is published into `seg` rather than into a name of the stream's
 * choosing, so a client can alternate two segments and decode one while the
 * worker fills the other. `capacity` is how big the client asked for that
 * segment to be (0 = FASTPOSTGRES_SEGMENT_BYTES).
 */
void   fastpostgres_stream_open(const char *stream, const char *query);
uint64 fastpostgres_stream_next(const char *stream, const char *seg, int64 batch, size_t capacity);
void   fastpostgres_stream_close(const char *stream);

/* After a failed request's savepoint has rolled back: drop this stream's
 * carried row (the portal died with the savepoint) and leave the others'. */
void   fastpostgres_stream_forget(const char *stream);

/* The next batch as FPROWS: the result tuples themselves, for a client that
 * wants rows. Falls back to the columnar frame when an attribute's storage is
 * not something the client can walk. */
uint64 fastpostgres_stream_next_rows(const char *stream, const char *seg, int64 batch, size_t capacity);

/* The same batch, reduced in the worker: `plan` is reduce.h's op/column plan and
 * the segment receives an FPREDUCE frame (one scalar per op), not the rows.
 * Returns the batch's row count, so the caller knows when the stream ended. */
uint64 fastpostgres_stream_next_reduce(const char *stream, const char *seg, int64 batch, size_t capacity,
                                       const char *plan);

#endif
