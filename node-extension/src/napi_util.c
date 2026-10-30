/* napi_util.c — tiny N-API value helpers shared across the addon modules. */
#include "napi_util.h"

napi_value fastpostgres_mk_undefined(napi_env env)
{
    napi_value v;
    napi_get_undefined(env, &v);
    return v;
}

napi_value fastpostgres_mk_i32(napi_env env, int32_t x)
{
    napi_value v;
    napi_create_int32(env, x, &v);
    return v;
}

bool fastpostgres_is_function(napi_env env, napi_value v)
{
    napi_valuetype t;
    if (napi_typeof(env, v, &t) != napi_ok) return false;
    return t == napi_function;
}
