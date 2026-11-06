/*
 * stream.c — pull-driven batches over an SPI cursor (a portal in
 * TopPortalContext). The design is documented in stream.h.
 */
#include "postgres.h"

#include <string.h>

#include "executor/spi.h"
#include "nodes/parsenodes.h"

#include "columnar.h"
#include "carry.h"
#include "reduce.h"
#include "rows.h"
#include "shm.h"
#include "stream.h"

#define FASTPOSTGRES_STREAM_PREFIX "fastpostgres_stream_"

static char *
cursor_name(const char *stream)
{
    char *name = palloc(strlen(FASTPOSTGRES_STREAM_PREFIX) + strlen(stream) + 1);
    sprintf(name, FASTPOSTGRES_STREAM_PREFIX "%s", stream);
    return name;
}

void
fastpostgres_stream_open(const char *stream, const char *query)
{
    char  *name = cursor_name(stream);
    Portal portal;

    if (SPI_connect() != SPI_OK_CONNECT) elog(ERROR, "fastpostgres_stream_open: SPI_connect failed");

    portal = SPI_cursor_open_with_args(name, query, 0, NULL, NULL, NULL, true, CURSOR_OPT_PARALLEL_OK);
    if (portal == NULL)
    {
        SPI_finish();
        elog(ERROR, "fastpostgres_stream_open: %s", SPI_result_code_string(SPI_result));
    }
    SPI_finish();
}

uint64
fastpostgres_stream_next(const char *stream, const char *seg, int64 batch, size_t capacity)
{
    char     *path = fastpostgres_shm_path(seg);
    char     *name = cursor_name(stream);
    Portal    portal;
    TupleDesc td;
    uint64    nrows;

    if (batch <= 0) elog(ERROR, "fastpostgres_stream_next: batch must be > 0");

    if (SPI_connect() != SPI_OK_CONNECT) elog(ERROR, "fastpostgres_stream_next: SPI_connect failed");

    portal = SPI_cursor_find(name);
    if (portal == NULL)
    {
        SPI_finish();
        elog(ERROR, "fastpostgres_stream_next: no open stream \"%s\"", stream);
    }

    td    = portal->tupDesc;
    nrows = fastpostgres_columnar_fetch(portal, td, path, batch, capacity);
    SPI_finish();
    return nrows;
}

void
fastpostgres_stream_close(const char *stream)
{
    char  *name = cursor_name(stream);
    Portal portal;

    if (SPI_connect() != SPI_OK_CONNECT) elog(ERROR, "fastpostgres_stream_close: SPI_connect failed");

    portal = SPI_cursor_find(name);
    if (portal != NULL)
    {
        fastpostgres_carry_drop(portal); /* the carried rows were this stream's */
        SPI_cursor_close(portal);
    }

    SPI_finish();
}

/* The same batch as the result tuples themselves: the executor's bytes are
 * copied into the segment once, and the client walks them. */
uint64
fastpostgres_stream_next_rows(const char *stream, const char *seg, int64 batch, size_t capacity)
{
    char     *path = fastpostgres_shm_path(seg);
    char     *name = cursor_name(stream);
    Portal    portal;
    TupleDesc td;
    uint64    nrows;

    if (batch <= 0) elog(ERROR, "fastpostgres_stream_next_rows: batch must be > 0");

    if (SPI_connect() != SPI_OK_CONNECT) elog(ERROR, "fastpostgres_stream_next_rows: SPI_connect failed");

    portal = SPI_cursor_find(name);
    if (portal == NULL)
    {
        SPI_finish();
        elog(ERROR, "fastpostgres_stream_next_rows: no open stream \"%s\"", stream);
    }
    td = portal->tupDesc;

    if (!fastpostgres_rows_native(td))
    {
        /* an attribute the client cannot walk in place: the columnar frame */
        SPI_finish();
        return fastpostgres_stream_next(stream, seg, batch, capacity);
    }

    nrows = fastpostgres_rows_fetch(portal, td, path, batch, capacity);
    SPI_finish();
    return nrows;
}

/* The batch reduced where the tuples are: the worker fetches, accumulates and
 * publishes a scalar frame, so neither the columnar encoder nor the client ever
 * sees the rows. The plan resolves against this cursor's descriptor. */
uint64
fastpostgres_stream_next_reduce(const char *stream, const char *seg, int64 batch, size_t capacity,
                                const char *plan)
{
    char      *path = fastpostgres_shm_path(seg);
    char      *name = cursor_name(stream);
    Portal     portal;
    TupleDesc  td;
    HeapTuple *tuples;
    uint64     nrows;

    if (batch <= 0) elog(ERROR, "fastpostgres_stream_next_reduce: batch must be > 0");

    if (SPI_connect() != SPI_OK_CONNECT) elog(ERROR, "fastpostgres_stream_next_reduce: SPI_connect failed");

    portal = SPI_cursor_find(name);
    if (portal == NULL)
    {
        SPI_finish();
        elog(ERROR, "fastpostgres_stream_next_reduce: no open stream \"%s\"", stream);
    }

    SPI_cursor_fetch(portal, true, (long)batch);
    nrows  = SPI_processed;
    td     = SPI_tuptable ? SPI_tuptable->tupdesc : portal->tupDesc;
    tuples = SPI_tuptable ? SPI_tuptable->vals : NULL;
    fastpostgres_reduce_publish(path, td, tuples, nrows, plan, capacity);
    SPI_finish();
    return nrows;
}
