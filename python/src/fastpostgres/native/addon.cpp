/* addon.cpp — module entry point.
 *
 * The client is split by concern, one module per file (as in the extension):
 *
 *   connection.cpp   Connection(conninfo)   libpq control channel
 *   ring.cpp         Ring(seg)              drive the worker through /dev/shm
 *   segment.cpp      read / read_rows       columnar segment -> Python
 *   aggregate.cpp    reduce                 columnar segment -> numbers, in C++
 *   codec.cpp        type OID -> Python value
 *   types.cpp        the shared Result / Column / Segment types
 *
 * The segment format lives in the extension's header
 * (extension/fastpostgres_shm.h), which both sides include.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "aggregate.hpp"
#include "codec.hpp"
#include "connection.hpp"
#include "fastpostgres/fastpostgres_shm.h"
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

/* Multi-phase init, so the free-threaded interpreter can be told this module is
 * safe without the GIL. What that claim rests on, in this module and its
 * neighbours: one ring belongs to one thread (ring.cpp refuses a second driver),
 * a connection belongs to one thread (libpq's rule), the segment map cache and
 * the decode counters are locked/atomic (segment.cpp, codec.cpp), and the codec's
 * Python singletons are written once at init and only borrowed afterwards. */
static int
client_exec(PyObject *m)
{
    if (!fp_register_types(m) || !fp_register_connection(m) || !fp_register_ring(m) ||
        !fp_register_segment(m) || !fp_register_aggregate(m) || !add_kind_constants(m))
        return -1;
    return 0;
}

static PyModuleDef_Slot client_slots[] = {
    {Py_mod_exec, (void *)client_exec},
#ifdef Py_mod_gil
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
    {0, nullptr},
};

static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "fastpostgres._client",
    "fastpostgres client (shared memory + libpq)",
    0, /* no module state: the types and caches are process globals, as before */
    nullptr,
    client_slots,
    nullptr,
    nullptr,
    nullptr,
};

PyMODINIT_FUNC
PyInit__client(void)
{
    if (!fp_codec_init()) return nullptr;
    if (!fp_types_init()) return nullptr;
    Py_AtExit(fp_stats_report); /* one line per process: how the row decode cost */

    return PyModuleDef_Init(&moduledef);
}
