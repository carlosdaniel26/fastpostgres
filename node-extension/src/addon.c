/*
 * addon.c — Node client for the nativepostgress shared-memory transport.
 *
 * Raw N-API (node_api.h, no node-addon-api):
 *   new Connection(conninfo)  libpq connection for the control channel
 *   conn.exec(sql)            run a statement, return rows as objects
 *   read(segName, opts)       view a columnar segment as JS typed arrays
 *
 * read() is zero-copy by default: the typed arrays alias the mapped segment, so
 * they are only valid until the segment is rewritten. Pass { copy: true } to
 * detach into Node-owned buffers.
 */
#include <node_api.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libpq-fe.h>

#include "np_shm.h"

#include "synth.h"

#define NP_ERR_MAX 1024

static napi_value mk_undefined(napi_env env) {
  napi_value v;
  napi_get_undefined(env, &v);
  return v;
}

static napi_value mk_i32(napi_env env, int32_t x) {
  napi_value v;
  napi_create_int32(env, x, &v);
  return v;
}

static bool is_function(napi_env env, napi_value v) {
  napi_valuetype t;
  if (napi_typeof(env, v, &t) != napi_ok) return false;
  return t == napi_function;
}

/* ---- libpq connection ---------------------------------------------------- */

typedef struct {
  PGconn *pg;
} Connection;

static void conn_finalize(napi_env env, void *data, void *hint) {
  Connection *c = data;
  (void) env;
  (void) hint;
  if (!c) return;
  if (c->pg) PQfinish(c->pg);
  free(c);
}

static napi_value conn_new(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1], self;
  char conninfo[1024] = "";
  Connection *c;
  PGconn *pg;

  napi_get_cb_info(env, info, &argc, argv, &self, NULL);
  if (argc >= 1) {
    size_t len = 0;
    if (napi_get_value_string_utf8(env, argv[0], conninfo, sizeof(conninfo),
                                   &len) != napi_ok) {
      napi_throw_type_error(env, NULL, "conninfo must be a string");
      return mk_undefined(env);
    }
  }

  pg = PQconnectdb(conninfo);
  if (PQstatus(pg) != CONNECTION_OK) {
    char msg[NP_ERR_MAX];
    snprintf(msg, sizeof(msg), "libpq: %s", PQerrorMessage(pg));
    PQfinish(pg);
    napi_throw_error(env, NULL, msg);
    return mk_undefined(env);
  }

  c = calloc(1, sizeof(*c));
  c->pg = pg;
  napi_wrap(env, self, c, conn_finalize, NULL, NULL);
  return self;
}

static napi_value conn_exec(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1], self, arr;
  Connection *c = NULL;
  char *sql;
  size_t len = 0;
  PGresult *res;
  ExecStatusType st;
  int nrows, ncols, r, f;

  napi_get_cb_info(env, info, &argc, argv, &self, NULL);
  napi_unwrap(env, self, (void **) &c);
  if (!c || !c->pg) {
    napi_throw_error(env, NULL, "connection is closed");
    return mk_undefined(env);
  }

  napi_get_value_string_utf8(env, argv[0], NULL, 0, &len);
  sql = malloc(len + 1);
  napi_get_value_string_utf8(env, argv[0], sql, len + 1, &len);

  res = PQexec(c->pg, sql);
  free(sql);

  st = PQresultStatus(res);
  if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
    char msg[NP_ERR_MAX];
    snprintf(msg, sizeof(msg), "libpq: %s", PQresultErrorMessage(res));
    PQclear(res);
    napi_throw_error(env, NULL, msg);
    return mk_undefined(env);
  }

  nrows = PQntuples(res);
  ncols = PQnfields(res);
  napi_create_array_with_length(env, (size_t) nrows, &arr);
  for (r = 0; r < nrows; r++) {
    napi_value row;
    napi_create_object(env, &row);
    for (f = 0; f < ncols; f++) {
      napi_value v;
      if (PQgetisnull(res, r, f))
        napi_get_null(env, &v);
      else
        napi_create_string_utf8(env, PQgetvalue(res, r, f), NAPI_AUTO_LENGTH,
                                &v);
      napi_set_named_property(env, row, PQfname(res, f), v);
    }
    napi_set_element(env, arr, (uint32_t) r, row);
  }
  PQclear(res);
  return arr;
}

