#ifndef FASTPOSTGRES_STREAM_H
#define FASTPOSTGRES_STREAM_H

#include "postgres.h"

/* Pull-driven cursor batches, driven by the ring worker. The cursor needs an
 * open transaction, so the worker keeps one open across fastpostgres_stream_next() calls
 * and commits after fastpostgres_stream_close(). */
void   fastpostgres_stream_open(const char *seg, const char *query);
uint64 fastpostgres_stream_next(const char *seg, int64 batch);
void   fastpostgres_stream_close(const char *seg);

/* The next batch as FPROWS: the result tuples themselves, for a client that
 * wants rows. Falls back to the columnar frame when an attribute's storage is
 * not something the client can walk. */
uint64 fastpostgres_stream_next_rows(const char *seg, int64 batch);

#endif
