#ifndef NP_STREAM_H
#define NP_STREAM_H

#include "postgres.h"

/* Pull-driven cursor batches, shared by the SQL functions and the ring worker.
 * The cursor needs an open transaction, so callers keep it open across
 * np_stream_next() calls and commit after np_stream_close(). */
void np_stream_open(const char *seg, const char *query);
uint64 np_stream_next(const char *seg, int64 batch);
void np_stream_close(const char *seg);

#endif