static napi_value conn_close(napi_env env, napi_callback_info info) {
  size_t argc = 0;
  napi_value self;
  Connection *c = NULL;

  napi_get_cb_info(env, info, &argc, NULL, &self, NULL);
  napi_unwrap(env, self, (void **) &c);
  if (c && c->pg) {
    PQfinish(c->pg);
    c->pg = NULL;
  }
  return mk_undefined(env);
}

/* ---- read(segName[, options]) -------------------------------------------- */

/* Owns the mapping once it has been handed to an external ArrayBuffer. */
typedef struct {
  void *base;
  size_t size;
} Mapping;

static void mapping_finalize(napi_env env, void *data, void *hint) {
  Mapping *m = hint;
  (void) env;
  (void) data;
  if (m) {
    munmap(m->base, m->size);
    free(m);
  }
}

static napi_value typed_over(napi_env env, napi_value ab,
                             napi_typedarray_type type, size_t count,
                             size_t offset) {
  napi_value v;
  if (napi_create_typedarray(env, type, count, ab, offset, &v) != napi_ok)
    return NULL;
  return v;
}

static napi_value read_impl(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2], opts, handlers = NULL, on_kind = NULL, on_column = NULL;
  char name[256];
  size_t nlen = sizeof(name);
  char path[300];
  int fd;
  struct stat st;
  size_t size;
  void *map;
  const uint8_t *payload;
  uint64_t payload_len = 0;
  NpReader rd;
  NpColumn col;
  bool copy = false;
  napi_value columns, result, ext_ab = NULL;
  uint32_t c;

  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 1) {
    napi_throw_type_error(env, NULL, "read(segName[, options])");
    return mk_undefined(env);
  }

  napi_get_value_string_utf8(env, argv[0], name, sizeof(name), &nlen);
  snprintf(path, sizeof(path), "/%s", name);

  if (argc >= 2) {
    napi_valuetype ot;
    napi_typeof(env, argv[1], &ot);
    if (ot == napi_object) {
      bool b;
      napi_value v;
      opts = argv[1];
      if (napi_get_named_property(env, opts, "copy", &v) == napi_ok &&
          napi_get_value_bool(env, v, &b) == napi_ok)
        copy = b;
      if (napi_get_named_property(env, argv[1], "handlers", &handlers) == napi_ok) {
        napi_valuetype ht;
        napi_typeof(env, handlers, &ht);
        if (ht == napi_object) {
          napi_valuetype kt;
          napi_get_named_property(env, handlers, "onKind", &on_kind);
          if (napi_typeof(env, on_kind, &kt) != napi_ok || kt != napi_object)
            on_kind = NULL;
          napi_get_named_property(env, handlers, "onColumn", &on_column);
          if (napi_typeof(env, on_column, &kt) != napi_ok ||
              kt != napi_object)
            on_column = NULL;
        }
      }
    }
  }

  fd = shm_open(path, O_RDONLY, 0);
  if (fd < 0) {
    napi_throw_error(env, NULL, "cannot open /dev/shm segment");
    return mk_undefined(env);
  }
  if (fstat(fd, &st) != 0) {
    close(fd);
    napi_throw_error(env, NULL, "fstat failed");
    return mk_undefined(env);
  }
  size = (size_t) st.st_size;
  map = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (map == MAP_FAILED) {
    napi_throw_error(env, NULL, "mmap failed");
    return mk_undefined(env);
  }

  if (!np_shm_validate(map, size, &payload_len) ||
      !np_reader_init(&rd, (const uint8_t *) map + NP_SHM_HEADER_SIZE,
                      payload_len)) {
    munmap(map, size);
    napi_throw_error(env, NULL, "not a nativepostgress segment");
    return mk_undefined(env);
  }
  payload = (const uint8_t *) map + NP_SHM_HEADER_SIZE;

  if (!copy) {
    Mapping *m = malloc(sizeof(*m));
    m->base = map;
    m->size = size;
    if (napi_create_external_arraybuffer(env, (void *) payload, payload_len,
                                         mapping_finalize, m, &ext_ab) !=
        napi_ok) {
      free(m);
      munmap(map, size);
      napi_throw_error(env, NULL, "cannot create arraybuffer");
      return mk_undefined(env);
    }
  }

  napi_create_array_with_length(env, rd.ncols, &columns);

  for (c = 0; c < rd.ncols; c++) {
    napi_value entry, jname, ab = NULL, value = NULL;
    size_t base_off = 0;
    napi_value cb = NULL;

    if (!np_reader_next(&rd, &col)) break;

    napi_create_string_utf8(env, col.name, col.name_len, &jname);
    if (!copy) base_off = (size_t) (col.data - payload);

    if (col.kind == NP_KIND_TEXT) {
      size_t offs_bytes = (size_t) (col.nrows + 1) * 4;
      size_t text_bytes = (size_t) col.data_len - offs_bytes;
      napi_value bytes_v, offsets_v;

      if (copy) {
        void *dst;
        napi_create_arraybuffer(env, (size_t) col.data_len, &dst, &ab);
        memcpy(dst, col.data, (size_t) col.data_len);
      } else {
        ab = ext_ab;
      }
      bytes_v = typed_over(env, ab, napi_uint8_array, text_bytes,
                           base_off + offs_bytes);
      offsets_v = typed_over(env, ab, napi_int32_array, (size_t) col.nrows + 1,
                             base_off);
      if (bytes_v == NULL || offsets_v == NULL) {
        if (copy) munmap(map, size);
        napi_throw_error(env, NULL,
                         "column data is not aligned for a typed array");
        return mk_undefined(env);
      }
      napi_create_object(env, &value);
      napi_set_named_property(env, value, "bytes", bytes_v);
      napi_set_named_property(env, value, "offsets", offsets_v);
    } else {
      napi_typedarray_type type;
      switch (col.kind) {
        case NP_KIND_BOOL: type = napi_uint8_array; break;
        case NP_KIND_INT16: type = napi_int16_array; break;
        case NP_KIND_INT32: type = napi_int32_array; break;
        case NP_KIND_INT64: type = napi_bigint64_array; break;
        case NP_KIND_FLOAT4: type = napi_float32_array; break;
        default: type = napi_float64_array; break;
      }
      if (copy) {
        void *dst;
        napi_create_arraybuffer(env, (size_t) col.data_len, &dst, &ab);
        memcpy(dst, col.data, (size_t) col.data_len);
      } else {
        ab = ext_ab;
      }
      value = typed_over(env, ab, type, (size_t) col.nrows, base_off);
      if (value == NULL) {
        if (copy) munmap(map, size);
        napi_throw_error(env, NULL,
                         "column data is not aligned for a typed array");
        return mk_undefined(env);
      }
    }

    napi_create_object(env, &entry);
    napi_set_named_property(env, entry, "name", jname);
    napi_set_named_property(env, entry, "pgtype", mk_i32(env, (int32_t) col.pgtype));
    napi_set_named_property(env, entry, "kind", mk_i32(env, (int32_t) col.kind));
    napi_set_named_property(env, entry, "value", value);
    napi_set_element(env, columns, c, entry);

    /* dispatch: onColumn[name] wins over onKind[kind] */
    if (on_column) {
      napi_value fn;
      if (napi_get_property(env, on_column, jname, &fn) == napi_ok &&
          is_function(env, fn))
        cb = fn;
    }
    if (cb == NULL && on_kind) {
      char kbuf[16];
      napi_value key, fn;
      snprintf(kbuf, sizeof(kbuf), "%d", col.kind);
      napi_create_string_utf8(env, kbuf, NAPI_AUTO_LENGTH, &key);
      if (napi_get_property(env, on_kind, key, &fn) == napi_ok &&
          is_function(env, fn))
        cb = fn;
    }
    if (cb) {
      napi_value args[2] = {entry, value};
      napi_value ignored;
      bool pending = false;
      napi_call_function(env, mk_undefined(env), cb, 2, args, &ignored);
      napi_is_exception_pending(env, &pending);
      if (pending) {
        if (copy) munmap(map, size);
        return mk_undefined(env);
      }
    }
  }

  if (copy) munmap(map, size);

  napi_create_object(env, &result);
  {
    napi_value rc;
    napi_create_double(env, (double) rd.nrows, &rc);
    napi_set_named_property(env, result, "rowCount", rc);
  }
  napi_set_named_property(env, result, "columns", columns);
  return result;
}

