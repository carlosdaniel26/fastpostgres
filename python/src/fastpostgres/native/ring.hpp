/* ring.hpp — the /dev/shm control channel to the backend worker. */
#pragma once

#include <Python.h>

#include "ring.h"

typedef struct {
    PyObject_HEAD FastpostgresRing *ring;
    void                           *map;
    size_t                          size;
    char                            data[128]; /* the columnar segment of the current stream */
} RingObject;

extern PyTypeObject RingType;

bool fp_register_ring(PyObject *module);
