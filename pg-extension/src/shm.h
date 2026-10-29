/* shm.h — POSIX shared-memory transport helpers. */
#ifndef NP_SHM_UTIL_H
#define NP_SHM_UTIL_H

#include <stddef.h>

/* "/<seg>" in the caller's memory context. */
char *np_shm_path(const char *seg);

/* Create/truncate the segment to `capacity` (0 = map_size) and map only
 * `map_size` bytes read-write. */
void *np_shm_map_rw(const char *path, size_t capacity, size_t map_size);

/* Open an existing segment read-only, map it whole and validate its header;
 * the mapped size goes to *size_out. */
void *np_shm_map_ro(const char *path, const char *fn, size_t *size_out);

#endif
