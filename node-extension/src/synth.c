/*
 * synth.c — native generators for the benchmark, so the JS benchmark does no
 * per-row work: it just calls these and consumes the result.
 *
 *   synthSegment(seg, n)  generate the bench rows straight into /dev/shm
 *                         (no intermediate JS arrays, no extra copy)
 *   synthCopy(n)          -> [Buffer, ...] for copyFrom()
 */
#include <node_api.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "np_shm.h"

#include "np_map.h"
#include "synth.h"

static napi_value mk_undefined(napi_env env) {
  napi_value v;
  napi_get_undefined(env, &v);
  return v;
}

static size_t dec_len(uint64_t v) {
  size_t n = 1;
  while (v >= 10) {
    v /= 10;
    n++;
  }
  return n;
}

static size_t put_dec(char *p, uint64_t v) {
  char tmp[24];
  size_t n = 0, i;
  if (v == 0) {
    p[0] = '0';
    return 1;
  }
  while (v) {
    tmp[n++] = (char) ('0' + (v % 10));
    v /= 10;
  }
  for (i = 0; i < n; i++) p[i] = tmp[n - 1 - i];
  return n;
}

static bool get_n(napi_env env, napi_callback_info info, const char *name,
                  uint64_t *out) {
  size_t argc = 1;
  napi_value argv[1];
  double nd;
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 1 || napi_get_value_double(env, argv[0], &nd) != napi_ok ||
      nd < 0) {
    char msg[64];
    snprintf(msg, sizeof(msg), "%s(n)", name);
    napi_throw_type_error(env, NULL, msg);
    return false;
  }
  *out = (uint64_t) nd;
  return true;
}

/* The bench table: id, account_id, balance, active, name, email, score, age. */
static const char *COLS_NAME[8] = {"id",        "account_id", "balance",
                                   "active",    "name",       "email",
                                   "score",     "age"};
static const int COLS_KIND[8] = {NP_KIND_INT32, NP_KIND_INT64, NP_KIND_FLOAT8,
                                 NP_KIND_BOOL,  NP_KIND_TEXT,  NP_KIND_TEXT,
                                 NP_KIND_FLOAT8, NP_KIND_INT32};
static const int COLS_PGTYPE[8] = {23, 20, 701, 16, 25, 25, 701, 23};

/* Write "user<r><suffix>" as an int32 offset table followed by the bytes. */
static void put_text_col(char *p, uint64_t n, const char *prefix,
                         size_t pre, const char *suffix, size_t suf) {
  int32_t *off = (int32_t *) p;
  char *b = p + (n + 1) * 4, *w = b;
  uint64_t r;
  off[0] = 0;
  for (r = 0; r < n; r++) {
    memcpy(w, prefix, pre);
    w += pre;
    w += put_dec(w, r);
    if (suf) {
      memcpy(w, suffix, suf);
      w += suf;
    }
    off[r + 1] = (int32_t) (w - b);
  }
}

