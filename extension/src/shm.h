/* shm.h — POSIX shared-memory transport helpers (backend side). */
#ifndef FASTPOSTGRES_SHM_UTIL_H
#define FASTPOSTGRES_SHM_UTIL_H

#include <stddef.h>

/* "/<seg>" in the caller's memory context. */
char *fastpostgres_shm_path(const char *seg);

/* Create/truncate the segment to `size` bytes and map it read-write. */
void *fastpostgres_shm_map_rw(const char *path, size_t size);

/* The same mapping, kept for the life of a stream and keyed by (path, size).
 * Batch publishing goes through this: the segment is the same object from one
 * batch to the next, and only the payload in it changes. */
void *fastpostgres_shm_map_cached(const char *path, size_t size);

/* Unmap everything the cache holds. Call at a stream boundary and on worker
 * exit; a mapping must not outlive the segment it describes. */
void fastpostgres_shm_cache_fini(void);

#endif
