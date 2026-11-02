/* ring.hpp — the control channel to the backend worker. */
#pragma once

#include <Python.h>

#include "fastpostgres/ring.h"
#include "transport.h"

/* Per-process state for whichever engine the ring resolved: the two ctx shapes
 * in transport.h, one of them live, nothing shared with the peer. */
typedef union
{
    FastpostgresShmCtx    shm;
    FastpostgresRegipcCtx regipc;
} RingCtx;

typedef struct {
    PyObject_HEAD FastpostgresRing *ring;
    void                           *map;
    size_t                          size;
    const FastpostgresTransport    *ops;       /* resolved at construction; names the segment's engine */
    RingCtx                         ctx;       /* this process's own state for `ops` */
    uint64_t                        seq;       /* the sequence of the request last posted */
    int                             done;      /* client_fini has run (or was never needed) */
    char                            data[128]; /* the columnar segment of the current stream */
} RingObject;

extern PyTypeObject RingType;

bool fp_register_ring(PyObject *module);
