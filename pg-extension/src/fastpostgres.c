/*
 * fastpostgres — the module entry point.
 *
 * The SQL surface is deliberately tiny: fastpostgres_ring_start / fastpostgres_ring_stop
 * (registered by fastpostgres--1.0.sql, implemented in worker.c) plus the
 * background worker they spawn. Everything else runs inside that worker, off
 * the request/response ring in /dev/shm, so nothing here needs a planner hook.
 */
#include "postgres.h"

#include "fmgr.h"

#include "kernels.h"

PG_MODULE_MAGIC;

void _PG_init(void);

/*
 * Pick the marshalling kernels once per backend, rather than testing the CPU
 * on every encode. The worker's whole job is that encode, so this is the one
 * place the choice has to be made.
 */
void _PG_init(void)
{
    fp_kernels_init();
}
