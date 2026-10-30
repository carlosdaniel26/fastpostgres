#ifndef FASTPOSTGRES_SEGMENT_WRITE_H
#define FASTPOSTGRES_SEGMENT_WRITE_H

#include <node_api.h>

/* Registers writeSegment(segName, columns) on `exports`. */
void fastpostgres_register_write(napi_env env, napi_value exports);

#endif
