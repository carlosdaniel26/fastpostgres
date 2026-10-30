/* codec.hpp — PostgreSQL type OID -> Python value, for the client's result
 * paths (libpq text rows and the columnar /dev/shm chunks). */
#pragma once

#include <Python.h>
#include <libpq-fe.h>

#include "fastpostgres_shm.h"

/* Import decimal/datetime once (call from module init). */
bool fp_codec_init(void);

/* Decode one libpq text value (PQgetvalue is NUL-terminated). */
PyObject *fp_decode_value(PGresult *res, int row, int col);

/* Decode one value out of a columnar /dev/shm chunk. */
PyObject *fp_decode_column(const FastpostgresColumn *col, uint64_t row);
