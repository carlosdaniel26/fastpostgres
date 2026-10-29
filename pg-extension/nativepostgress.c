/*
 * nativepostgress — a PostgreSQL extension that runs a query via SPI and
 * returns the result as a compact COLUMNAR bytea (the same shape the V8 side
 * materializes as typed arrays + raw text). This is the "real query" producer.
 *
 *   native_query(text)        -> bytea   (full columnar result)
 *   native_query_header(text) -> text    (human-readable summary)
 *
 * Columnar format (all integers little-endian):
 *
 *   header:
 *     magic[8]  "NPCOLUMN"
 *     version   u32 = 1
 *     flags     u32 = 1   (bit0: 1 = little-endian)
 *     ncols     u32
 *     nrows     u64
 *   per column:
 *     name_len  u32
 *     name      bytes (no NUL)
 *     kind      u8    (1=bool, 2=int32, 3=int64, 4=float8, 5=text)
 *     pgtype    u32   (informational OID)
 *     nulls     bitmap ((nrows+7)/8 bytes)
 *     data_len  u64
 *     data      bytes:
 *       kind 1/2/3/4: nrows * width(1/4/8/8)
 *       kind 5:       offsets int32[(nrows+1)] followed by raw text bytes
 */
#include "postgres.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/tupdesc.h"
#include "access/tupmacs.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "nodes/makefuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/snapmgr.h"

#include "np_shm.h"

PG_MODULE_MAGIC;

#define NP_MAGIC "NPCOLUMN"
#define NP_VERSION 1

#define NP_KIND_BOOL 1
#define NP_KIND_INT32 2
#define NP_KIND_INT64 3
#define NP_KIND_FLOAT8 4
#define NP_KIND_TEXT 5

static int oid_to_kind(Oid typ) {
  switch (typ) {
    case BOOLOID:
      return NP_KIND_BOOL;
    case INT2OID:
    case INT4OID:
      return NP_KIND_INT32;
    case INT8OID:
      return NP_KIND_INT64;
    case FLOAT4OID:
    case FLOAT8OID:
    case NUMERICOID:
      return NP_KIND_FLOAT8;
    default:
      return NP_KIND_TEXT; /* varlena text OR fallback via SPI_getvalue */
  }
}

static bool is_varlena_text(Oid typ) {
  return typ == TEXTOID || typ == VARCHAROID || typ == BPCHAROID;
}

static int kind_width(int kind) {
  switch (kind) {
    case NP_KIND_BOOL:
      return 1;
    case NP_KIND_INT32:
      return 4;
    case NP_KIND_INT64:
      return 8;
    case NP_KIND_FLOAT8:
      return 8;
    default:
      return 0;
  }
}

/* little-endian writers */
static void put_u32(char *p, uint32_t v) { memcpy(p, &v, 4); }
static void put_u64(char *p, uint64_t v) { memcpy(p, &v, 8); }

typedef struct ColInfo {
  const char *name;
  int attnum; /* 1-based attribute number in the relation */
  Oid type;
  Oid typoutput; /* output function for non-varlena text */
  int kind;
  int width;
  int null_bytes;
  uint64 data_len;
} ColInfo;

/* Direct access to a text cell's bytes (no SPI: output function for the rest). */
static void
text_cell(HeapTuple tuple, TupleDesc td, int col, Oid type, Oid typoutput,
          bool *isnull, const char **data, int *len)
{
  Datum d = heap_getattr(tuple, col + 1, td, isnull);
  if (*isnull)
  {
    *data = NULL;
    *len = 0;
    return;
  }
  if (is_varlena_text(type))
  {
    *data = VARDATA_ANY(d);
    *len = VARSIZE_ANY_EXHDR(d);
  }
  else
  {
    char *s = OidOutputFunctionCall(typoutput, d);
    *data = s ? s : "";
    *len = s ? (int) strlen(s) : 0;
  }
}

/* Pass 1: compute per-column sizes and the total output size. */
static uint64
compute_sizes(SPITupleTable *tt, TupleDesc td, uint64 nrows, ColInfo *cols,
              int ncols)
{
  uint64 total = 8 + 4 + 4 + 4 + 8; /* magic + version + flags + ncols + nrows */
  int c;
  uint64 r;

  for (c = 0; c < ncols; c++)
  {
    Form_pg_attribute att = TupleDescAttr(td, c);
    cols[c].name = NameStr(att->attname);
    cols[c].attnum = c + 1;
    cols[c].type = att->atttypid;
    cols[c].kind = oid_to_kind(att->atttypid);
    cols[c].width = kind_width(cols[c].kind);
    cols[c].null_bytes = (int) ((nrows + 7) / 8);
    if (cols[c].kind == NP_KIND_TEXT && !is_varlena_text(att->atttypid))
    {
      bool varlena;
      getTypeOutputInfo(att->atttypid, &cols[c].typoutput, &varlena);
    }
    else
      cols[c].typoutput = InvalidOid;

    total += 4 + (uint64) strlen(cols[c].name) + 1 + 4 + cols[c].null_bytes + 8;

    if (cols[c].kind == NP_KIND_TEXT)
    {
      uint64 tbytes = 0;
      for (r = 0; r < nrows; r++)
      {
        bool isnull;
        const char *data;
        int len;
        text_cell(tt->vals[r], td, c, cols[c].type, cols[c].typoutput,
                  &isnull, &data, &len);
        if (!isnull) tbytes += (uint64) len;
      }
      cols[c].data_len = (uint64) (nrows + 1) * 4 + tbytes;
    }
    else
    {
      cols[c].data_len = (uint64) nrows * (uint64) cols[c].width;
    }
    total += cols[c].data_len;
  }
  return total;
}

