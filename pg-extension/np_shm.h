/* np_shm.h — shared-memory result-segment format, agreed between the
 * PostgreSQL extension (writer) and the V8 embedder (reader).
 *
 * Segment layout:
 *   [ NpShmHeader (64 bytes) ][ NPCOLUMN payload (header + columns) ]
 *
 * The writer fills the payload, then publishes payload_size and bumps seq with
 * a memory barrier; the reader checks seq before/after copying the payload to
 * detect a concurrent write (seqlock-style).
 */
#ifndef NP_SHM_H
#define NP_SHM_H

#include <stdint.h>

#define NP_SHM_MAGIC "NPQSHMEM"
#define NP_SHM_VERSION 1
#define NP_SHM_HEADER_SIZE 64

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

#endif
