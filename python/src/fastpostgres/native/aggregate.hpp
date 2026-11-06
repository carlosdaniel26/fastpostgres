/* aggregate.hpp — reduce a columnar segment in C++ (no Python object per row). */
#pragma once

#include <Python.h>

/* reduce(seg, [(op, column), ...]) -> list of scalars, the AGG_* constants, and
 * read_reduce() / read_reduce_bytes() for the worker's FPREDUCE frame. */
bool fp_register_aggregate(PyObject *module);

/* aggregate_frames(rings, segs, sqls, plan, batch): drive one partition per ring
 * concurrently on native threads and return the raw FPREDUCE frames, or None.
 * Implemented in parallel.cpp; registered beside the other aggregate methods. */
bool fp_register_parallel(PyObject *module);