/* ---- conn.copyFrom(sql, data) -------------------------------------------- */

/* Stream `n` bytes with libpq's COPY protocol, retrying while it needs flushing. */
static int copy_put(PGconn *pg, const char *buf, size_t n, char *err,
                    size_t errsz) {
  size_t off = 0;
  while (off < n) {
    size_t chunk = n - off;
    int rc;
    if (chunk > (size_t) (1 << 20)) chunk = 1 << 20;
    rc = PQputCopyData(pg, buf + off, (int) chunk);
    if (rc < 0) {
      snprintf(err, errsz, "%s", PQerrorMessage(pg));
      return -1;
    }
    if (rc == 0) {
      if (PQflush(pg) != 0) {
        snprintf(err, errsz, "%s", PQerrorMessage(pg));
        return -1;
      }
      continue; /* retry the same chunk */
    }
    off += chunk;
  }
  return 0;
}

static napi_value conn_copy_from(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2], self;
  Connection *c = NULL;
  char *sql = NULL, *str = NULL;
  size_t sql_len = 0, str_len = 0;
  bool is_buf = false, is_arr = false;
  PGresult *res;
  char err[NP_ERR_MAX] = "";

  napi_get_cb_info(env, info, &argc, argv, &self, NULL);
  napi_unwrap(env, self, (void **) &c);
  if (!c || !c->pg) {
    napi_throw_error(env, NULL, "connection is closed");
    return mk_undefined(env);
  }
  if (argc < 2) {
    napi_throw_type_error(env, NULL, "copyFrom(sql, data)");
    return mk_undefined(env);
  }

  napi_get_value_string_utf8(env, argv[0], NULL, 0, &sql_len);
  sql = malloc(sql_len + 1);
  napi_get_value_string_utf8(env, argv[0], sql, sql_len + 1, &sql_len);

  res = PQexec(c->pg, sql);
  if (PQresultStatus(res) != PGRES_COPY_IN) {
    snprintf(err, sizeof(err), "libpq: %s", PQresultErrorMessage(res));
    PQclear(res);
    free(sql);
    napi_throw_error(env, NULL, err);
    return mk_undefined(env);
  }
  PQclear(res);

  napi_is_buffer(env, argv[1], &is_buf);
  napi_is_array(env, argv[1], &is_arr);
  if (is_arr) {
    uint32_t len = 0, i;
    napi_get_array_length(env, argv[1], &len);
    for (i = 0; i < len; i++) {
      napi_value e;
      void *p = NULL;
      size_t n = 0;
      napi_get_element(env, argv[1], i, &e);
      if (napi_get_buffer_info(env, e, &p, &n) != napi_ok) {
        snprintf(err, sizeof(err), "copyFrom: array entries must be Buffers");
        goto fail;
      }
      if (n && copy_put(c->pg, p, n, err, sizeof(err)) != 0) goto fail;
    }
  } else if (is_buf) {
    void *p = NULL;
    size_t n = 0;
    napi_get_buffer_info(env, argv[1], &p, &n);
    if (n && copy_put(c->pg, p, n, err, sizeof(err)) != 0) goto fail;
  } else {
    napi_get_value_string_utf8(env, argv[1], NULL, 0, &str_len);
    str = malloc(str_len + 1);
    napi_get_value_string_utf8(env, argv[1], str, str_len + 1, &str_len);
    if (str_len && copy_put(c->pg, str, str_len, err, sizeof(err)) != 0)
      goto fail;
  }

  for (;;) {
    int rc = PQputCopyEnd(c->pg, NULL);
    if (rc < 0) {
      snprintf(err, sizeof(err), "%s", PQerrorMessage(c->pg));
      goto fail;
    }
    if (rc > 0) break;
    if (PQflush(c->pg) != 0) {
      snprintf(err, sizeof(err), "%s", PQerrorMessage(c->pg));
      goto fail;
    }
  }

  res = PQgetResult(c->pg);
  if (res == NULL || PQresultStatus(res) != PGRES_COMMAND_OK) {
    snprintf(err, sizeof(err), "libpq: %s",
             res ? PQresultErrorMessage(res) : PQerrorMessage(c->pg));
    if (res) PQclear(res);
    goto fail;
  }
  PQclear(res);
  while ((res = PQgetResult(c->pg)) != NULL) PQclear(res);

  free(sql);
  free(str);
  return mk_undefined(env);