static napi_value synth_segment(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2], out;
  char name[256], path[300];
  size_t nlen = sizeof(name);
  uint64_t n, r, c, data_len[8], null_bytes, off, total;
  size_t seg_size, map_size;
  void *map;
  char *base, *p;
  size_t pre_user = 4, suf_mail = sizeof("@example.com") - 1;

  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 2) {
    napi_throw_type_error(env, NULL, "synthSegment(segName, n)");
    return mk_undefined(env);
  }
  napi_get_value_string_utf8(env, argv[0], name, sizeof(name), &nlen);
  snprintf(path, sizeof(path), "/%s", name);
  {
    double nd;
    if (napi_get_value_double(env, argv[1], &nd) != napi_ok || nd < 0) {
      napi_throw_type_error(env, NULL, "synthSegment(segName, n)");
      return mk_undefined(env);
    }
    n = (uint64_t) nd;
  }

  data_len[0] = n * 4;
  data_len[1] = n * 8;
  data_len[2] = n * 8;
  data_len[3] = n * 1;
  data_len[6] = n * 8;
  data_len[7] = n * 4;
  {
    uint64_t name_bytes = 0, mail_bytes = 0;
    for (r = 0; r < n; r++) {
      size_t d = dec_len(r);
      name_bytes += pre_user + d;
      mail_bytes += pre_user + d + suf_mail;
    }
    data_len[4] = (n + 1) * 4 + name_bytes;
    data_len[5] = (n + 1) * 4 + mail_bytes;
  }

  null_bytes = (n + 7) / 8;
  off = 8 + 4 + 4 + 4 + 8;
  for (c = 0; c < 8; c++) {
    off += 4 + strlen(COLS_NAME[c]) + 1 + 4 + null_bytes + 8;
    off = NP_ALIGN8(off);
    off += data_len[c];
  }
  total = off;
  if (total + NP_SHM_HEADER_SIZE > (uint64_t) NP_SEGMENT_BYTES) {
    napi_throw_error(env, NULL, "synthSegment: result exceeds the 1 GiB segment");
    return mk_undefined(env);
  }
  seg_size = NP_SEGMENT_BYTES;
  map_size = (size_t) (NP_SHM_HEADER_SIZE + total);

  map = np_map_rw(path, seg_size, map_size);
  if (map == NULL) {
    napi_throw_error(env, NULL, "synthSegment: cannot map /dev/shm segment");
    return mk_undefined(env);
  }

  base = (char *) map + NP_SHM_HEADER_SIZE;
  p = base;
  memcpy(p, NP_COLUMNAR_MAGIC, 8); p += 8;
  np_put_u32(p, NP_COLUMNAR_VERSION); p += 4;
  np_put_u32(p, 1); p += 4; /* flags: little-endian */
  np_put_u32(p, 8); p += 4;
  np_put_u64(p, n); p += 8;

  for (c = 0; c < 8; c++) {
    size_t nl = strlen(COLS_NAME[c]);
    np_put_u32(p, (uint32_t) nl); p += 4;
    memcpy(p, COLS_NAME[c], nl); p += nl;
    *p++ = (char) COLS_KIND[c];
    np_put_u32(p, (uint32_t) COLS_PGTYPE[c]); p += 4;
    memset(p, 0, null_bytes); p += null_bytes; /* no nulls */
    np_put_u64(p, data_len[c]); p += 8;
    while (((size_t) (p - base)) & 7) *p++ = 0; /* 8-byte aligned column data */

    switch (c) {
      case 0: { int32_t *v = (int32_t *) p; for (r = 0; r < n; r++) v[r] = (int32_t) r; break; }
      case 1: { int64_t *v = (int64_t *) p; for (r = 0; r < n; r++) v[r] = (int64_t) (r * 1000); break; }
      case 2: { double *v = (double *) p; for (r = 0; r < n; r++) v[r] = (double) r * 1.5; break; }
      case 3: { uint8_t *v = (uint8_t *) p; for (r = 0; r < n; r++) v[r] = (r & 1) ? 0 : 1; break; }
      case 4: put_text_col(p, n, "user", pre_user, "", 0); break;
      case 5: put_text_col(p, n, "user", pre_user, "@example.com", suf_mail); break;
      case 6: { double *v = (double *) p; for (r = 0; r < n; r++) v[r] = (double) r * 0.25; break; }
      default: { int32_t *v = (int32_t *) p; for (r = 0; r < n; r++) v[r] = (int32_t) (r % 100); break; }
    }
    p += data_len[c];
  }

  np_shm_publish(map, seg_size, total);
  munmap(map, map_size);

  napi_create_double(env, (double) total, &out);
  return out;
}

/* COPY FROM STDIN payload (tab-separated), built natively in 1 MiB Buffers. */
static napi_value synth_copy(napi_env env, napi_callback_info info) {
  uint64_t n, i;
  napi_value arr, b;
  uint32_t nbufs = 0;
  const size_t cap = 1 << 20;
  char *chunk;
  size_t off = 0;

  if (!get_n(env, info, "synthCopy", &n)) return mk_undefined(env);

  chunk = malloc(cap);
  napi_create_array(env, &arr);
  for (i = 0; i < n; i++) {
    if (cap - off < 256) {
      napi_create_buffer_copy(env, off, chunk, NULL, &b);
      napi_set_element(env, arr, nbufs++, b);
      off = 0;
    }
    off += (size_t) snprintf(
        chunk + off, cap - off,
        "%llu\t%lld\t%.1f\t%c\tuser%llu\tuser%llu@example.com\t%.2f\t%llu\n",
        (unsigned long long) i, (long long) (i * 1000), (double) i * 1.5,
        (i & 1) ? 'f' : 't', (unsigned long long) i, (unsigned long long) i,
        (double) i * 0.25, (unsigned long long) (i % 100));
  }
  if (off) {
    napi_create_buffer_copy(env, off, chunk, NULL, &b);
    napi_set_element(env, arr, nbufs++, b);
  }
  free(chunk);
  return arr;
}

napi_value np_register_synth(napi_env env, napi_value exports) {
  napi_value f;
  napi_create_function(env, "synthSegment", NAPI_AUTO_LENGTH, synth_segment,
                       NULL, &f);
  napi_set_named_property(env, exports, "synthSegment", f);
  napi_create_function(env, "synthCopy", NAPI_AUTO_LENGTH, synth_copy, NULL,
                       &f);
  napi_set_named_property(env, exports, "synthCopy", f);
  return exports;
}
