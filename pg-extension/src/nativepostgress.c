/*
 * nativepostgress — the module entry point.
 *
 * The SQL surface is deliberately tiny: native_ring_start / native_ring_stop
 * (registered by nativepostgress--1.0.sql, implemented in worker.c) plus the
 * background worker they spawn. Everything else runs inside that worker, off
 * the request/response ring in /dev/shm, so nothing here needs a planner hook.
 */
#include "postgres.h"

#include "fmgr.h"

PG_MODULE_MAGIC;
