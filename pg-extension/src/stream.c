/*
 * stream.c — pull-driven batches over an SPI cursor. The cursor is a portal:
 * it lives in TopPortalContext and is dropped at COMMIT, so the caller must
 * keep a transaction open around the stream.
 *
 * The db_* helpers are shared by the SQL functions below and by the ring
 * worker (worker.c).
 */
#include "postgres.h"

#include <stdio.h>
#include <string.h>

#include "executor/spi.h"
#include "fmgr.h"
#include "nodes/parsenodes.h"
#include "utils/builtins.h"

#include "columnar.h"
#include "shm.h"
#include "stream.h"

#define NP_STREAM_PREFIX "np_stream_"
#define NP_STREAM_DEFAULT_BATCH 10000

static char *
cursor_name(const char *seg)
{
  char *name = palloc(strlen(NP_STREAM_PREFIX) + strlen(seg) + 1);
  sprintf(name, NP_STREAM_PREFIX "%s", seg);
  return name;
}

void
np_stream_open(const char *seg, const char *query)
{
  char *name = cursor_name(seg);
  Portal portal;

  if (SPI_connect() != SPI_OK_CONNECT)
    elog(ERROR, "np_stream_open: SPI_connect failed");

  portal = SPI_cursor_open_with_args(name, query, 0, NULL, NULL, NULL, true,
                                     CURSOR_OPT_PARALLEL_OK);
  if (portal == NULL)
  {
    SPI_finish();
    elog(ERROR, "np_stream_open: %s", SPI_result_code_string(SPI_result));
  }
  SPI_finish();
}

uint64
np_stream_next(const char *seg, int64 batch)
{
  char *path = np_shm_path(seg);
  char *name = cursor_name(seg);
  Portal portal;
  TupleDesc td;
  HeapTuple *tuples;
  uint64 nrows;

  if (batch <= 0)
    elog(ERROR, "np_stream_next: batch must be > 0");

  if (SPI_connect() != SPI_OK_CONNECT)
    elog(ERROR, "np_stream_next: SPI_connect failed");

  portal = SPI_cursor_find(name);
  if (portal == NULL)
  {
    SPI_finish();
    elog(ERROR, "np_stream_next: no open stream for segment \"%s\"", seg);
  }

  SPI_cursor_fetch(portal, true, (long) batch);
  nrows = SPI_processed;
  td = SPI_tuptable ? SPI_tuptable->tupdesc : portal->tupDesc;
  tuples = SPI_tuptable ? SPI_tuptable->vals : NULL;
  np_publish_columnar_shm(path, td, tuples, nrows, 0); /* chunk-sized */
  SPI_finish();
  return nrows;
}

void
np_stream_close(const char *seg)
{
  char *name = cursor_name(seg);
  Portal portal;

  if (SPI_connect() != SPI_OK_CONNECT)
    elog(ERROR, "np_stream_close: SPI_connect failed");

  portal = SPI_cursor_find(name);
  if (portal != NULL)
    SPI_cursor_close(portal);

  SPI_finish();
}

/* ---- SQL wrappers -------------------------------------------------------- */

PG_FUNCTION_INFO_V1(native_stream_open);
Datum
native_stream_open(PG_FUNCTION_ARGS)
{
  char *seg = text_to_cstring(PG_GETARG_TEXT_PP(0));
  char *q = text_to_cstring(PG_GETARG_TEXT_PP(1));
  char summary[256];

  np_stream_open(seg, q);
  snprintf(summary, sizeof(summary), "seg=%s cursor=%s", seg,
           cursor_name(seg));
  PG_RETURN_TEXT_P(cstring_to_text(summary));
}

PG_FUNCTION_INFO_V1(native_stream_next);
Datum
native_stream_next(PG_FUNCTION_ARGS)
{
  char *seg = text_to_cstring(PG_GETARG_TEXT_PP(0));
  int64 batch = PG_NARGS() > 1 && !PG_ARGISNULL(1) ? PG_GETARG_INT64(1)
                                                   : NP_STREAM_DEFAULT_BATCH;
  PG_RETURN_INT64((int64) np_stream_next(seg, batch));
}

PG_FUNCTION_INFO_V1(native_stream_close);
Datum
native_stream_close(PG_FUNCTION_ARGS)
{
  char *seg = text_to_cstring(PG_GETARG_TEXT_PP(0));
  char summary[256];

  np_stream_close(seg);
  snprintf(summary, sizeof(summary), "seg=%s closed", seg);
  PG_RETURN_TEXT_P(cstring_to_text(summary));
}
