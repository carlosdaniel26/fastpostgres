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

/* Map a bulk-loaded columnar memfd read-only, via the backend that owns it.
 * The mapping is cached for the process lifetime and is immutable: the
 * descriptor was sealed at load time, so it can never move under a reader.
 * Returns the FPCOLUMN payload, or nullptr with a Python exception set. */
const uint8_t *fp_columnar_map(int pid, int fd, uint64_t size, uint64_t *payload_len);
