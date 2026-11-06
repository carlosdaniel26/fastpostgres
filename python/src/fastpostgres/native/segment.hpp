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

/* Register the AF_UNIX connection on which this ring's worker hands over its
 * data segments' memfds (SCM_RIGHTS). read()/read_rows() consult it by segment
 * name; without one they use /dev/shm. Ring start/teardown own the socket. */
void fp_handoff_register(int sock);
void fp_handoff_unregister(int sock);
