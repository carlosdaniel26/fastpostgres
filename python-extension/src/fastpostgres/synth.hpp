/* synth.hpp — native generators for the benchmark. */
#ifndef FASTPOSTGRES_SYNTH_HPP
#define FASTPOSTGRES_SYNTH_HPP

#include <Python.h>

/* Register synth_segment(seg, n) / synth_copy(n) on `module`. */
void fp_register_synth(PyObject *module);

#endif
