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
    uint64_t                        seq;       /* the sequence of the request last posted */
    int                             done;      /* client_fini has run (or was never needed) */
    int                             busy;      /* a request is in flight on the single slot */
    unsigned long                   busy_by;   /* the thread that posted it */
    int                             owned;     /* a thread holds the ring's open stream */
    unsigned long                   owner;     /* which thread that is */
    uint32_t                        pid;       /* the process that made the ring */
    int                             fd_listen; /* memfd-handoff listener, or -1 */
    int                             fd_sock;   /* accepted handoff connection, or -1 */
    int                             memfd;     /* the client asked for memfd segments */
    char                            data[128]; /* the columnar segment of the current stream */
} RingObject;

extern PyTypeObject RingType;

bool fp_register_ring(PyObject *module);