fail:
  while ((res = PQgetResult(c->pg)) != NULL) PQclear(res); /* resync */
  free(sql);
  free(str);
  napi_throw_error(env, NULL, err);
  return mk_undefined(env);
}

/* ---- writeSegment(segName, columns) -------------------------------------- */

/* columns: [{ name, kind, pgtype, value }]; value is a TypedArray, or for text
 * { bytes: Uint8Array, offsets: Int32Array }. No nulls. */
typedef struct {
  char *name;
  uint32_t name_len;
  uint32_t kind;
  uint32_t pgtype;
  const void *data;
  uint64_t data_len;
} WCol;

static int ta_elem_size(napi_typedarray_type t) {
  switch (t) {
    case napi_int8_array:
    case napi_uint8_array:
    case napi_uint8_clamped_array:
      return 1;
    case napi_int16_array:
    case napi_uint16_array:
      return 2;
    case napi_int32_array:
    case napi_uint32_array:
    case napi_float32_array:
      return 4;
    case napi_float64_array:
    case napi_bigint64_array:
    case napi_biguint64_array:
      return 8;
    default:
      return 0;
  }
}

static void *map_rw(const char *path, size_t capacity, size_t map_size) {
  int fd = shm_open(path, O_RDWR | O_CREAT, 0666);
  struct stat st;
  void *map;
  if (capacity == 0) capacity = map_size;
  if (fd < 0) return NULL;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return NULL;
  }
  if ((size_t) st.st_size < capacity && ftruncate(fd, (off_t) capacity) != 0) {
    close(fd);
    return NULL;
  }
  /* map only what is used; the file keeps the full capacity */
  map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  return map == MAP_FAILED ? NULL : map;
}

