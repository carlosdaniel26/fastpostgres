/* np_shm.h — shared-memory result-segment format, agreed between the
 * PostgreSQL extension (writer) and the client (reader).
 *
 * Segment layout:
 *   [ NpShmHeader (64 bytes) ][ NPCOLUMN payload (header + columns) ]
 *
 * This is the single source of truth for the format: the extension and the
 * client include this one header (the client-only copy is a shim), and it
 * carries the reader/writer helpers so the NPCOLUMN codec lives in one place.
 *
 * The writer fills the payload, then publishes payload_size and bumps seq with
 * a memory barrier; the reader checks seq before/after copying the payload to
 * detect a concurrent write (seqlock-style).
 */
#ifndef NP_SHM_H
#define NP_SHM_H

#include <stdint.h>
#include <string.h>

#define NP_SHM_MAGIC "NPQSHMEM"
#define NP_SHM_VERSION 1
#define NP_SHM_HEADER_SIZE 64

/* Fixed capacity for the writer's segment (lazily faulted, so only what is
 * written costs memory). */
#define NP_SEGMENT_BYTES (1024u * 1024u * 1024u)

typedef struct NpShmHeader {
  char magic[8];        /* "NPQSHMEM" */
  uint32_t version;     /* 1 */
  uint32_t flags;       /* bit0: 1 = little-endian */
  uint64_t payload_size;/* valid NPCOLUMN bytes at offset NP_SHM_HEADER_SIZE */
  uint64_t capacity;    /* total mapped bytes */
  uint64_t seq;         /* bumped after each completed write */
  uint32_t reserved[6]; /* pad to 64 bytes */
} NpShmHeader;

#ifdef __cplusplus
static_assert(sizeof(NpShmHeader) == 64, "NpShmHeader must be 64 bytes");
#endif

/* NPCOLUMN payload:
 *   magic[8] "NPCOLUMN", version u32, flags u32, ncols u32, nrows u64
 *   per column: name_len u32, name, kind u8, pgtype u32, nulls bitmap, then
 *   data_len u64 and data: fixed-width values, or (text) an int32 offset table
 *   followed by the raw bytes.
 *
 * Each column's data is padded to 8-byte alignment (relative to the payload
 * base), so a client can view it in place as a typed array.
 */
#define NP_ALIGN8(x) (((uint64_t) (x) + 7) & ~(uint64_t) 7)
#define NP_COLUMNAR_MAGIC "NPCOLUMN"
#define NP_COLUMNAR_VERSION 1

enum NpKind {
  NP_KIND_BOOL = 1,
  NP_KIND_INT32 = 2,
  NP_KIND_INT64 = 3,
  NP_KIND_FLOAT8 = 4,
  NP_KIND_TEXT = 5,
  NP_KIND_INT16 = 6, /* verbatim 2-byte integer (int2) */
  NP_KIND_FLOAT4 = 7  /* verbatim 4-byte float (float4) */
};

/* little-endian scalar readers/writers */
static inline void np_put_u32(char *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void np_put_u64(char *p, uint64_t v) { memcpy(p, &v, 8); }
static inline uint32_t np_rd_u32(const uint8_t **p) {
  uint32_t v;
  memcpy(&v, *p, 4);
  *p += 4;
  return v;
}
static inline uint64_t np_rd_u64(const uint8_t **p) {
  uint64_t v;
  memcpy(&v, *p, 8);
  *p += 8;
  return v;
}

/* One decoded column. `data` points at the value bytes; for text it is the
 * int32 offset table and `bytes` the raw payload. `name` is not NUL-terminated
 * (use name_len). */
typedef struct NpColumn {
  const char *name;
  uint32_t name_len;
  uint8_t kind;
  uint32_t pgtype;
  const uint8_t *nulls;
  const uint8_t *data;
  const uint8_t *bytes;
  uint64_t data_len;
  uint64_t nrows;
} NpColumn;

typedef struct NpReader {
  const uint8_t *base; /* payload base; column data is aligned relative to it */
  const uint8_t *p;
  const uint8_t *end;
  uint32_t ncols;
  uint64_t nrows;
} NpReader;

/* Point a reader at an NPCOLUMN payload; returns 0 if the magic is wrong. */
static inline int np_reader_init(NpReader *r, const void *payload, size_t len) {
  const uint8_t *p = (const uint8_t *) payload;
  if (len < 8 || memcmp(p, NP_COLUMNAR_MAGIC, 8) != 0) return 0;
  r->base = p;
  r->p = p + 8;
  r->end = p + len;
  (void) np_rd_u32(&r->p); /* version */
  (void) np_rd_u32(&r->p); /* flags */
  r->ncols = np_rd_u32(&r->p);
  r->nrows = np_rd_u64(&r->p);
  return 1;
}

/* Decode the next column; returns 0 at the end of the payload. */
static inline int np_reader_next(NpReader *r, NpColumn *c) {
  const uint8_t *p = r->p;
  if (p == NULL || p >= r->end) return 0;
  c->name_len = np_rd_u32(&p);
  c->name = (const char *) p;
  p += c->name_len;
  c->kind = *p++;
  c->pgtype = np_rd_u32(&p);
  c->nulls = p;
  p += (r->nrows + 7) / 8;
  c->data_len = np_rd_u64(&p);
  p = r->base + NP_ALIGN8((uint64_t) (p - r->base)); /* data is 8-byte aligned */
  c->data = p;
  c->bytes = (c->kind == NP_KIND_TEXT) ? p + (r->nrows + 1) * 4 : NULL;
  p += c->data_len;
  if (p > r->end) return 0;
  c->nrows = r->nrows;
  r->p = p;
  return 1;
}

static inline int np_col_isnull(const NpColumn *c, uint64_t row) {
  return (c->nulls[row >> 3] & (1u << (row & 7))) != 0;
}

/* Fill in the segment header and publish it with a release barrier. */
static inline void np_shm_publish(void *map, uint64_t capacity,
                                  uint64_t payload_size) {
  NpShmHeader *h = (NpShmHeader *) map;
  memcpy(h->magic, NP_SHM_MAGIC, 8);
  h->version = NP_SHM_VERSION;
  h->flags = 1; /* little-endian */
  h->capacity = capacity;
  __sync_synchronize();
  h->payload_size = payload_size;
  __sync_synchronize();
  h->seq++;
}

/* Validate a mapped segment header; returns payload size via *payload_size. */
static inline int np_shm_validate(const void *map, size_t size,
                                  uint64_t *payload_size) {
  const NpShmHeader *h = (const NpShmHeader *) map;
  if (size < NP_SHM_HEADER_SIZE || memcmp(h->magic, NP_SHM_MAGIC, 8) != 0)
    return 0;
  *payload_size = h->payload_size;
  return 1;
}

#endif
