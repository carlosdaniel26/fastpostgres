/* shm.h — POSIX shared-memory transport helpers (backend side). */
#ifndef FASTPOSTGRES_SHM_UTIL_H
#define FASTPOSTGRES_SHM_UTIL_H

#include <stddef.h>

/* "/<seg>" in the caller's memory context. */
char *fastpostgres_shm_path(const char *seg);

/* Create/truncate the segment to `size` bytes and map it read-write. */
void *fastpostgres_shm_map_rw(const char *path, size_t size);

/* Map an existing segment read-only, whole and header-validated; the mapped
 * size goes to *size_out. */
#endif