static int col_width(uint32_t kind) {
  switch (kind) {
    case NP_KIND_BOOL: return 1;
    case NP_KIND_INT16: return 2;
    case NP_KIND_INT32: return 4;
    case NP_KIND_INT64: return 8;
    case NP_KIND_FLOAT4: return 4;
    case NP_KIND_FLOAT8: return 8;
    default: return 0;
  }
}

static napi_value write_segment(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2], cols, out;
  napi_valuetype ot;
  char name[256];
  size_t nlen = sizeof(name);
  char path[300];
  uint32_t ncols = 0, c;
  uint64_t nrows = 0, null_bytes, off, total;
  bool have_rows = false;
  size_t seg_size, map_size;
  void *map;
  char *p, *base;
  WCol *wc = NULL;

  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 2) {
    napi_throw_type_error(env, NULL, "writeSegment(segName, columns)");
    return mk_undefined(env);
  }
  napi_get_value_string_utf8(env, argv[0], name, sizeof(name), &nlen);
  snprintf(path, sizeof(path), "/%s", name);
  napi_typeof(env, argv[1], &ot);
  if (ot != napi_object) {
    napi_throw_type_error(env, NULL, "columns must be an array");
    return mk_undefined(env);
  }
  cols = argv[1];
  napi_get_array_length(env, cols, &ncols);
  if (ncols == 0) {
    napi_throw_error(env, NULL, "writeSegment: no columns");
    return mk_undefined(env);
  }

  wc = calloc(ncols, sizeof(*wc));
  for (c = 0; c < ncols; c++) {
    napi_value col, v, nm;
    char cname[128];
    size_t cname_len = sizeof(cname);
    int32_t kind = 0, pgtype = 0;

    napi_get_element(env, cols, c, &col);
    napi_get_named_property(env, col, "name", &nm);
    napi_get_value_string_utf8(env, nm, cname, sizeof(cname), &cname_len);
    wc[c].name = malloc(cname_len + 1);
    memcpy(wc[c].name, cname, cname_len);
    wc[c].name[cname_len] = 0;
    wc[c].name_len = (uint32_t) cname_len;

    napi_get_named_property(env, col, "kind", &v);
    napi_get_value_int32(env, v, &kind);
    wc[c].kind = (uint32_t) kind;
    napi_get_named_property(env, col, "pgtype", &v);
    napi_get_value_int32(env, v, &pgtype);
    wc[c].pgtype = (uint32_t) pgtype;

    napi_get_named_property(env, col, "value", &v);
    if (wc[c].kind == NP_KIND_TEXT) {
      napi_value nb, no, ab;
      napi_typedarray_type t;
      void *bp = NULL, *op = NULL;
      size_t bl = 0, ol = 0, bo = 0;
      napi_get_named_property(env, v, "bytes", &nb);
      napi_get_named_property(env, v, "offsets", &no);
      if (napi_get_typedarray_info(env, nb, &t, &bl, &bp, &ab, &bo) != napi_ok ||
          napi_get_typedarray_info(env, no, &t, &ol, &op, &ab, &bo) != napi_ok) {
        napi_throw_type_error(env, NULL, "text value must be {bytes, offsets}");
        goto fail;
      }
      wc[c].data = op; /* the int32 offset table; bytes follow it */
      wc[c].data_len = (uint64_t) ol * 4 + bl;
      if (ol == 0) {
        napi_throw_error(env, NULL, "text offsets must be non-empty");
        goto fail;
      }
      if (!have_rows) { nrows = ol - 1; have_rows = true; }
      else if (ol - 1 != nrows) {
        napi_throw_error(env, NULL, "columns have different row counts");
        goto fail;
      }
    } else {
      napi_value ab;
      napi_typedarray_type t;
      void *dp = NULL;
      size_t len = 0, bo = 0;
      if (napi_get_typedarray_info(env, v, &t, &len, &dp, &ab, &bo) != napi_ok) {
        napi_throw_type_error(env, NULL, "value must be a TypedArray");
        goto fail;
      }
      if (ta_elem_size(t) != col_width(wc[c].kind)) {
        napi_throw_type_error(env, NULL, "value type does not match kind");
        goto fail;
      }
      wc[c].data = dp;
      wc[c].data_len = (uint64_t) len * (uint64_t) col_width(wc[c].kind);
      if (!have_rows) { nrows = len; have_rows = true; }
      else if (len != nrows) {
        napi_throw_error(env, NULL, "columns have different row counts");
        goto fail;
      }
    }
  }

  null_bytes = (nrows + 7) / 8;
  off = 8 + 4 + 4 + 4 + 8;
  for (c = 0; c < ncols; c++) {
    off += 4 + wc[c].name_len + 1 + 4 + null_bytes + 8;
    off = NP_ALIGN8(off);
    off += wc[c].data_len;
  }
  total = off;
  if (total + NP_SHM_HEADER_SIZE > (uint64_t) NP_SEGMENT_BYTES) {
    napi_throw_error(env, NULL, "writeSegment: result exceeds the 1 GiB segment");
    goto fail;
  }
  seg_size = NP_SEGMENT_BYTES;

  map_size = (size_t) (NP_SHM_HEADER_SIZE + total);
  map = map_rw(path, seg_size, map_size);
  if (map == NULL) {
    napi_throw_error(env, NULL, "writeSegment: cannot map /dev/shm segment");
    goto fail;
  }

  base = (char *) map + NP_SHM_HEADER_SIZE;
  p = base;
  memcpy(p, NP_COLUMNAR_MAGIC, 8); p += 8;
  np_put_u32(p, NP_COLUMNAR_VERSION); p += 4;
  np_put_u32(p, 1); p += 4; /* flags: little-endian */
  np_put_u32(p, ncols); p += 4;
  np_put_u64(p, nrows); p += 8;
  for (c = 0; c < ncols; c++) {
    np_put_u32(p, wc[c].name_len); p += 4;
    memcpy(p, wc[c].name, wc[c].name_len); p += wc[c].name_len;
    *p++ = (char) wc[c].kind;
    np_put_u32(p, wc[c].pgtype); p += 4;
    memset(p, 0, null_bytes); p += null_bytes; /* no nulls */
    np_put_u64(p, wc[c].data_len); p += 8;
    while (((size_t) (p - base)) & 7) *p++ = 0; /* 8-byte aligned column data */
    memcpy(p, wc[c].data, (size_t) wc[c].data_len);
    p += wc[c].data_len;
  }
  np_shm_publish(map, seg_size, total);
  munmap(map, map_size);

  for (c = 0; c < ncols; c++) free(wc[c].name);
  free(wc);

  napi_create_double(env, (double) total, &out);
  return out;

