/* addon.cpp — module entry point.
 *
 * The client is split by concern, one module per file (as in pg-extension):
 *
 *   connection.cpp   Connection(conninfo)   libpq control channel
 *   ring.cpp         Ring(seg)              drive the worker through /dev/shm
 *   segment.cpp      read / read_rows       columnar segment -> Python
 *   codec.cpp        type OID -> Python value
 *   types.cpp        the shared Result / Column / Segment types
 *
 * The segment format lives in the extension's header
 * (pg-extension/fastpostgres_shm.h), which both sides include.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "codec.hpp"
#include "connection.hpp"
#include "fastpostgres_shm.h"
#include "ring.hpp"
#include "segment.hpp"
#include "types.hpp"

/* The wire kinds, so kind values have one source (fastpostgres_shm.h) and the
 * Python enum can be built from them instead of repeating the numbers. */
static bool
add_kind_constants(PyObject *module)
{
    struct {
        const char *name;
        int         value;
    } kinds[] = {
        {"KIND_BOOL", FASTPOSTGRES_KIND_BOOL},
        {"KIND_INT16", FASTPOSTGRES_KIND_INT16},
        {"KIND_INT32", FASTPOSTGRES_KIND_INT32},
        {"KIND_INT64", FASTPOSTGRES_KIND_INT64},
        {"KIND_FLOAT4", FASTPOSTGRES_KIND_FLOAT4},
        {"KIND_FLOAT8", FASTPOSTGRES_KIND_FLOAT8},
        {"KIND_TEXT", FASTPOSTGRES_KIND_TEXT},
        {"KIND_DATE", FASTPOSTGRES_KIND_DATE},
        {"KIND_TIME", FASTPOSTGRES_KIND_TIME},
        {"KIND_TIMESTAMP", FASTPOSTGRES_KIND_TIMESTAMP},
        {"KIND_TIMESTAMPTZ", FASTPOSTGRES_KIND_TIMESTAMPTZ},
        {"KIND_UUID", FASTPOSTGRES_KIND_UUID},
        {"KIND_BYTEA", FASTPOSTGRES_KIND_BYTEA},
    };

    for (auto &k : kinds)
        if (PyModule_AddIntConstant(module, k.name, k.value) != 0) return false;
    return true;
}

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
    fp_objpool_init();            /* off unless the layouts verify (FASTPOSTGRES_OBJ_ARENA) */
    Py_AtExit(fp_objpool_report); /* one line per process: how values were read */

    PyObject *m = PyModule_Create(&moduledef);
    if (!m) return nullptr;

    if (!fp_register_types(m) || !fp_register_connection(m) || !fp_register_ring(m) ||
        !fp_register_segment(m) || !add_kind_constants(m))
    {
        Py_DECREF(m);
        return nullptr;
    }
    if (PyModule_AddStringConstant(m, "objpool", fp_objpool_mode()) != 0)
    {
        Py_DECREF(m);
        return nullptr;
    }
    return m;
}
