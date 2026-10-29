#ifndef NP_WRITE_H
#define NP_WRITE_H

#include "postgres.h"

/* Ingest a columnar segment into `table`; returns the rows inserted. */
uint64 np_write_segment(const char *seg, const char *table);

#endif
