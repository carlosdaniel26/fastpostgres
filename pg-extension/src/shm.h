/* shm.h — POSIX shared-memory transport helpers (backend side). */
#ifndef NP_SHM_UTIL_H
#define NP_SHM_UTIL_H

#include <stddef.h>

/* "/<seg>" in the caller's memory context. */
char *np_shm_path(const char *seg);

/* Create/truncate the segment to `size` bytes and map it read-write. */
void *np_shm_map_rw(const char *path, size_t size);

/* Map an existing segment read-only, whole and header-validated; the mapped
 * size goes to *size_out. */
void *np_shm_map_ro(const char *path, const char *fn, size_t *size_out);

#endif
