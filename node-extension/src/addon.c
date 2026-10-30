/*
 * addon.c — module entry point.
 *
 * Raw N-API (node_api.h, no node-addon-api). Each concern lives in its own
 * module and registers itself here:
 *
 *   connection.c     new Connection(conninfo)  libpq, the control channel
 *   ring_client.c    new Ring(seg)             drive the worker through the ring
 *   segment_read.c   read(segName, opts)       columnar segment -> JS typed arrays
 *   segment_write.c  writeSegment(seg, cols)   JS typed arrays -> columnar segment
 *   synth.c          synthSegment / synthCopy  benchmark generators
 */
#include <node_api.h>

#include "connection.h"
#include "ring_client.h"
#include "segment_read.h"
#include "segment_write.h"
#include "synth.h"

static napi_value init(napi_env env, napi_value exports)
{
    fastpostgres_register_connection(env, exports);
    fastpostgres_register_ring(env, exports);
    fastpostgres_register_read(env, exports);
    fastpostgres_register_write(env, exports);
    fastpostgres_register_synth(env, exports);
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, init)
