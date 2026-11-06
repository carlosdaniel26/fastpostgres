/*
 * stream.cpp — pull-driven batches over an SPI cursor (a portal in
 * TopPortalContext). The design is documented in stream.h.
 */
#include "pgxx.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "executor/spi.h"
#include "nodes/parsenodes.h"
}
#include "carry.h"
#include "columnar.h"
#include "reduce.h"
#include "rows.h"
#include "shm.h"
#include "stream.h"

namespace
{

constexpr char kStreamPrefix[] = "fastpostgres_stream_";

char *
cursor_name(const char *stream)
{
    const std::size_t n    = sizeof(kStreamPrefix) - 1 + std::strlen(stream) + 1;
    char             *name = static_cast<char *>(palloc(n));

    std::snprintf(name, n, "%s%s", kStreamPrefix, stream);
    return name;
}

} // namespace

void
fp::stream_open(const char *stream, const char *query)
{
    char *name = cursor_name(stream);

    if (SPI_connect() != SPI_OK_CONNECT) elog(ERROR, "fastpostgres_stream_open: SPI_connect failed");

    Portal portal =
        SPI_cursor_open_with_args(name, query, 0, nullptr, nullptr, nullptr, true, CURSOR_OPT_PARALLEL_OK);

    if (portal == nullptr)
    {
        SPI_finish();
        elog(ERROR, "fastpostgres_stream_open: %s", SPI_result_code_string(SPI_result));
    }
    SPI_finish();
}

uint64
fp::stream_next(const char *stream, const char *seg, int64 batch, size_t capacity)
{
    char *path = fp::shm_path(seg);
    char *name = cursor_name(stream);

    if (batch <= 0) elog(ERROR, "fastpostgres_stream_next: batch must be > 0");

    if (SPI_connect() != SPI_OK_CONNECT) elog(ERROR, "fastpostgres_stream_next: SPI_connect failed");

    Portal portal = SPI_cursor_find(name);

    if (portal == nullptr)
    {
        SPI_finish();
        elog(ERROR, "fastpostgres_stream_next: no open stream \"%s\"", stream);
    }

    TupleDesc td    = portal->tupDesc;
    uint64    nrows = fp::columnar_fetch(portal, td, path, batch, capacity);

    SPI_finish();
    return nrows;
}

void
fp::stream_close(const char *stream)
{
    char *name = cursor_name(stream);

    if (SPI_connect() != SPI_OK_CONNECT) elog(ERROR, "fastpostgres_stream_close: SPI_connect failed");

    Portal portal = SPI_cursor_find(name);

    if (portal != nullptr)
    {
        fp::carry_drop(portal); /* the carried rows were this stream's */
        SPI_cursor_close(portal);
    }

    SPI_finish();
}

/* A request failed inside its savepoint: the portal, if any, is gone with the
 * savepoint, so only the carry remains, keyed by the cursor's name. Drop that
 * stream's carry and leave the other rings' carries untouched. Called after the
 * subtransaction has rolled back, so it must not touch SPI. */
void
fp::stream_forget(const char *stream)
{
    char *name = cursor_name(stream);

    fp::carry_drop_name(name);
    pfree(name);
}

/* The same batch as the result tuples themselves: the executor's bytes are
 * copied into the segment once, and the client walks them. */
uint64
fp::stream_next_rows(const char *stream, const char *seg, int64 batch, size_t capacity)
{
    char *path = fp::shm_path(seg);
    char *name = cursor_name(stream);

    if (batch <= 0) elog(ERROR, "fastpostgres_stream_next_rows: batch must be > 0");

    if (SPI_connect() != SPI_OK_CONNECT) elog(ERROR, "fastpostgres_stream_next_rows: SPI_connect failed");

    Portal portal = SPI_cursor_find(name);

    if (portal == nullptr)
    {
        SPI_finish();
        elog(ERROR, "fastpostgres_stream_next_rows: no open stream \"%s\"", stream);
    }

    TupleDesc td = portal->tupDesc;

    if (!fp::rows_native(td))
    {
        /* an attribute the client cannot walk in place: the columnar frame */
        SPI_finish();
        return fp::stream_next(stream, seg, batch, capacity);
    }

    uint64 nrows = fp::rows_fetch(portal, td, path, batch, capacity);

    SPI_finish();
    return nrows;
}

/* The batch reduced where the tuples are: the worker fetches, accumulates and
 * publishes a scalar frame, so neither the columnar encoder nor the client ever
 * sees the rows. The plan resolves against this cursor's descriptor. */
uint64
fp::stream_next_reduce(const char *stream, const char *seg, int64 batch, size_t capacity, const char *plan)
{
    char *path = fp::shm_path(seg);
    char *name = cursor_name(stream);

    if (batch <= 0) elog(ERROR, "fastpostgres_stream_next_reduce: batch must be > 0");

    if (SPI_connect() != SPI_OK_CONNECT) elog(ERROR, "fastpostgres_stream_next_reduce: SPI_connect failed");

    Portal portal = SPI_cursor_find(name);

    if (portal == nullptr)
    {
        SPI_finish();
        elog(ERROR, "fastpostgres_stream_next_reduce: no open stream \"%s\"", stream);
    }

    SPI_cursor_fetch(portal, true, static_cast<long>(batch));

    uint64     nrows  = SPI_processed;
    TupleDesc  td     = SPI_tuptable != nullptr ? SPI_tuptable->tupdesc : portal->tupDesc;
    HeapTuple *tuples = SPI_tuptable != nullptr ? SPI_tuptable->vals : nullptr;

    fp::reduce_publish(path, td, tuples, nrows, plan, capacity);
    SPI_finish();
    return nrows;
}
