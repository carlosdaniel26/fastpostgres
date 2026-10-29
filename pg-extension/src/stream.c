/*
 * stream.c — pull-driven batches. native_stream_open() opens an SPI cursor on a
 * query; each native_stream_next() fetches up to `batch` rows through the
 * executor and writes them as one columnar chunk into /dev/shm.
 *
 * The cursor is a portal: it lives in TopPortalContext and is dropped at COMMIT,
 * so the client must keep a transaction open around the stream.
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

#define NP_STREAM_PREFIX "np_stream_"
#define NP_STREAM_DEFAULT_BATCH 10000

static char *
cursor_name(const char *seg)
{
  char *name = palloc(strlen(NP_STREAM_PREFIX) + strlen(seg) + 1);
  sprintf(name, NP_STREAM_PREFIX "%s", seg);
  return name;
}

/* native_stream_open(segname, query) -> text */
PG_FUNCTION_INFO_V1(native_stream_open);
Datum
native_stream_open(PG_FUNCTION_ARGS)
{
  text *segname = PG_GETARG_TEXT_PP(0);
  text *query = PG_GETARG_TEXT_PP(1);
  char *seg = text_to_cstring(segname);
  char *q = text_to_cstring(query);
  char *name = cursor_name(seg);
  Portal portal;
  char summary[256];

  if (SPI_connect() != SPI_OK_CONNECT)
    elog(ERROR, "native_stream_open: SPI_connect failed");

  portal = SPI_cursor_open_with_args(name, q, 0, NULL, NULL, NULL, true,
                                     CURSOR_OPT_PARALLEL_OK);
  if (portal == NULL)
  {
    SPI_finish();
    elog(ERROR, "native_stream_open: %s", SPI_result_code_string(SPI_result));
  }
  SPI_finish();

  snprintf(summary, sizeof(summary), "seg=%s cursor=%s", seg, name);
  PG_RETURN_TEXT_P(cstring_to_text(summary));
}

/* native_stream_next(segname [, batch]) -> bigint: rows in this chunk (0 = end) */
PG_FUNCTION_INFO_V1(native_stream_next);
Datum
native_stream_next(PG_FUNCTION_ARGS)
{
  text *segname = PG_GETARG_TEXT_PP(0);
  int64 batch = PG_NARGS() > 1 && !PG_ARGISNULL(1) ? PG_GETARG_INT64(1)
                                                   : NP_STREAM_DEFAULT_BATCH;
  char *seg = text_to_cstring(segname);
  char *path = np_shm_path(seg);
  char *name = cursor_name(seg);
  Portal portal;
  TupleDesc td;
  HeapTuple *tuples;
  uint64 nrows;

  if (batch <= 0)
    elog(ERROR, "native_stream_next: batch must be > 0");

  if (SPI_connect() != SPI_OK_CONNECT)
    elog(ERROR, "native_stream_next: SPI_connect failed");

  portal = SPI_cursor_find(name);
  if (portal == NULL)
  {
    SPI_finish();
    elog(ERROR, "native_stream_next: no open stream for segment \"%s\"", seg);
  }

  SPI_cursor_fetch(portal, true, (long) batch);
  nrows = SPI_processed;
  td = SPI_tuptable ? SPI_tuptable->tupdesc : portal->tupDesc;
  tuples = SPI_tuptable ? SPI_tuptable->vals : NULL;
  np_publish_columnar_shm(path, td, tuples, nrows, 0); /* chunk-sized */
  SPI_finish();

  PG_RETURN_INT64((int64) nrows);
}

/* native_stream_close(segname) -> text */
PG_FUNCTION_INFO_V1(native_stream_close);
Datum
native_stream_close(PG_FUNCTION_ARGS)
{
  text *segname = PG_GETARG_TEXT_PP(0);
  char *seg = text_to_cstring(segname);
  char *name = cursor_name(seg);
  Portal portal;
  char summary[256];

  if (SPI_connect() != SPI_OK_CONNECT)
    elog(ERROR, "native_stream_close: SPI_connect failed");

  portal = SPI_cursor_find(name);
  if (portal != NULL)
    SPI_cursor_close(portal);

  SPI_finish();

  snprintf(summary, sizeof(summary), "seg=%s closed", seg);
  PG_RETURN_TEXT_P(cstring_to_text(summary));
}
