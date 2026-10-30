#ifndef FASTPOSTGRES_NAPI_UTIL_H
#define FASTPOSTGRES_NAPI_UTIL_H

#include <node_api.h>

/* Small N-API value helpers shared by the addon modules. */
napi_value fastpostgres_mk_undefined(napi_env env);
napi_value fastpostgres_mk_i32(napi_env env, int32_t x);
bool       fastpostgres_is_function(napi_env env, napi_value v);

#endif
