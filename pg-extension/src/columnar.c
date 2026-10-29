/* columnar.c — serialize a result set into the NPCOLUMN payload. */
#include "postgres.h"

#include <string.h>
#include <sys/mman.h>

#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "varatt.h"

#include "np_shm.h"

#include "columnar.h"
#include "shm.h"

int
np_oid_to_kind(Oid typ)
{
  switch (typ)
  {
    case BOOLOID:
      return NP_KIND_BOOL;
    case INT2OID:
      return NP_KIND_INT16; /* copied verbatim, not widened */
    case INT4OID:
      return NP_KIND_INT32;
    case INT8OID:
      return NP_KIND_INT64;
    case FLOAT4OID:
      return NP_KIND_FLOAT4; /* copied verbatim, not widened */
    case FLOAT8OID:
    case NUMERICOID: /* numeric has no wire kind: converted, lossily */
      return NP_KIND_FLOAT8;
    default:
      return NP_KIND_TEXT; /* varlena text, or an output-function fallback */
  }
}

bool
np_is_text_type(Oid typ)
{
  return typ == TEXTOID || typ == VARCHAROID || typ == BPCHAROID;
}

/* Text bytes come straight off the varlena; every other type goes through its
 * output function. */
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
  if (np_is_text_type(type))
  {
    struct varlena *v =
        pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));

    *data = VARDATA_ANY(v);
    *len = VARSIZE_ANY_EXHDR(v);
  }
  else
  {
    char *s = OidOutputFunctionCall(typoutput, d);
    *data = s ? s : "";
    *len = s ? (int) strlen(s) : 0;
  }
}

/* One pass into [out, end); `data_len` is backpatched once the column's bytes
 * are known. Returns the payload size written. */
uint64
np_columnar_build(HeapTuple *tuples, TupleDesc td, uint64 nrows, char *out,
                  char *end)
{
  char *p = out, *base = out;
  int ncols = td->natts;
  int null_bytes = (int) ((nrows + 7) / 8);
  int c;
  uint64 r;

  memcpy(p, NP_COLUMNAR_MAGIC, 8); p += 8;
  np_put_u32(p, NP_COLUMNAR_VERSION); p += 4;
  np_put_u32(p, 1); p += 4; /* flags: little-endian */
  np_put_u32(p, (uint32) ncols); p += 4;
  np_put_u64(p, nrows); p += 8;

  for (c = 0; c < ncols; c++)
  {
    Form_pg_attribute att = TupleDescAttr(td, c);
    Oid type = att->atttypid;
    int kind = np_oid_to_kind(type);
    int width = np_kind_width((uint8_t) kind);
    Oid typoutput = InvalidOid;
    size_t name_len = strlen(NameStr(att->attname));
    char *nulls, *lenp;

    if (kind == NP_KIND_TEXT && !np_is_text_type(type))
    {
      bool varlena;
      getTypeOutputInfo(type, &typoutput, &varlena);
    }

    if (p + 4 + name_len + 1 + 4 + (size_t) null_bytes + 8 + 8 > end)
      elog(ERROR, "nativepostgress: result exceeds the segment");

    np_put_u32(p, (uint32) name_len); p += 4;
    memcpy(p, NameStr(att->attname), name_len); p += name_len;
    *p++ = (char) kind;
    np_put_u32(p, (uint32) type); p += 4;
    nulls = p;
    memset(nulls, 0, null_bytes);
    p += null_bytes;
    lenp = p;
    p += 8; /* data_len, backpatched below */
    while (((size_t) (p - base)) & 7) *p++ = 0; /* 8-byte aligned column data */

    if (kind == NP_KIND_TEXT)
    {
      uint32 *off;
      char *bytes;

      if (p + (uint64) (nrows + 1) * 4 > end)
        elog(ERROR, "nativepostgress: result exceeds the segment");
      off = (uint32 *) p;
      p += (nrows + 1) * 4;
      bytes = p;
      off[0] = 0;
      for (r = 0; r < nrows; r++)
      {
        bool isnull;
        const char *d;
        int len;
        text_cell(tuples[r], td, c, type, typoutput, &isnull, &d, &len);
        if (isnull)
        {
          nulls[r >> 3] |= (char) (1 << (r & 7));
          off[r + 1] = off[r];
        }
        else
        {
          if (p + len > end)
            elog(ERROR, "nativepostgress: result exceeds the segment");
          memcpy(p, d, len);
          p += len;
          off[r + 1] = (uint32) (p - bytes);
        }
      }
      np_put_u64(lenp, (uint64) (nrows + 1) * 4 + (uint64) (p - bytes));
    }
    else
    {
      if (p + (uint64) nrows * (uint64) width > end)
        elog(ERROR, "nativepostgress: result exceeds the segment");
      for (r = 0; r < nrows; r++)
      {
        bool isnull;
        Datum d = heap_getattr(tuples[r], c + 1, td, &isnull);
        if (isnull)
        {
          nulls[r >> 3] |= (char) (1 << (r & 7));
          continue;
        }
        switch (kind)
        {
          case NP_KIND_BOOL:
            *p = DatumGetBool(d) ? 1 : 0;
            break;
          case NP_KIND_INT16:
          {
            int16 v = DatumGetInt16(d);
            memcpy(p, &v, 2);
            break;
          }
          case NP_KIND_INT32:
          {
            int32 v = DatumGetInt32(d);
            memcpy(p, &v, 4);
            break;
          }
          case NP_KIND_INT64:
          {
            int64 v = DatumGetInt64(d);
            memcpy(p, &v, 8);
            break;
          }
          case NP_KIND_FLOAT4:
          {
            float4 v = DatumGetFloat4(d);
            memcpy(p, &v, 4);
            break;
          }
          case NP_KIND_FLOAT8:
          {
            double v = (type == NUMERICOID)
                           ? DatumGetFloat8(DirectFunctionCall1(numeric_float8, d))
                           : DatumGetFloat8(d);
            memcpy(p, &v, 8);
            break;
          }
        }
        p += width;
      }
      np_put_u64(lenp, (uint64) nrows * (uint64) width);
    }
  }

  return (uint64) (p - base);
}

uint64
np_publish_columnar_shm(const char *path, TupleDesc td, HeapTuple *tuples,
                        uint64 nrows, size_t capacity)
{
  size_t cap = capacity ? capacity : NP_SEGMENT_BYTES;
  void *map = np_shm_map_rw(path, cap);
  uint64 total = np_columnar_build(tuples, td, nrows,
                                   (char *) map + NP_SHM_HEADER_SIZE,
                                   (char *) map + cap);

  np_shm_publish(map, cap, total);
  munmap(map, cap);
  return total;
}
