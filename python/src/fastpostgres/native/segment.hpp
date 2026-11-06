/* segment.hpp — reading a columnar /dev/shm segment into Python. */
#pragma once

#include <Python.h>

#include <cstdint>

/* read(seg)      -> Segment of zero-copy column views
 * read_rows(seg) -> Result of typed row tuples */
bool fp_register_segment(PyObject *module);

/* The mapped payload of a data segment, or nullptr with a Python exception set.
 * The mapping is cached for the process lifetime (segment.cpp), so the pointer
 * stays valid; the worker may rewrite the bytes on the next request, which is
 * why the caller must be the stream's owner. */
const uint8_t *fp_segment_payload(const char *name, uint64_t *payload_len);

/* The same mapping and validation, but silent: it never touches PyErr_*, so it
 * is callable with the GIL released (the parallel aggregate's threads re-map
 * after every batch). nullptr means the segment is not mapped or not ours; the
 * caller decides what that means. Reuses the one mapping cache. */
const uint8_t *fp_segment_payload_raw(const char *name, uint64_t *payload_len);

/* Register the AF_UNIX connection on which this ring's worker hands over its
 * data segments' memfds (SCM_RIGHTS). read()/read_rows() consult it by segment
 * name; without one they use /dev/shm. Ring start/teardown own the socket. */
void fp_handoff_register(int sock);
void fp_handoff_unregister(int sock);
/* Map a bulk-loaded columnar memfd read-only, via the backend that owns it.
 * The mapping is cached for the process lifetime and is immutable: the
 * descriptor was sealed at load time, so it can never move under a reader.
 * Returns the FPCOLUMN payload, or nullptr with a Python exception set. */
const uint8_t *fp_columnar_map(int pid, int fd, uint64_t size, uint64_t *payload_len);

/* The SCM_RIGHTS half: a handle names a descriptor the client received on its
 * control socket and mapped once. The backend's pid is not consulted, so the
 * client and server need not share a UID. */
const uint8_t *fp_columnar_map_handle(int handle, uint64_t size, uint64_t *payload_len);
