/* shm.h — data-segment transport helpers (backend side).
 *
 * A worker keeps one mapping per segment name for its lifetime. The default
 * backing is an anonymous sealed memfd handed to the client once over an
 * AF_UNIX socket (ring.h fd_handoff); the original /dev/shm/<seg> file is the
 * fallback when the client did not ask for the handoff, or the kernel cannot
 * make a memfd.
 */
#ifndef FASTPOSTGRES_SHM_UTIL_H
#define FASTPOSTGRES_SHM_UTIL_H

#include <stddef.h>

#include "fastpostgres/ring.h"

/* "/<seg>" in the caller's memory context. */
char *fastpostgres_shm_path(const char *seg);

/* Map the data segment named by `path` read-write, creating it on first use.
 * The mapping (and a memfd's descriptor) is cached for the worker's lifetime;
 * `*cap_out` is the capacity the mapping actually has, which is the size the
 * caller must use for its end pointer. */
void *fastpostgres_shm_map_rw(const char *path, size_t size, size_t *cap_out);

/* Open the fd-handoff channel (when the ring asks for one) and reset the
 * segment cache. Called once by the ring worker before its first request. */
void fastpostgres_shm_worker_begin(FastpostgresRing *ring);

/* Unmap every cached segment, close the memfd descriptors and the handoff
 * socket. Called once as the ring worker exits. */
void fastpostgres_shm_worker_end(void);

#endif
