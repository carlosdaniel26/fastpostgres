#ifndef FASTPOSTGRES_SEGMENT_READ_H
#define FASTPOSTGRES_SEGMENT_READ_H

#include <node_api.h>

/* Registers read(segName[, options]) on `exports`. */
void fastpostgres_register_read(napi_env env, napi_value exports);

#endif
