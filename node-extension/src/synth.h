#ifndef FASTPOSTGRES_SYNTH_H
#define FASTPOSTGRES_SYNTH_H

#include <node_api.h>

/* Registers synthSegment(seg, n) / synthCopy(n) on `exports`. */
napi_value fastpostgres_register_synth(napi_env env, napi_value exports);

#endif