/* Pass 2: serialize the result directly into `out` (no intermediate buffer). */
static void
fill_columnar(SPITupleTable *tt, TupleDesc td, uint64 nrows, ColInfo *cols,
              int ncols, char *out)
{
  char *p = out;
  int c;
  uint64 r;

  memcpy(p, NP_MAGIC, 8); p += 8;
  put_u32(p, NP_VERSION); p += 4;
  put_u32(p, 1); p += 4; /* flags: little-endian */
  put_u32(p, (uint32) ncols); p += 4;
  put_u64(p, nrows); p += 8;

  for (c = 0; c < ncols; c++)
  {
    ColInfo *col = &cols[c];
    uint32 name_len = (uint32) strlen(col->name);

    put_u32(p, name_len); p += 4;
    memcpy(p, col->name, name_len); p += name_len;
    *p++ = (char) col->kind;
    put_u32(p, (uint32) col->type); p += 4;

    char *nulls = p;
    memset(nulls, 0, col->null_bytes);
    p += col->null_bytes;

    put_u64(p, col->data_len); p += 8;

    if (col->kind == NP_KIND_TEXT)
    {
      uint32 *off = (uint32 *) p;
      p += (nrows + 1) * 4;
      char *bytes = p;
      off[0] = 0;
      for (r = 0; r < nrows; r++)
      {
        bool isnull;
        const char *data;
        int len;
        text_cell(tt->vals[r], td, c, col->type, col->typoutput, &isnull,
                  &data, &len);
        if (isnull)
        {
          nulls[r >> 3] |= (char) (1 << (r & 7));
          off[r + 1] = off[r];
        }
        else
        {
          memcpy(p, data, len);
          p += len;
          off[r + 1] = (uint32) (p - bytes);
        }
      }
    }
    else
    {
      int w = col->width;
      for (r = 0; r < nrows; r++)
      {
        bool isnull;
        Datum d = heap_getattr(tt->vals[r], c + 1, td, &isnull);
        if (isnull)
        {
          nulls[r >> 3] |= (char) (1 << (r & 7));
          continue;
        }
        switch (col->kind)
        {
          case NP_KIND_BOOL:
            *p = DatumGetBool(d) ? 1 : 0;
            break;
          case NP_KIND_INT32:
          {
            int32 v = (col->type == INT2OID) ? (int32) DatumGetInt16(d)
                                             : DatumGetInt32(d);
            memcpy(p, &v, 4);
            break;
          }
          case NP_KIND_INT64:
          {
            int64 v = DatumGetInt64(d);
            memcpy(p, &v, 8);
            break;
          }
          case NP_KIND_FLOAT8:
          {
            double v;
            switch (col->type)
            {
              case FLOAT4OID:
                v = (double) DatumGetFloat4(d);
                break;
              case FLOAT8OID:
                v = DatumGetFloat8(d);
                break;
              case NUMERICOID:
                v = DatumGetFloat8(DirectFunctionCall1(numeric_float8, d));
                break;
              default:
                v = 0.0;
                break;
            }
            memcpy(p, &v, 8);
            break;
          }
        }
        p += w;
      }
    }
  }
}

PG_FUNCTION_INFO_V1(native_query);
Datum native_query(PG_FUNCTION_ARGS) {
  text *query = PG_GETARG_TEXT_PP(0);
  char *q = text_to_cstring(query);
  int ret;
  MemoryContext upper = CurrentMemoryContext; /* survives SPI_finish */

  if ((ret = SPI_connect()) != SPI_OK_CONNECT)
    elog(ERROR, "native_query: SPI_connect failed (%d)", ret);

  ret = SPI_execute(q, true, 0);
  if (ret < 0 || SPI_tuptable == NULL) {
    SPI_finish();
    elog(ERROR, "native_query: not a row-returning query");
  }

  {
    int ncols = SPI_tuptable->tupdesc->natts;
    ColInfo *cols = palloc0(sizeof(ColInfo) * ncols);
    uint64 total = compute_sizes(SPI_tuptable, SPI_tuptable->tupdesc,
                                 SPI_processed, cols, ncols);
    /* bytea is a 4-byte varlena: max ~1 GB */
    if (total > (uint64) 0x3FFFFFFF - VARHDRSZ)
      elog(ERROR, "native_query: result exceeds bytea limit");
    bytea *result = (bytea *) MemoryContextAlloc(upper, VARHDRSZ + total);
    SET_VARSIZE(result, VARHDRSZ + total);
    fill_columnar(SPI_tuptable, SPI_tuptable->tupdesc, SPI_processed,
                  cols, ncols, VARDATA(result));
    pfree(cols);
    SPI_finish();
    PG_RETURN_BYTEA_P(result);
  }
}

