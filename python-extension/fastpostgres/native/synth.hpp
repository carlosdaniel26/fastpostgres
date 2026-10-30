/* synth.hpp — native generators for the benchmark. */
#pragma once

#include <Python.h>

/* Register synth_segment(seg, n) / synth_copy(n) on `module`. */
void fp_register_synth(PyObject *module);
