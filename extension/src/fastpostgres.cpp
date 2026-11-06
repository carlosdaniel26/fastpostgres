/*
 * fastpostgres — the module entry point.
 *
 * The SQL surface is deliberately tiny: fastpostgres_ring_start / fastpostgres_ring_stop
 * (registered by fastpostgres--1.0.sql, implemented in worker.cpp) plus the
 * background worker they spawn. Everything else runs inside that worker, off
 * the request/response ring in /dev/shm, so nothing here needs a planner hook.
 *
 * PG_MODULE_MAGIC defines Pg_magic_func, which the backend resolves by that
 * exact name: it has to keep C linkage, so it sits in the extern "C" block
 * along with the PostgreSQL headers it expands.
 */
#include "pgxx.h"

extern "C" {
#include "fmgr.h"

PG_MODULE_MAGIC;
}
