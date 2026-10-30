/* addon.cpp — module entry point.
 *
 * The client is split by concern, one module per file (as in pg-extension):
 *
 *   connection.cpp   Connection(conninfo)   libpq control channel
 *   ring.cpp         Ring(seg)              drive the worker through /dev/shm
 *   segment.cpp      read / read_rows       columnar segment -> Python
 *   codec.cpp        type OID -> Python value
 *   synth.cpp        benchmark generators
 *   types.cpp        the shared Result / Column / Segment types
 *
 * The segment format lives in the extension's header
 * (pg-extension/fastpostgres_shm.h), which both sides include.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "codec.hpp"
#include "connection.hpp"
#include "ring.hpp"
#include "segment.hpp"
#include "synth.hpp"
#include "types.hpp"

static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "fastpostgres._client",
    "fastpostgres client (shared memory + libpq)",
    -1,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

PyMODINIT_FUNC
PyInit__client(void)
{
    if (!fp_codec_init()) return nullptr;
    if (!fp_types_init()) return nullptr;

    PyObject *m = PyModule_Create(&moduledef);
    if (!m) return nullptr;

    if (!fp_register_types(m) || !fp_register_connection(m) || !fp_register_ring(m) ||
        !fp_register_segment(m))
    {
        Py_DECREF(m);
        return nullptr;
    }
    fp_register_synth(m);
    return m;
}
