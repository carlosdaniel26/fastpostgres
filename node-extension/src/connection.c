/* connection.c — the JS Connection class: libpq exec + COPY FROM STDIN. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "connection.h"
#include "napi_util.h"

#define FASTPOSTGRES_ERR_MAX 1024

static void conn_finalize(napi_env env, void *data, void *hint)
{
    FastpostgresConnection *c = data;
    (void)env;
    (void)hint;
    if (!c) return;
    if (c->pg) PQfinish(c->pg);
    free(c);
}

static napi_value conn_new(napi_env env, napi_callback_info info)
{
    size_t                  argc = 1;
    napi_value              argv[1], self;
    char                    conninfo[1024] = "";
    FastpostgresConnection *c;
    PGconn                 *pg;

    napi_get_cb_info(env, info, &argc, argv, &self, NULL);
    if (argc >= 1)
    {
        size_t len = 0;
        if (napi_get_value_string_utf8(env, argv[0], conninfo, sizeof(conninfo), &len) != napi_ok)
        {
            napi_throw_type_error(env, NULL, "conninfo must be a string");
            return fastpostgres_mk_undefined(env);
        }
    }

    pg = PQconnectdb(conninfo);
    if (PQstatus(pg) != CONNECTION_OK)
    {
        char msg[FASTPOSTGRES_ERR_MAX];
        snprintf(msg, sizeof(msg), "libpq: %s", PQerrorMessage(pg));
        PQfinish(pg);
        napi_throw_error(env, NULL, msg);
        return fastpostgres_mk_undefined(env);
    }

    c     = calloc(1, sizeof(*c));
    c->pg = pg;
    napi_wrap(env, self, c, conn_finalize, NULL, NULL);
    return self;
}

static napi_value conn_exec(napi_env env, napi_callback_info info)
{
    size_t                  argc = 1;
    napi_value              argv[1], self, arr;
    FastpostgresConnection *c = NULL;
    char                   *sql;
    size_t                  len = 0;
    PGresult               *res;
    ExecStatusType          st;
    int                     nrows, ncols, r, f;

    napi_get_cb_info(env, info, &argc, argv, &self, NULL);
    napi_unwrap(env, self, (void **)&c);
    if (!c || !c->pg)
    {
        napi_throw_error(env, NULL, "connection is closed");
        return fastpostgres_mk_undefined(env);
    }

    napi_get_value_string_utf8(env, argv[0], NULL, 0, &len);
    sql = malloc(len + 1);
    napi_get_value_string_utf8(env, argv[0], sql, len + 1, &len);

    res = PQexec(c->pg, sql);
    free(sql);

    st = PQresultStatus(res);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK)
    {
        char msg[FASTPOSTGRES_ERR_MAX];
        snprintf(msg, sizeof(msg), "libpq: %s", PQresultErrorMessage(res));
        PQclear(res);
        napi_throw_error(env, NULL, msg);
        return fastpostgres_mk_undefined(env);
    }

    nrows = PQntuples(res);
    ncols = PQnfields(res);
    napi_create_array_with_length(env, (size_t)nrows, &arr);
    for (r = 0; r < nrows; r++)
    {
        napi_value row;
        napi_create_object(env, &row);
        for (f = 0; f < ncols; f++)
        {
            napi_value v;
            if (PQgetisnull(res, r, f))
                napi_get_null(env, &v);
            else
                napi_create_string_utf8(env, PQgetvalue(res, r, f), NAPI_AUTO_LENGTH, &v);
            napi_set_named_property(env, row, PQfname(res, f), v);
        }
        napi_set_element(env, arr, (uint32_t)r, row);
    }
    PQclear(res);
    return arr;
}

static napi_value conn_close(napi_env env, napi_callback_info info)
{
    size_t                  argc = 0;
    napi_value              self;
    FastpostgresConnection *c = NULL;

    napi_get_cb_info(env, info, &argc, NULL, &self, NULL);
    napi_unwrap(env, self, (void **)&c);
    if (c && c->pg)
    {
        PQfinish(c->pg);
        c->pg = NULL;
    }
    return fastpostgres_mk_undefined(env);
}

/* Stream `n` bytes with libpq's COPY protocol, retrying while it needs flushing. */
static int copy_put(PGconn *pg, const char *buf, size_t n, char *err, size_t errsz)
{
    size_t off = 0;
    while (off < n)
    {
        size_t chunk = n - off;
        int    rc;
        if (chunk > (size_t)(1 << 20)) chunk = 1 << 20;
        rc = PQputCopyData(pg, buf + off, (int)chunk);
        if (rc < 0)
        {
            snprintf(err, errsz, "%s", PQerrorMessage(pg));
            return -1;
        }
        if (rc == 0)
        {
            if (PQflush(pg) != 0)
            {
                snprintf(err, errsz, "%s", PQerrorMessage(pg));
                return -1;
            }
            continue; /* retry the same chunk */
        }
        off += chunk;
    }
    return 0;
}