PG_FUNCTION_INFO_V1(native_query_header);
Datum native_query_header(PG_FUNCTION_ARGS) {
  text *query = PG_GETARG_TEXT_PP(0);
  char *q = text_to_cstring(query);
  int ret;
  MemoryContext upper = CurrentMemoryContext; /* survives SPI_finish */

  if ((ret = SPI_connect()) != SPI_OK_CONNECT)
    elog(ERROR, "native_query_header: SPI_connect failed (%d)", ret);

  ret = SPI_execute(q, true, 0);
  if (ret < 0 || SPI_tuptable == NULL) {
    SPI_finish();
    elog(ERROR, "native_query_header: not a row-returning query");
  }

  {
    TupleDesc td = SPI_tuptable->tupdesc;
    StringInfoData s;
    int i;
    text *result;
    MemoryContext old;
    initStringInfo(&s);
    appendStringInfo(&s, "nrows=%llu ncols=%d cols=[", (unsigned long long) SPI_processed,
                     td->natts);
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
}

static char *
shm_path(const char *seg)
{
  char *path = palloc(strlen(seg) + 2);
  sprintf(path, "/%s", seg);
  return path;
}

/*
 * native_query_shm(segname, query) -> text
 * Run a query and write the columnar result straight into a POSIX shared-memory
 * segment (/dev/shm/<segname>) that a separate process (the V8 embedder) reads.
 */
PG_FUNCTION_INFO_V1(native_query_shm);
Datum
native_query_shm(PG_FUNCTION_ARGS)
{
  text *segname = PG_GETARG_TEXT_PP(0);
  text *query = PG_GETARG_TEXT_PP(1);
  char *seg = text_to_cstring(segname);
  char *q = text_to_cstring(query);
  char *path = shm_path(seg);
  int ret;
  uint64 total;

  if ((ret = SPI_connect()) != SPI_OK_CONNECT)
    elog(ERROR, "native_query_shm: SPI_connect failed (%d)", ret);
  ret = SPI_execute(q, true, 0);
  if (ret < 0 || SPI_tuptable == NULL)
  {
    SPI_finish();
    elog(ERROR, "native_query_shm: not a row-returning query");
  }

  {
    int ncols = SPI_tuptable->tupdesc->natts;
    ColInfo *cols = palloc0(sizeof(ColInfo) * ncols);
    total = compute_sizes(SPI_tuptable, SPI_tuptable->tupdesc,
                          SPI_processed, cols, ncols);
    size_t seg_size = NP_SHM_HEADER_SIZE + total;
    int fd;
    struct stat st;
    void *map;
    NpShmHeader *h;

    fd = shm_open(path, O_RDWR | O_CREAT, 0666);
    if (fd < 0)
      elog(ERROR, "native_query_shm: shm_open(%s) failed: %m", path);
    fchmod(fd, 0666); /* world-readable so a separate process can read it */

    if (fstat(fd, &st) != 0)
    {
      close(fd);
      elog(ERROR, "native_query_shm: fstat failed: %m");
    }
    if ((size_t) st.st_size < seg_size)
    {
      if (ftruncate(fd, (off_t) seg_size) != 0)
      {
        close(fd);
        elog(ERROR, "native_query_shm: ftruncate failed: %m");
      }
    }

    map = mmap(NULL, seg_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
      elog(ERROR, "native_query_shm: mmap failed: %m");
    (void) madvise(map, seg_size, MADV_HUGEPAGE); /* huge pages: fewer TLB misses */

    h = (NpShmHeader *) map;
    memcpy(h->magic, NP_SHM_MAGIC, 8);
    h->version = NP_SHM_VERSION;
    h->flags = 1; /* little-endian */
    h->capacity = seg_size;

    /* serialize directly into the shared segment (no intermediate buffer) */
    fill_columnar(SPI_tuptable, SPI_tuptable->tupdesc, SPI_processed,
                  cols, ncols, (char *) map + NP_SHM_HEADER_SIZE);

    __sync_synchronize();
    h->payload_size = total;
    __sync_synchronize();
    h->seq++;

    munmap(map, seg_size);
    pfree(cols);
  }

  SPI_finish();

  {
    char summary[256];
    snprintf(summary, sizeof(summary), "seg=%s payload=%llu bytes", seg,
             (unsigned long long) total);
    PG_RETURN_TEXT_P(cstring_to_text(summary));
  }
}

/* native_shm_read(segname) -> bytea : read the NPCOLUMN payload back out. */
PG_FUNCTION_INFO_V1(native_shm_read);
Datum
native_shm_read(PG_FUNCTION_ARGS)
{
  text *segname = PG_GETARG_TEXT_PP(0);
  char *seg = text_to_cstring(segname);
  char *path = shm_path(seg);
  int fd = shm_open(path, O_RDONLY, 0);
  struct stat st;
  void *map;
  NpShmHeader *h;
  size_t n;
  bytea *result;

  if (fd < 0)
    elog(ERROR, "native_shm_read: shm_open(%s) failed: %m", path);
  if (fstat(fd, &st) != 0)
  {
    close(fd);
    elog(ERROR, "native_shm_read: fstat failed: %m");
  }
  if ((size_t) st.st_size < NP_SHM_HEADER_SIZE)
  {
    close(fd);
    elog(ERROR, "native_shm_read: segment too small");
  }

  map = mmap(NULL, (size_t) st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (map == MAP_FAILED)
    elog(ERROR, "native_shm_read: mmap failed: %m");

  h = (NpShmHeader *) map;
  if (memcmp(h->magic, NP_SHM_MAGIC, 8) != 0)
  {
    munmap(map, (size_t) st.st_size);
    elog(ERROR, "native_shm_read: bad segment magic");
  }
  n = h->payload_size;
  if (NP_SHM_HEADER_SIZE + n > (size_t) st.st_size)
  {
    munmap(map, (size_t) st.st_size);
    elog(ERROR, "native_shm_read: payload exceeds segment");
  }

  result = (bytea *) palloc(VARHDRSZ + n);
  SET_VARSIZE(result, VARHDRSZ + n);
  memcpy(VARDATA(result), (char *) map + NP_SHM_HEADER_SIZE, n);
  munmap(map, (size_t) st.st_size);

  PG_RETURN_BYTEA_P(result);
}

/* native_shm_info(segname) -> text : segment header as a summary string. */
PG_FUNCTION_INFO_V1(native_shm_info);
Datum
native_shm_info(PG_FUNCTION_ARGS)
{
  text *segname = PG_GETARG_TEXT_PP(0);
  char *seg = text_to_cstring(segname);
  char *path = shm_path(seg);
  int fd = shm_open(path, O_RDONLY, 0);
  struct stat st;
  void *map;
  NpShmHeader *h;
  char summary[256];

  if (fd < 0)
    elog(ERROR, "native_shm_info: shm_open(%s) failed: %m", path);
  if (fstat(fd, &st) != 0)
  {
    close(fd);
    elog(ERROR, "native_shm_info: fstat failed: %m");
  }
  map = mmap(NULL, (size_t) st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (map == MAP_FAILED)
    elog(ERROR, "native_shm_info: mmap failed: %m");

  h = (NpShmHeader *) map;
  if (memcmp(h->magic, NP_SHM_MAGIC, 8) != 0)
  {
    munmap(map, (size_t) st.st_size);
    elog(ERROR, "native_shm_info: bad segment magic");
  }

  snprintf(summary, sizeof(summary), "payload=%llu capacity=%llu seq=%llu",
           (unsigned long long) h->payload_size,
           (unsigned long long) h->capacity, (unsigned long long) h->seq);
  munmap(map, (size_t) st.st_size);

  PG_RETURN_TEXT_P(cstring_to_text(summary));
}

/* --------------------------------------------------------------------- */
/* write path: native_write_shm(segname, tablename) -> bigint            */
/* Reads a columnar segment (same NPCOLUMN format as native_query_shm)   */
/* and bulk-inserts every row into `tablename` via table_multi_insert     */
/* (the COPY fast path — no per-row SQL, no text round-trip).            */
/* --------------------------------------------------------------------- */

typedef struct WCol {
  int kind;
  Oid atttypid;
  const uint8_t *nulls;
  const uint8_t *data;  /* fixed-width values, or offsets for text */
  const uint8_t *bytes; /* text bytes (NULL for fixed-width) */
} WCol;

static Datum
col_datum(WCol *col, uint64_t r, bool *isnull)
{
  *isnull = (col->nulls[r >> 3] & (1 << (r & 7))) != 0;
  if (*isnull)
    return (Datum) 0;

  switch (col->kind)
  {
    case NP_KIND_BOOL:
      return BoolGetDatum(col->data[r] != 0);

    case NP_KIND_INT32:
    {
      int32 v;
      memcpy(&v, col->data + r * 4, 4);
      switch (col->atttypid)
      {
        case INT2OID: return Int16GetDatum((int16) v);
        case INT8OID: return Int64GetDatum((int64) v);
        case FLOAT4OID: return Float4GetDatum((float4) v);
        case FLOAT8OID: return Float8GetDatum((double) v);
        default: return Int32GetDatum(v);
      }
    }

    case NP_KIND_INT64:
    {
      int64 v;
      memcpy(&v, col->data + r * 8, 8);
      switch (col->atttypid)
      {
        case INT2OID: return Int16GetDatum((int16) v);
        case INT4OID: return Int32GetDatum((int32) v);
        case FLOAT4OID: return Float4GetDatum((float4) v);
        case FLOAT8OID: return Float8GetDatum((double) v);
        default: return Int64GetDatum(v);
      }
    }

    case NP_KIND_FLOAT8:
    {
      double v;
      memcpy(&v, col->data + r * 8, 8);
      switch (col->atttypid)
      {
        case INT2OID: return Int16GetDatum((int16) v);
        case INT4OID: return Int32GetDatum((int32) v);
        case INT8OID: return Int64GetDatum((int64) v);
        case FLOAT4OID: return Float4GetDatum((float4) v);
        default: return Float8GetDatum(v);
      }
    }

    case NP_KIND_TEXT:
    {
      const uint32_t *off = (const uint32_t *) col->data;
      uint32_t s = off[r], e = off[r + 1];
      return PointerGetDatum(
          cstring_to_text_with_len((const char *) col->bytes + s, e - s));
    }

    default:
      return (Datum) 0;
  }
}

PG_FUNCTION_INFO_V1(native_write_shm);
Datum
native_write_shm(PG_FUNCTION_ARGS)
{
  text *segname = PG_GETARG_TEXT_PP(0);
  text *tabname = PG_GETARG_TEXT_PP(1);
  char *seg = text_to_cstring(segname);
  char *tab = text_to_cstring(tabname);
  char *path = shm_path(seg);

  int fd = shm_open(path, O_RDONLY, 0);
  struct stat st;
  void *map;
  NpShmHeader *h;
  const uint8_t *p;
  uint32_t ncols;
  uint64_t nrows;
  WCol *wcols;
  Relation rel;
  TupleDesc tupdesc;
  CommandId cid;
  BulkInsertState bistate;
  Datum *values;
  bool *nulls;
  TupleTableSlot **slots;
  MemoryContext batch_ctx;
  const int BATCH = 1000;
  uint64_t start;
  uint32_t c;

  if (fd < 0)
    elog(ERROR, "native_write_shm: shm_open(%s) failed: %m", path);
  if (fstat(fd, &st) != 0)
  {
    close(fd);
    elog(ERROR, "native_write_shm: fstat failed: %m");
  }
  map = mmap(NULL, (size_t) st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (map == MAP_FAILED)
    elog(ERROR, "native_write_shm: mmap failed: %m");
  (void) madvise(map, (size_t) st.st_size, MADV_HUGEPAGE);

  h = (NpShmHeader *) map;
  if (memcmp(h->magic, NP_SHM_MAGIC, 8) != 0)
  {
    munmap(map, (size_t) st.st_size);
    elog(ERROR, "native_write_shm: bad segment magic");
  }
  p = (const uint8_t *) map + NP_SHM_HEADER_SIZE;
  if (memcmp(p, "NPCOLUMN", 8) != 0)
  {
    munmap(map, (size_t) st.st_size);
    elog(ERROR, "native_write_shm: bad columnar magic");
  }
  p += 8;
  p += 4; /* version */
  p += 4; /* flags */
  memcpy(&ncols, p, 4); p += 4;
  memcpy(&nrows, p, 8); p += 8;

  rel = table_openrv(makeRangeVar(NULL, tab, -1), RowExclusiveLock);
  tupdesc = RelationGetDescr(rel);
  if (tupdesc->natts != (int) ncols)
  {
    table_close(rel, RowExclusiveLock);
    munmap(map, (size_t) st.st_size);
    elog(ERROR, "native_write_shm: column count mismatch (%d vs %u)",
         tupdesc->natts, ncols);
  }

  wcols = palloc0(sizeof(WCol) * ncols);
  for (c = 0; c < ncols; c++)
  {
    uint32_t name_len;
    memcpy(&name_len, p, 4); p += 4;
    p += name_len;
    wcols[c].kind = (int) *p++;
    p += 4; /* pgtype */
    wcols[c].nulls = p;
    p += (nrows + 7) / 8;
    {
      uint64_t data_len;
      memcpy(&data_len, p, 8); p += 8;
      wcols[c].data = p;
      if (wcols[c].kind == NP_KIND_TEXT)
        wcols[c].bytes = p + (nrows + 1) * 4;
      else
        wcols[c].bytes = NULL;
      p += data_len;
    }
    wcols[c].atttypid = TupleDescAttr(tupdesc, c)->atttypid;
  }

  cid = GetCurrentCommandId(false);
  values = palloc(sizeof(Datum) * ncols);
  nulls = palloc(sizeof(bool) * ncols);
  slots = palloc(sizeof(TupleTableSlot *) * BATCH);
  for (c = 0; c < (uint32) BATCH; c++)
    slots[c] = MakeSingleTupleTableSlot(tupdesc, &TTSOpsHeapTuple);
  batch_ctx = AllocSetContextCreate(CurrentMemoryContext, "native_write_shm",
                                    ALLOCSET_DEFAULT_SIZES);
  bistate = GetBulkInsertState();

  for (start = 0; start < nrows; start += BATCH)
  {
    int n = (int) Min((uint64) BATCH, nrows - start);
    MemoryContext old = MemoryContextSwitchTo(batch_ctx);
    int i;
    for (i = 0; i < n; i++)
    {
      uint64_t r = start + i;
      HeapTuple tuple;
      for (c = 0; c < ncols; c++)
        values[c] = col_datum(&wcols[c], r, &nulls[c]);
      tuple = heap_form_tuple(tupdesc, values, nulls);
      ExecStoreHeapTuple(tuple, slots[i], false);
    }
    table_multi_insert(rel, slots, n, cid, 0, bistate);
    MemoryContextSwitchTo(old);
    MemoryContextReset(batch_ctx);
  }

  FreeBulkInsertState(bistate);
  for (c = 0; c < (uint32) BATCH; c++)
    ExecDropSingleTupleTableSlot(slots[c]);

  table_close(rel, RowExclusiveLock);
  pfree(wcols);
  pfree(values);
  pfree(nulls);
  pfree(slots);
  MemoryContextDelete(batch_ctx);
  munmap(map, (size_t) st.st_size);

  PG_RETURN_INT64((int64) nrows);
}

/* comparison for the supported predicate types; returns -1/0/1 */
static int
compare_datum(Oid type, Datum a, Datum b)
{
  switch (type)
  {
    case INT2OID:
      return (DatumGetInt16(a) > DatumGetInt16(b)) - (DatumGetInt16(a) < DatumGetInt16(b));
    case INT4OID:
      return (DatumGetInt32(a) > DatumGetInt32(b)) - (DatumGetInt32(a) < DatumGetInt32(b));
    case INT8OID:
      return (DatumGetInt64(a) > DatumGetInt64(b)) - (DatumGetInt64(a) < DatumGetInt64(b));
    case FLOAT4OID:
      return (DatumGetFloat4(a) > DatumGetFloat4(b)) - (DatumGetFloat4(a) < DatumGetFloat4(b));
    case FLOAT8OID:
      return (DatumGetFloat8(a) > DatumGetFloat8(b)) - (DatumGetFloat8(a) < DatumGetFloat8(b));
    case BOOLOID:
      return (DatumGetBool(a) ? 1 : 0) - (DatumGetBool(b) ? 1 : 0);
    case TEXTOID:
    case VARCHAROID:
    case BPCHAROID:
    {
      int la = VARSIZE_ANY_EXHDR(a);
      int lb = VARSIZE_ANY_EXHDR(b);
      int n = la < lb ? la : lb;
      int cmp = memcmp(VARDATA_ANY(a), VARDATA_ANY(b), n);
      if (cmp != 0)
        return cmp;
      return (la > lb) - (la < lb);
    }
    default:
      return 0;
  }
}

/* a simple predicate: "<column> <op> <value>", e.g. "age >= 18" */
typedef struct Pred {
  int attnum;
  Oid type;
  char op; /* '=' '<' '>' 'L'(<=) 'G'(>=) '!'(!=) */
  Datum value;
} Pred;

static bool
parse_predicate(const char *s, Pred *pred, TupleDesc td)
{
  char colname[64];
  char opstr[3];
  char val[256];
  int n = sscanf(s, "%63s %2s %255s", colname, opstr, val);
  int i;
  Oid typinput;
  Oid typioparam;

  if (n != 3)
    return false;

  if (strcmp(opstr, "=") == 0) pred->op = '=';
  else if (strcmp(opstr, "<") == 0) pred->op = '<';
  else if (strcmp(opstr, ">") == 0) pred->op = '>';
  else if (strcmp(opstr, "<=") == 0) pred->op = 'L';
  else if (strcmp(opstr, ">=") == 0) pred->op = 'G';
  else if (strcmp(opstr, "!=") == 0 || strcmp(opstr, "<>") == 0) pred->op = '!';
  else
    return false;

  for (i = 0; i < td->natts; i++)
  {
    Form_pg_attribute att = TupleDescAttr(td, i);
    if (strcmp(NameStr(att->attname), colname) == 0)
    {
      pred->attnum = i + 1;
      pred->type = att->atttypid;
      break;
    }
  }
  if (i == td->natts)
    return false;

  getTypeInputInfo(pred->type, &typinput, &typioparam);
  pred->value = OidInputFunctionCall(typinput, val, typioparam, -1);
  return true;
}

static bool
pred_matches(Pred *pred, HeapTuple tuple, TupleDesc td)
{
  bool isnull;
  Datum d = heap_getattr(tuple, pred->attnum, td, &isnull);
  int cmp;
  if (isnull)
    return false;
  cmp = compare_datum(pred->type, d, pred->value);
  switch (pred->op)
  {
    case '=':
      return cmp == 0;
    case '<':
      return cmp < 0;
    case '>':
      return cmp > 0;
    case 'L':
      return cmp <= 0;
    case 'G':
      return cmp >= 0;
    case '!':
      return cmp != 0;
    default:
      return false;
  }
}

/*
 * native_scan_shm(segname, tablename [, columns, predicate]) -> text
 * Direct heap scan of a table — no SQL executor, no SPI. As each matching
 * tuple is found, its columns are written straight into the columnar /dev/shm
 * segment. `columns` (text[]) selects/orders columns; `predicate` is a simple
 * "<column> <op> <value>" filter. Two passes: (1) count + size, (2) serialize.
 */
PG_FUNCTION_INFO_V1(native_scan_shm);
Datum
native_scan_shm(PG_FUNCTION_ARGS)
{
  text *segname = PG_GETARG_TEXT_PP(0);
  text *tabname = PG_GETARG_TEXT_PP(1);
  char *seg = text_to_cstring(segname);
  char *tab = text_to_cstring(tabname);
  char *path = shm_path(seg);
  bool have_cols = PG_NARGS() > 2 && !PG_ARGISNULL(2);
  bool have_pred = PG_NARGS() > 3 && !PG_ARGISNULL(3);
  Relation rel;
  TupleDesc td;
  int *attnums;
  int ncols;
  Pred pred;
  bool use_pred;
  Snapshot snap;
  ColInfo *cols;
  uint64 *text_bytes;
  uint64 nrows = 0;
  uint64 total;
  int c;
  uint64 r;
  TableScanDesc scan;
  HeapTuple tuple;
  int fd;
  struct stat st;
  size_t seg_size;
  void *map;
  NpShmHeader *h;
  char *p;
  char **col_ptr;
  uint32 **text_off;
  char **nulls;
  char summary[256];

  rel = table_openrv(makeRangeVar(NULL, tab, -1), AccessShareLock);
  td = RelationGetDescr(rel);
  snap = GetActiveSnapshot();

  /* column selection (default: all columns, in table order) */
  attnums = palloc(sizeof(int) * td->natts);
  ncols = 0;
  if (have_cols)
  {
    ArrayType *arr = PG_GETARG_ARRAYTYPE_P(2);
    Datum *elems;
    bool *enulls;
    int nelems;
    int i;
    deconstruct_array(arr, TEXTOID, -1, false, 'i', &elems, &enulls, &nelems);
    for (i = 0; i < nelems; i++)
    {
      char *cname;
      int an;
      if (enulls[i])
        elog(ERROR, "native_scan_shm: NULL column name");
      cname = TextDatumGetCString(elems[i]);
      an = get_attnum(RelationGetRelid(rel), cname);
      if (an == InvalidAttrNumber)
        elog(ERROR, "native_scan_shm: no such column \"%s\"", cname);
      attnums[ncols++] = an;
    }
  }
  else
  {
    for (c = 0; c < td->natts; c++)
      attnums[ncols++] = c + 1;
  }

  /* optional "<column> <op> <value>" predicate */
  use_pred = false;
  if (have_pred)
  {
    char *ps = text_to_cstring(PG_GETARG_TEXT_PP(3));
    if (!parse_predicate(ps, &pred, td))
      elog(ERROR, "native_scan_shm: bad predicate \"%s\"", ps);
    use_pred = true;
  }

  cols = palloc0(sizeof(ColInfo) * ncols);
  text_bytes = palloc0(sizeof(uint64) * ncols);
  col_ptr = palloc0(sizeof(char *) * ncols);
  text_off = palloc0(sizeof(uint32 *) * ncols);
  nulls = palloc0(sizeof(char *) * ncols);

  for (c = 0; c < ncols; c++)
  {
    Form_pg_attribute att = TupleDescAttr(td, attnums[c] - 1);
    cols[c].name = NameStr(att->attname);
    cols[c].attnum = attnums[c];
    cols[c].type = att->atttypid;
    cols[c].kind = oid_to_kind(att->atttypid);
    cols[c].width = kind_width(cols[c].kind);
    if (cols[c].kind == NP_KIND_TEXT && !is_varlena_text(att->atttypid))
    {
      bool varlena;
      getTypeOutputInfo(att->atttypid, &cols[c].typoutput, &varlena);
    }
    else
      cols[c].typoutput = InvalidOid;
  }

  /* pass 1: count rows + total text bytes per column */
  scan = table_beginscan(rel, snap, 0, NULL);
  while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
  {
    if (use_pred && !pred_matches(&pred, tuple, td))
      continue;
    nrows++;
    for (c = 0; c < ncols; c++)
    {
      bool isnull;
      const char *data;
      int len;
      if (cols[c].kind != NP_KIND_TEXT)
        continue;
      text_cell(tuple, td, cols[c].attnum - 1, cols[c].type, cols[c].typoutput,
                &isnull, &data, &len);
      if (!isnull)
        text_bytes[c] += (uint64) len;
    }
  }
  table_endscan(scan);

  total = 8 + 4 + 4 + 4 + 8;
  for (c = 0; c < ncols; c++)
  {
    cols[c].null_bytes = (int) ((nrows + 7) / 8);
    total += 4 + (uint64) strlen(cols[c].name) + 1 + 4 + cols[c].null_bytes + 8;
    if (cols[c].kind == NP_KIND_TEXT)
      cols[c].data_len = (uint64) (nrows + 1) * 4 + text_bytes[c];
    else
      cols[c].data_len = (uint64) nrows * cols[c].width;
    total += cols[c].data_len;
  }

  seg_size = NP_SHM_HEADER_SIZE + total;
  fd = shm_open(path, O_RDWR | O_CREAT, 0666);
  if (fd < 0)
    elog(ERROR, "native_scan_shm: shm_open(%s) failed: %m", path);
  fchmod(fd, 0666);
  if (fstat(fd, &st) != 0)
  {
    close(fd);
    elog(ERROR, "native_scan_shm: fstat failed: %m");
  }
  if ((size_t) st.st_size < seg_size)
  {
    if (ftruncate(fd, (off_t) seg_size) != 0)
    {
      close(fd);
      elog(ERROR, "native_scan_shm: ftruncate failed: %m");
    }
  }
  map = mmap(NULL, seg_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (map == MAP_FAILED)
    elog(ERROR, "native_scan_shm: mmap failed: %m");
  (void) madvise(map, seg_size, MADV_HUGEPAGE);

  /* header + per-column headers, and per-column write cursors */
  p = (char *) map + NP_SHM_HEADER_SIZE;
  memcpy(p, NP_MAGIC, 8); p += 8;
  put_u32(p, NP_VERSION); p += 4;
  put_u32(p, 1); p += 4;
  put_u32(p, (uint32) ncols); p += 4;
  put_u64(p, nrows); p += 8;

  for (c = 0; c < ncols; c++)
  {
    put_u32(p, (uint32) strlen(cols[c].name)); p += 4;
    memcpy(p, cols[c].name, strlen(cols[c].name)); p += strlen(cols[c].name);
    *p++ = (char) cols[c].kind;
    put_u32(p, (uint32) cols[c].type); p += 4;
    nulls[c] = p;
    memset(p, 0, cols[c].null_bytes); p += cols[c].null_bytes;
    put_u64(p, cols[c].data_len); p += 8;
    if (cols[c].kind == NP_KIND_TEXT)
    {
      text_off[c] = (uint32 *) p;
      text_off[c][0] = 0;
      p += (nrows + 1) * 4; /* skip offsets */
      col_ptr[c] = p;       /* bytes cursor */
      p += text_bytes[c];   /* skip bytes */
    }
    else
    {
      col_ptr[c] = p;
      p += cols[c].data_len;
    }
  }

  /* pass 2: scan again, write each column straight into the segment */
  scan = table_beginscan(rel, snap, 0, NULL);
  r = 0;
  while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
  {
    if (use_pred && !pred_matches(&pred, tuple, td))
      continue;
    for (c = 0; c < ncols; c++)
    {
      bool isnull;
      Datum d = heap_getattr(tuple, cols[c].attnum, td, &isnull);
      if (isnull)
      {
        nulls[c][r >> 3] |= (char) (1 << (r & 7));
        if (cols[c].kind == NP_KIND_TEXT)
          text_off[c][r + 1] = text_off[c][r];
        continue;
      }
      switch (cols[c].kind)
      {
        case NP_KIND_BOOL:
          *col_ptr[c] = DatumGetBool(d) ? 1 : 0;
          col_ptr[c] += 1;
          break;
        case NP_KIND_INT32:
        {
          int32 v = (cols[c].type == INT2OID) ? (int32) DatumGetInt16(d)
                                              : DatumGetInt32(d);
          memcpy(col_ptr[c], &v, 4);
          col_ptr[c] += 4;
          break;
        }
        case NP_KIND_INT64:
        {
          int64 v = DatumGetInt64(d);
          memcpy(col_ptr[c], &v, 8);
          col_ptr[c] += 8;
          break;
        }
        case NP_KIND_FLOAT8:
        {
          double v;
          switch (cols[c].type)
          {
            case FLOAT4OID:
              v = (double) DatumGetFloat4(d);
              break;
            case FLOAT8OID:
              v = DatumGetFloat8(d);
              break;
            case NUMERICOID:
              v = DatumGetFloat8(DirectFunctionCall1(numeric_float8, d));
              break;
            default:
              v = 0.0;
              break;
          }
          memcpy(col_ptr[c], &v, 8);
          col_ptr[c] += 8;
          break;
        }
        case NP_KIND_TEXT:
        {
          const char *data;
          int len;
          if (is_varlena_text(cols[c].type))
          {
            data = VARDATA_ANY(d);
            len = VARSIZE_ANY_EXHDR(d);
          }
          else
          {
            data = OidOutputFunctionCall(cols[c].typoutput, d);
            len = (int) strlen(data);
          }
          memcpy(col_ptr[c], data, len);
          col_ptr[c] += len;
          if (!is_varlena_text(cols[c].type))
            pfree((void *) data);
          text_off[c][r + 1] = text_off[c][r] + (uint32) len;
          break;
        }
      }
    }
    r++;
  }
  table_endscan(scan);

  h = (NpShmHeader *) map;
  memcpy(h->magic, NP_SHM_MAGIC, 8);
  h->version = NP_SHM_VERSION;
  h->flags = 1;
  h->capacity = seg_size;
  __sync_synchronize();
  h->payload_size = total;
  __sync_synchronize();
  h->seq++;
  munmap(map, seg_size);

  table_close(rel, AccessShareLock);

  snprintf(summary, sizeof(summary), "seg=%s nrows=%llu payload=%llu bytes", seg,
           (unsigned long long) nrows, (unsigned long long) total);
  PG_RETURN_TEXT_P(cstring_to_text(summary));
}
