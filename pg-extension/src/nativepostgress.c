/*
 * nativepostgress — run a query through PostgreSQL's executor (SPI) and ship
 * the result as a columnar NPCOLUMN payload: a bytea, or a /dev/shm segment.
 */
#include "postgres.h"

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "varatt.h"

#include "np_shm.h"

#include "columnar.h"
#include "shm.h"

PG_MODULE_MAGIC;

/* Run a read-only row-returning query via SPI. The caller must SPI_finish(). */
static void
spi_run(const char *sql, const char *fn)
{
  if (SPI_connect() != SPI_OK_CONNECT)
    elog(ERROR, "%s: SPI_connect failed", fn);
  if (SPI_execute(sql, true, 0) < 0 || SPI_tuptable == NULL)
  {
    SPI_finish();
    elog(ERROR, "%s: not a row-returning query: %s", fn, sql);
  }
}

PG_FUNCTION_INFO_V1(native_query);
Datum native_query(PG_FUNCTION_ARGS) {
  text *query = PG_GETARG_TEXT_PP(0);
  char *q = text_to_cstring(query);
  MemoryContext upper = CurrentMemoryContext; /* survives SPI_finish */
  uint64 total;
  bytea *result;
  char *body;

  spi_run(q, "native_query");

  total = np_columnar_size(SPI_tuptable->vals, SPI_tuptable->tupdesc,
                           SPI_processed);
  /* bytea is a 4-byte varlena: max ~1 GB */
  if (total > (uint64) 0x3FFFFFFF - VARHDRSZ)
    elog(ERROR, "native_query: result exceeds bytea limit");
  result = (bytea *) MemoryContextAlloc(upper, VARHDRSZ + total);
  SET_VARSIZE(result, VARHDRSZ + total);
  body = VARDATA(result);
  np_columnar_build(SPI_tuptable->vals, SPI_tuptable->tupdesc, SPI_processed,
                    body, body + total);
  SPI_finish();
  PG_RETURN_BYTEA_P(result);
}

PG_FUNCTION_INFO_V1(native_query_header);
Datum native_query_header(PG_FUNCTION_ARGS) {
  text *query = PG_GETARG_TEXT_PP(0);
  char *q = text_to_cstring(query);
  MemoryContext upper = CurrentMemoryContext; /* survives SPI_finish */
  TupleDesc td;
  StringInfoData s;
  int i;
  text *result;
  MemoryContext old;

  spi_run(q, "native_query_header");
  td = SPI_tuptable->tupdesc;

  initStringInfo(&s);
  appendStringInfo(&s, "nrows=%llu ncols=%d cols=[",
                   (unsigned long long) SPI_processed, td->natts);
  for (i = 0; i < td->natts; i++) {
    Form_pg_attribute att = TupleDescAttr(td, i);
    if (i) appendStringInfoString(&s, ", ");
    appendStringInfo(&s, "%s:%s", NameStr(att->attname),
                     format_type_be(att->atttypid));
  }
  appendStringInfoString(&s, "]");

  /* build the result in the caller's context, not the SPI context */
  old = MemoryContextSwitchTo(upper);
  result = cstring_to_text(s.data);
  MemoryContextSwitchTo(old);

  SPI_finish();
  PG_RETURN_TEXT_P(result);
}

/*
 * native_query_shm(segname, query) -> text
 * Run a query and write the columnar result into /dev/shm/<segname>, which a
 * separate process reads.
 */
PG_FUNCTION_INFO_V1(native_query_shm);
Datum
native_query_shm(PG_FUNCTION_ARGS)
{
  text *segname = PG_GETARG_TEXT_PP(0);
  text *query = PG_GETARG_TEXT_PP(1);
  char *seg = text_to_cstring(segname);
  char *q = text_to_cstring(query);
  char *path = np_shm_path(seg);
  uint64 nrows;
  uint64 total;

  spi_run(q, "native_query_shm");
  nrows = SPI_processed;
  total = np_publish_columnar_shm(path, SPI_tuptable->tupdesc,
                                  SPI_tuptable->vals, nrows,
                                  NP_SEGMENT_BYTES);
  SPI_finish();

  {
    char summary[256];
    snprintf(summary, sizeof(summary), "seg=%s payload=%llu bytes", seg,
             (unsigned long long) total);
    PG_RETURN_TEXT_P(cstring_to_text(summary));
  }
}

/* native_shm_read(segname) -> bytea */
PG_FUNCTION_INFO_V1(native_shm_read);
Datum
native_shm_read(PG_FUNCTION_ARGS)
{
  text *segname = PG_GETARG_TEXT_PP(0);
  char *seg = text_to_cstring(segname);
  char *path = np_shm_path(seg);
  size_t size;
  void *map = np_shm_map_ro(path, "native_shm_read", &size);
  size_t n = ((NpShmHeader *) map)->payload_size;
  bytea *result;

  if (NP_SHM_HEADER_SIZE + n > size)
  {
    munmap(map, size);
    elog(ERROR, "native_shm_read: payload exceeds segment");
  }

  result = (bytea *) palloc(VARHDRSZ + n);
  SET_VARSIZE(result, VARHDRSZ + n);
  memcpy(VARDATA(result), (char *) map + NP_SHM_HEADER_SIZE, n);
  munmap(map, size);

  PG_RETURN_BYTEA_P(result);
}

/* native_shm_info(segname) -> text */
PG_FUNCTION_INFO_V1(native_shm_info);
Datum
native_shm_info(PG_FUNCTION_ARGS)
{
  text *segname = PG_GETARG_TEXT_PP(0);
  char *seg = text_to_cstring(segname);
  char *path = np_shm_path(seg);
  size_t size;
  void *map = np_shm_map_ro(path, "native_shm_info", &size);
  NpShmHeader *h = (NpShmHeader *) map;
  char summary[256];

  snprintf(summary, sizeof(summary), "payload=%llu capacity=%llu seq=%llu",
           (unsigned long long) h->payload_size,
           (unsigned long long) h->capacity, (unsigned long long) h->seq);
  munmap(map, size);

  PG_RETURN_TEXT_P(cstring_to_text(summary));
}
