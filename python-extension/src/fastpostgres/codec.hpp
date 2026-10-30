/* codec.hpp — PostgreSQL type OID -> Python value, for the client's result
 * paths (libpq text rows and the columnar /dev/shm chunks). */
#ifndef FASTPOSTGRES_CODEC_HPP
#define FASTPOSTGRES_CODEC_HPP

#include <Python.h>
#include <libpq-fe.h>

/* Import decimal/datetime once (call from module init). */
bool fp_codec_init(void);

/* Decode one libpq text value (PQgetvalue is NUL-terminated). */
PyObject *fp_decode_value(PGresult *res, int row, int col);

#endif
