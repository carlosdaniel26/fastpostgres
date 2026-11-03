/* ring.hpp — the control channel to the backend worker. */
#pragma once

#include <Python.h>

#include "fastpostgres/ring.h"
#include "transport.h"

typedef struct {
    PyObject_HEAD FastpostgresRing *ring;
    void                           *map;
    size_t                          size;
    const FastpostgresTransport    *ops;       /* the engine's hooks (always the shm entry) */
    FastpostgresShmCtx              ctx;       /* this process's own state; nothing shared with the peer */
    uint64_t                        seq;       /* the sequence of the request last posted */
    int                             done;      /* client_fini has run (or was never needed) */
    char                            data[128]; /* the columnar segment of the current stream */
} RingObject;

extern PyTypeObject RingType;

bool fp_register_ring(PyObject *module);
