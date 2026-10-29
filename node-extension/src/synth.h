#ifndef NP_SYNTH_H
#define NP_SYNTH_H

#include <node_api.h>

/* Registers synthColumns(n) / synthCopy(n) on `exports`. */
napi_value np_register_synth(napi_env env, napi_value exports);

#endif
