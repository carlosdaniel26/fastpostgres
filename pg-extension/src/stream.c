/*
 * stream.c — pull-driven batches over an SPI cursor. The cursor is a portal:
 * it lives in TopPortalContext and is dropped at COMMIT, so the ring worker
 * keeps a transaction open around the stream.
 */
#include "postgres.h"

#include <string.h>

#include "executor/spi.h"
#include "nodes/parsenodes.h"

#include "columnar.h"
#include "shm.h"
#include "stream.h"

#define FASTPOSTGRES_STREAM_PREFIX "fastpostgres_stream_"

static char *cursor_name(const char *seg)
{
    char *name = palloc(strlen(FASTPOSTGRES_STREAM_PREFIX) + strlen(seg) + 1);
    sprintf(name, FASTPOSTGRES_STREAM_PREFIX "%s", seg);
    return name;
}

void fastpostgres_stream_open(const char *seg, const char *query)
{
    char  *name = cursor_name(seg);
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

uint64 fastpostgres_stream_next(const char *seg, int64 batch)
{
    char      *path = fastpostgres_shm_path(seg);
    char      *name = cursor_name(seg);
    Portal     portal;
    TupleDesc  td;
    HeapTuple *tuples;
    uint64     nrows;

    if (batch <= 0) elog(ERROR, "fastpostgres_stream_next: batch must be > 0");

    if (SPI_connect() != SPI_OK_CONNECT) elog(ERROR, "fastpostgres_stream_next: SPI_connect failed");

    portal = SPI_cursor_find(name);
    if (portal == NULL)
    {
        SPI_finish();
        elog(ERROR, "fastpostgres_stream_next: no open stream for segment \"%s\"", seg);
    }

    SPI_cursor_fetch(portal, true, (long)batch);
    nrows  = SPI_processed;
    td     = SPI_tuptable ? SPI_tuptable->tupdesc : portal->tupDesc;
    tuples = SPI_tuptable ? SPI_tuptable->vals : NULL;
    fastpostgres_publish_columnar_shm(path, td, tuples, nrows, 0); /* chunk-sized */
    SPI_finish();
    return nrows;
}

void fastpostgres_stream_close(const char *seg)
{
    char  *name = cursor_name(seg);
    Portal portal;

    if (SPI_connect() != SPI_OK_CONNECT) elog(ERROR, "fastpostgres_stream_close: SPI_connect failed");

    portal = SPI_cursor_find(name);
    if (portal != NULL) SPI_cursor_close(portal);

    SPI_finish();
}
