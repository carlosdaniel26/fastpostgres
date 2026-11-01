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
void        fp_objpool_init(void);
void        fp_objpool_report(void);
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

/* Fill `rows` (a list of nrows tuples already allocated by the caller) from a
 * decoded columnar frame: one specialized loop per column, so the kind switch
 * runs once per column rather than once per value, and the births inline.
 * Returns false with a Python exception set. */
bool fp_decode_columns(const FastpostgresColumn *cols, uint32_t ncols, uint64_t nrows, PyObject *rows);

/* Fill `rows` (a list of nrows slots, one per tuple in the row frame) by walking
 * each tuple in place: the server hands over PostgreSQL's own row layout, so
 * every value is decoded where it lies. Returns false with an exception set. */
bool fp_decode_tuples(const FastpostgresColumn *cols, uint32_t ncols, FastpostgresRowsReader *rd,
                      PyObject *rows);

/* Account a whole read_rows() call (TSC cycles and rows) next to the decode's own
 * count, so the report can say what the frame and the list cost on top of it. */
void fp_stats_add(unsigned long long cycles, unsigned long long rows);
