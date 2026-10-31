/* codec.hpp — PostgreSQL type OID -> Python value, for the client's result
 * paths (libpq text rows and the columnar /dev/shm chunks). */
#pragma once

#include <Python.h>
#include <libpq-fe.h>

#include "fastpostgres_shm.h"

/* Import decimal/datetime once (call from module init). */
bool fp_codec_init(void);

/* The immortal object pool (H11) and the constructors that use it. Values are
 * born in place inside one arena and never freed (the refcount is the
 * interpreter's immortal sentinel), which is worth 89.9 -> 20.5 ns/row on the
 * benchmark's row shape. fp_objpool_init() sizes the arena from
 * FASTPOSTGRES_OBJ_ARENA (0 disables it) and proves the layouts before handing
 * anything out; when it cannot, every constructor here falls back to the normal
 * one, so the flag only ever costs or saves the pool. Call from module init. */
void      fp_objpool_init(void);
void      fp_objpool_report(void);
const char *fp_objpool_mode(void);

/* These always return a usable object: the pool when it can, the standard
 * constructor otherwise. fp_birth_text returns NULL for a value it cannot
 * write in place (empty or non-ASCII), for its caller to fall back. */
PyObject *fp_birth_float(double v);
PyObject *fp_birth_long(long long v);
PyObject *fp_birth_text(const char *p, Py_ssize_t n);

/* Decode one libpq text value (PQgetvalue is NUL-terminated). */
PyObject *fp_decode_value(PGresult *res, int row, int col);

/* Decode one value out of a columnar /dev/shm chunk. */
PyObject *fp_decode_column(const FastpostgresColumn *col, uint64_t row);

/* The same, without the null test: for callers that already know the column has
 * no nulls (decided once per column, not once per value). */
PyObject *fp_decode_column_nonull(const FastpostgresColumn *col, uint64_t row);
