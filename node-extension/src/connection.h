#ifndef FASTPOSTGRES_CONNECTION_H
#define FASTPOSTGRES_CONNECTION_H

#include <node_api.h>

#include <postgresql/libpq-fe.h>

/* The JS `Connection`: a libpq handle used as the control channel (and for
 * COPY FROM STDIN). */
typedef struct {
    PGconn *pg;
} FastpostgresConnection;

/* Registers the Connection class on `exports`. */
void fastpostgres_register_connection(napi_env env, napi_value exports);

#endif
