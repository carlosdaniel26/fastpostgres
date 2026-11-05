/* aggregate.hpp — reduce a columnar segment in C++ (no Python object per row). */
#pragma once

#include <Python.h>

/* reduce(seg, [(op, column), ...]) -> list of scalars, and the AGG_* constants. */
bool fp_register_aggregate(PyObject *module);
