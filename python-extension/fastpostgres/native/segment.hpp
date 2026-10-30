/* segment.hpp — reading a columnar /dev/shm segment into Python. */
#pragma once

#include <Python.h>

/* read(seg)      -> Segment of zero-copy column views
 * read_rows(seg) -> Result of typed row tuples */
bool fp_register_segment(PyObject *module);