fail:
  if (wc) {
    for (c = 0; c < ncols; c++) free(wc[c].name);
    free(wc);
  }
  return mk_undefined(env);
}

/* ---- module -------------------------------------------------------------- */

static napi_value init(napi_env env, napi_value exports) {
  napi_value conn_class, read_fn, write_fn;
  napi_property_descriptor conn_props[] = {
      {"exec", NULL, conn_exec, NULL, NULL, NULL, napi_default, NULL},
      {"copyFrom", NULL, conn_copy_from, NULL, NULL, NULL, napi_default, NULL},
      {"close", NULL, conn_close, NULL, NULL, NULL, napi_default, NULL},
  };

  napi_define_class(env, "Connection", NAPI_AUTO_LENGTH, conn_new, NULL,
                    sizeof(conn_props) / sizeof(conn_props[0]), conn_props,
                    &conn_class);
  napi_create_function(env, "read", NAPI_AUTO_LENGTH, read_impl, NULL, &read_fn);
  napi_create_function(env, "writeSegment", NAPI_AUTO_LENGTH, write_segment,
                       NULL, &write_fn);
  napi_set_named_property(env, exports, "Connection", conn_class);
  napi_set_named_property(env, exports, "read", read_fn);
  napi_set_named_property(env, exports, "writeSegment", write_fn);
  return np_register_synth(env, exports);
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, init)
