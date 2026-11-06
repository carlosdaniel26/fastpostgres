/* codec.hpp — PostgreSQL type OID -> Python value, for the client's result
 * paths (libpq text rows and the columnar /dev/shm chunks). */
#pragma once

#include <Python.h>
#include <libpq-fe.h>

#include "fastpostgres/fastpostgres_shm.h"

/* Import decimal/datetime once (call from module init). */
bool fp_codec_init(void);

/* Decode one libpq text value (PQgetvalue is NUL-terminated). */
PyObject *fp_decode_value(PGresult *res, int row, int col);

/* Decode one value out of a columnar /dev/shm chunk. */
PyObject *fp_decode_column(const FastpostgresColumn *col, uint64_t row);

/* Fill `rows` (a list of nrows tuples already allocated by the caller) from a
 * decoded columnar frame: one specialized loop per column, so the kind switch
 * runs once per column rather than once per value. Returns false with a Python
 * exception set. */
bool fp_decode_columns(const FastpostgresColumn *cols, uint32_t ncols, uint64_t nrows, PyObject *rows);

/* The same fill for a window of rows: `rows[i]` receives column row
 * `start + i`. This is the block-wise decode behind lazy iteration, so the
 * kind switch and the null-map test still run once per column per block while
 * a consumer can stop after any block. */
bool fp_decode_columns_range(const FastpostgresColumn *cols, uint32_t ncols, uint64_t start,
                             uint64_t nrows, PyObject *rows);

/* Fill `rows` (a list of nrows slots, one per tuple in the row frame) by walking
 * each tuple in place: the server hands over PostgreSQL's own row layout, so
 * every value is decoded where it lies. Returns false with an exception set. */
bool fp_decode_tuples(const FastpostgresColumn *cols, uint32_t ncols, FastpostgresRowsReader *rd,
                      PyObject *rows);

/* Account a whole read_rows() call (TSC cycles and rows) next to the decode's own
 * count, so the report can say what the frame and the list cost on top of it. */
void fp_stats_add(unsigned long long cycles, unsigned long long rows);

/* Print the decode's cycles/row to stderr at process exit. */
void fp_stats_report(void);
