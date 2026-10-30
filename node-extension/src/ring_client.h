#ifndef FASTPOSTGRES_RING_CLIENT_H
#define FASTPOSTGRES_RING_CLIENT_H

#include <node_api.h>

/* Registers the Ring class on `exports`. Named "ring_client" to avoid clashing
 * with the shared wire-format header ring.h from pg-extension/src. */
void fastpostgres_register_ring(napi_env env, napi_value exports);

#endif