static napi_value conn_copy_from(napi_env env, napi_callback_info info)
{
    size_t                  argc = 2;
    napi_value              argv[2], self;
    FastpostgresConnection *c   = NULL;
    char                   *sql = NULL, *str = NULL;
    size_t                  sql_len = 0, str_len = 0;
    bool                    is_buf = false, is_arr = false;
    PGresult               *res;
    char                    err[FASTPOSTGRES_ERR_MAX] = "";

    napi_get_cb_info(env, info, &argc, argv, &self, NULL);
    napi_unwrap(env, self, (void **)&c);
    if (!c || !c->pg)
    {
        napi_throw_error(env, NULL, "connection is closed");
        return fastpostgres_mk_undefined(env);
    }
    if (argc < 2)
    {
        napi_throw_type_error(env, NULL, "copyFrom(sql, data)");
        return fastpostgres_mk_undefined(env);
    }

    napi_get_value_string_utf8(env, argv[0], NULL, 0, &sql_len);
    sql = malloc(sql_len + 1);
    napi_get_value_string_utf8(env, argv[0], sql, sql_len + 1, &sql_len);

    res = PQexec(c->pg, sql);
    if (PQresultStatus(res) != PGRES_COPY_IN)
    {
        snprintf(err, sizeof(err), "libpq: %s", PQresultErrorMessage(res));
        PQclear(res);
        free(sql);
        napi_throw_error(env, NULL, err);
        return fastpostgres_mk_undefined(env);
    }
    PQclear(res);

    napi_is_buffer(env, argv[1], &is_buf);
    napi_is_array(env, argv[1], &is_arr);
    if (is_arr)
    {
        uint32_t len = 0, i;
        napi_get_array_length(env, argv[1], &len);
        for (i = 0; i < len; i++)
        {
            napi_value e;
            void      *p = NULL;
            size_t     n = 0;
            napi_get_element(env, argv[1], i, &e);
            if (napi_get_buffer_info(env, e, &p, &n) != napi_ok)
            {
                snprintf(err, sizeof(err), "copyFrom: array entries must be Buffers");
                goto fail;
            }
            if (n && copy_put(c->pg, p, n, err, sizeof(err)) != 0) goto fail;
        }
    }
    else if (is_buf)
    {
        void  *p = NULL;
        size_t n = 0;
        napi_get_buffer_info(env, argv[1], &p, &n);
        if (n && copy_put(c->pg, p, n, err, sizeof(err)) != 0) goto fail;
    }
    else
    {
        napi_get_value_string_utf8(env, argv[1], NULL, 0, &str_len);
        str = malloc(str_len + 1);
        napi_get_value_string_utf8(env, argv[1], str, str_len + 1, &str_len);
        if (str_len && copy_put(c->pg, str, str_len, err, sizeof(err)) != 0) goto fail;
    }

    for (;;)
    {
        int rc = PQputCopyEnd(c->pg, NULL);
        if (rc < 0)
        {
            snprintf(err, sizeof(err), "%s", PQerrorMessage(c->pg));
            goto fail;
        }
        if (rc > 0) break;
        if (PQflush(c->pg) != 0)
        {
            snprintf(err, sizeof(err), "%s", PQerrorMessage(c->pg));
            goto fail;
        }
    }

    res = PQgetResult(c->pg);
    if (res == NULL || PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        snprintf(err, sizeof(err), "libpq: %s", res ? PQresultErrorMessage(res) : PQerrorMessage(c->pg));
        if (res) PQclear(res);
        goto fail;
    }
    PQclear(res);
    while ((res = PQgetResult(c->pg)) != NULL)
        PQclear(res);

    free(sql);
    free(str);
    return fastpostgres_mk_undefined(env);

fail:
    while ((res = PQgetResult(c->pg)) != NULL)
        PQclear(res); /* resync */
    free(sql);
    free(str);
    napi_throw_error(env, NULL, err);
    return fastpostgres_mk_undefined(env);
}

void fastpostgres_register_connection(napi_env env, napi_value exports)
{
    napi_value               conn_class;
    napi_property_descriptor conn_props[] = {
        {"exec", NULL, conn_exec, NULL, NULL, NULL, napi_default, NULL},
        {"copyFrom", NULL, conn_copy_from, NULL, NULL, NULL, napi_default, NULL},
        {"close", NULL, conn_close, NULL, NULL, NULL, napi_default, NULL},
    };

    napi_define_class(env, "Connection", NAPI_AUTO_LENGTH, conn_new, NULL,
                      sizeof(conn_props) / sizeof(conn_props[0]), conn_props, &conn_class);
    napi_set_named_property(env, exports, "Connection", conn_class);
}
