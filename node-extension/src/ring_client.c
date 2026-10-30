/* ring_client.c — the JS Ring class: the control channel over /dev/shm. */
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "fastpostgres_shm.h"
#include "ring.h"

#include "connection.h"
#include "napi_util.h"

typedef struct {
    FastpostgresRing *ring;
    void             *map;
    size_t            size;
    char              seg[128];  /* the ring itself */
    char              data[128]; /* the columnar segment of the current stream */
    uint32_t          tail;      /* push mode: consumer slot index */
} RingState;

static void ring_finalize(napi_env env, void *data, void *hint)
{
    RingState *st = data;
    (void)env;
    (void)hint;
    if (!st) return;
    if (st->map) munmap(st->map, st->size);
    free(st);
}

static napi_value ring_new(napi_env env, napi_callback_info info)
{
    size_t     argc = 1;
    napi_value argv[1], self;
    char       seg[128];
    size_t     seg_len = sizeof(seg);
    char       path[160];
    int        fd;
    void      *map;
    RingState *st;
    size_t     i;

    napi_get_cb_info(env, info, &argc, argv, &self, NULL);
    if (argc < 1 || napi_get_value_string_utf8(env, argv[0], seg, sizeof(seg), &seg_len) != napi_ok)
    {
        napi_throw_type_error(env, NULL, "Ring(segName)");
        return fastpostgres_mk_undefined(env);
    }
    for (i = 0; i < seg_len; i++)
    {
        char ch = seg[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
              ch == '-'))
        {
            napi_throw_error(env, NULL, "Ring: invalid segment name");
            return fastpostgres_mk_undefined(env);
        }
    }

    snprintf(path, sizeof(path), "/%s", seg);
    fd = shm_open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0)
    {
        napi_throw_error(env, NULL, "Ring: cannot create /dev/shm segment");
        return fastpostgres_mk_undefined(env);
    }
    if (ftruncate(fd, FASTPOSTGRES_RING_BYTES) != 0)
    {
        close(fd);
        napi_throw_error(env, NULL, "Ring: ftruncate failed");
        return fastpostgres_mk_undefined(env);
    }
    map = mmap(NULL, FASTPOSTGRES_RING_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
    {
        napi_throw_error(env, NULL, "Ring: mmap failed");
        return fastpostgres_mk_undefined(env);
    }

    {
        FastpostgresRing *r = (FastpostgresRing *)map;
        if (memcmp(r->magic, FASTPOSTGRES_RING_MAGIC, 8) != 0)
        {
            /* fresh segment: initialise the semaphores before the worker starts */
            memset(r, 0, FASTPOSTGRES_RING_BYTES);
            if (sem_init(&r->req_ready, 1, 0) != 0 || sem_init(&r->resp_ready, 1, 0) != 0 ||
                sem_init(&r->push_free, 1, FASTPOSTGRES_PUSH_SLOTS) != 0 ||
                sem_init(&r->push_ready, 1, 0) != 0)
            {
                munmap(map, FASTPOSTGRES_RING_BYTES);
                napi_throw_error(env, NULL, "Ring: sem_init failed");
                return fastpostgres_mk_undefined(env);
            }
            r->version = FASTPOSTGRES_RING_VERSION;
            memcpy(r->magic, FASTPOSTGRES_RING_MAGIC, 8);
        }
    }

    st       = calloc(1, sizeof(*st));
    st->ring = (FastpostgresRing *)map;
    st->map  = map;
    st->size = FASTPOSTGRES_RING_BYTES;
    snprintf(st->seg, sizeof(st->seg), "%s", seg);
    napi_wrap(env, self, st, ring_finalize, NULL, NULL);
    return self;
}

/* Send one request and block for its response; returns nrows, or -1 on error
 * (leaving a JS exception pending). */
static int64_t ring_request(napi_env env, RingState *st, uint32_t op, const char *a1, const char *a2,
                            uint32_t batch)
{
    FastpostgresRing *r = st->ring;
    struct timespec   ts;
    size_t            n = 0;

    if (a1)
    {
        size_t l = strlen(a1);
        if (l + 1 > FASTPOSTGRES_RING_ARG)
        {
            napi_throw_error(env, NULL, "ring: argument too long");
            return -1;
        }
        memcpy(r->arg, a1, l + 1);
        n = l + 1;
    }
    else
    {
        r->arg[0] = 0;
        n         = 1;
    }
    if (a2)
    {
        size_t l = strlen(a2);
        if (n + l + 1 > FASTPOSTGRES_RING_ARG)
        {
            napi_throw_error(env, NULL, "ring: argument too long");
            return -1;
        }
        memcpy(r->arg + n, a2, l + 1);
    }

    r->op       = op;
    r->batch    = batch;
    r->status   = 0;
    r->nrows    = 0;
    r->error[0] = 0;

    __atomic_add_fetch(&r->req_seq, 1, __ATOMIC_RELEASE);
    sem_post(&r->req_ready);

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 10;
    if (sem_timedwait(&r->resp_ready, &ts) != 0)
    {
        napi_throw_error(env, NULL, "ring: timeout waiting for the worker");
        return -1;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (r->status != 0)
    {
        napi_throw_error(env, NULL, r->error[0] ? r->error : "ring: worker error");
        return -1;
    }
    return (int64_t)r->nrows;
}

static napi_value ring_start(napi_env env, napi_callback_info info)
{
    size_t                  argc = 1;
    napi_value              argv[1], self;
    RingState              *st = NULL;
    FastpostgresConnection *c  = NULL;
    char                    sql[256];
    PGresult               *res;
    int                     waited = 0;

    napi_get_cb_info(env, info, &argc, argv, &self, NULL);
    napi_unwrap(env, self, (void **)&st);
    if (argc < 1 || napi_unwrap(env, argv[0], (void **)&c) != napi_ok || !c || !c->pg)
    {
        napi_throw_type_error(env, NULL, "ring.start(conn)");
        return fastpostgres_mk_undefined(env);
    }

    snprintf(sql, sizeof(sql), "SELECT fastpostgres_ring_start('%s')", st->seg);
    res = PQexec(c->pg, sql);
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        char msg[512];
        snprintf(msg, sizeof(msg), "libpq: %s", PQresultErrorMessage(res));
        PQclear(res);
        napi_throw_error(env, NULL, msg);
        return fastpostgres_mk_undefined(env);
    }
    PQclear(res);

    while (__atomic_load_n(&st->ring->worker_ready, __ATOMIC_ACQUIRE) == 0)
    {
        struct timespec pause = {0, 1000000}; /* 1 ms */
        nanosleep(&pause, NULL);
        if (++waited > 10000)
        {
            napi_throw_error(env, NULL, "ring: worker did not become ready");
            return fastpostgres_mk_undefined(env);
        }
    }
    return fastpostgres_mk_undefined(env);
}

static napi_value ring_open(napi_env env, napi_callback_info info)
{
    size_t     argc = 2;
    napi_value argv[2], self;
    RingState *st = NULL;
    char       seg[128], query[FASTPOSTGRES_RING_ARG];
    size_t     slen = sizeof(seg), qlen = 0;

    napi_get_cb_info(env, info, &argc, argv, &self, NULL);
    napi_unwrap(env, self, (void **)&st);
    if (argc < 2 || napi_get_value_string_utf8(env, argv[0], seg, sizeof(seg), &slen) != napi_ok ||
        napi_get_value_string_utf8(env, argv[1], query, sizeof(query), &qlen) != napi_ok)
    {
        napi_throw_type_error(env, NULL, "ring.open(seg, query)");
        return fastpostgres_mk_undefined(env);
    }
    snprintf(st->data, sizeof(st->data), "%s", seg);
    if (ring_request(env, st, FASTPOSTGRES_OP_OPEN, st->data, query, 0) < 0)
        return fastpostgres_mk_undefined(env);
    return fastpostgres_mk_undefined(env);
}

static napi_value ring_next(napi_env env, napi_callback_info info)
{
    size_t     argc = 1;
    napi_value argv[1], self, out;
    RingState *st    = NULL;
    int64_t    batch = 100000;
    int64_t    nrows;

    napi_get_cb_info(env, info, &argc, argv, &self, NULL);
    napi_unwrap(env, self, (void **)&st);
    if (argc >= 1)
    {
        double b;
        if (napi_get_value_double(env, argv[0], &b) == napi_ok && b > 0) batch = (int64_t)b;
    }
    nrows = ring_request(env, st, FASTPOSTGRES_OP_NEXT, st->data, NULL, (uint32_t)batch);
    if (nrows < 0) return fastpostgres_mk_undefined(env);
    napi_create_double(env, (double)nrows, &out);
    return out;
}

static napi_value ring_close(napi_env env, napi_callback_info info)
{
    size_t     argc = 0;
    napi_value self;
    RingState *st = NULL;

    napi_get_cb_info(env, info, &argc, NULL, &self, NULL);
    napi_unwrap(env, self, (void **)&st);
    if (ring_request(env, st, FASTPOSTGRES_OP_CLOSE, st->data, NULL, 0) < 0)
        return fastpostgres_mk_undefined(env);
    return fastpostgres_mk_undefined(env);
}

static napi_value ring_write(napi_env env, napi_callback_info info)
{
    size_t     argc = 2;
    napi_value argv[2], self, out;
    RingState *st = NULL;
    char       seg[128], table[128];
    size_t     slen = sizeof(seg), tlen = sizeof(table);
    int64_t    nrows;

    napi_get_cb_info(env, info, &argc, argv, &self, NULL);
    napi_unwrap(env, self, (void **)&st);
    if (argc < 2 || napi_get_value_string_utf8(env, argv[0], seg, sizeof(seg), &slen) != napi_ok ||
        napi_get_value_string_utf8(env, argv[1], table, sizeof(table), &tlen) != napi_ok)
    {
        napi_throw_type_error(env, NULL, "ring.write(seg, table)");
        return fastpostgres_mk_undefined(env);
    }
    nrows = ring_request(env, st, FASTPOSTGRES_OP_WRITE, seg, table, 0);
    if (nrows < 0) return fastpostgres_mk_undefined(env);
    napi_create_double(env, (double)nrows, &out);
    return out;
}

static napi_value ring_stop(napi_env env, napi_callback_info info)
{
    size_t            argc = 0;
    napi_value        self;
    RingState        *st = NULL;
    FastpostgresRing *r;

    napi_get_cb_info(env, info, &argc, NULL, &self, NULL);
    napi_unwrap(env, self, (void **)&st);
    r     = st->ring;
    r->op = FASTPOSTGRES_OP_STOP;
    __atomic_add_fetch(&r->req_seq, 1, __ATOMIC_RELEASE);
    sem_post(&r->req_ready);
    return fastpostgres_mk_undefined(env);
}

/* Push mode: send the query, then consume one JS object per row. */
static napi_value ring_push_open(napi_env env, napi_callback_info info)
{
    size_t            argc = 1;
    napi_value        argv[1], self;
    RingState        *st = NULL;
    FastpostgresRing *r;
    char              query[FASTPOSTGRES_RING_ARG];
    size_t            qlen = 0, l;

    napi_get_cb_info(env, info, &argc, argv, &self, NULL);
    napi_unwrap(env, self, (void **)&st);
    if (argc < 1 || napi_get_value_string_utf8(env, argv[0], query, sizeof(query), &qlen) != napi_ok)
    {
        napi_throw_type_error(env, NULL, "ring.pushOpen(query)");
        return fastpostgres_mk_undefined(env);
    }
    r = st->ring;
    l = strlen(query);
    if (l + 1 > FASTPOSTGRES_RING_ARG)
    {
        napi_throw_error(env, NULL, "ring: query too long");
        return fastpostgres_mk_undefined(env);
    }
    memcpy(r->arg, query, l + 1);
    r->op            = FASTPOSTGRES_OP_PUSH;
    r->push_status   = 0;
    r->push_error[0] = 0;
    st->tail         = 0;
    __atomic_add_fetch(&r->req_seq, 1, __ATOMIC_RELEASE);
    sem_post(&r->req_ready);
    return fastpostgres_mk_undefined(env);
}

static napi_value ring_push_rows(napi_env env, napi_callback_info info)
{
    size_t            argc = 1;
    napi_value        argv[1], self;
    RingState        *st = NULL;
    FastpostgresRing *r;
    napi_value        cb;
    char             *slots;
    bool              call_handler = true;

    napi_get_cb_info(env, info, &argc, argv, &self, NULL);
    napi_unwrap(env, self, (void **)&st);
    if (argc < 1 || !fastpostgres_is_function(env, argv[0]))
    {
        napi_throw_type_error(env, NULL, "ring.pushRows(handler)");
        return fastpostgres_mk_undefined(env);
    }
    r     = st->ring;
    cb    = argv[0];
    slots = (char *)st->map + FASTPOSTGRES_PUSH_OFFSET;

    for (;;)
    {
        struct timespec ts;
        char           *s;
        uint32_t        ncol, i;
        size_t          nullbytes, p;
        napi_value      row;

        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 10;
        if (sem_timedwait(&r->push_ready, &ts) != 0)
        {
            napi_throw_error(env, NULL, "ring: timeout waiting for a row");
            return fastpostgres_mk_undefined(env);
        }
        s = slots + (size_t)st->tail * FASTPOSTGRES_PUSH_SLOT;

        if (s[0] == FASTPOSTGRES_PUSH_DONE)
        {
            st->tail = (st->tail + 1) % FASTPOSTGRES_PUSH_SLOTS;
            sem_post(&r->push_free);
            break;
        }
        if (s[0] == FASTPOSTGRES_PUSH_ERROR)
        {
            char msg[FASTPOSTGRES_RING_ERR];
            snprintf(msg, sizeof(msg), "%s", r->push_error[0] ? r->push_error : "push: error");
            st->tail = (st->tail + 1) % FASTPOSTGRES_PUSH_SLOTS;
            sem_post(&r->push_free);
            napi_throw_error(env, NULL, msg);
            return fastpostgres_mk_undefined(env);
        }

        ncol      = r->push_ncols;
        nullbytes = (size_t)((ncol + 7) / 8);
        p         = 1 + nullbytes;
        napi_create_object(env, &row);

        for (i = 0; i < ncol; i++)
        {
            bool       isnull = (s[1 + (i >> 3)] >> (i & 7)) & 1;
            napi_value v;

            if (isnull)
            {
                napi_get_null(env, &v);
            }
            else
            {
                switch (r->push_cols[i].kind)
                {
                    case FASTPOSTGRES_KIND_BOOL:
                        napi_get_boolean(env, s[p++] != 0, &v);
                        break;
                    case FASTPOSTGRES_KIND_INT16: {
                        int16_t x;
                        memcpy(&x, s + p, 2);
                        p += 2;
                        napi_create_int32(env, x, &v);
                        break;
                    }
                    case FASTPOSTGRES_KIND_INT32: {
                        int32_t x;
                        memcpy(&x, s + p, 4);
                        p += 4;
                        napi_create_int32(env, x, &v);
                        break;
                    }
                    case FASTPOSTGRES_KIND_INT64: {
                        int64_t x;
                        memcpy(&x, s + p, 8);
                        p += 8;
                        napi_create_bigint_int64(env, x, &v);
                        break;
                    }
                    case FASTPOSTGRES_KIND_FLOAT4: {
                        float x;
                        memcpy(&x, s + p, 4);
                        p += 4;
                        napi_create_double(env, (double)x, &v);
                        break;
                    }
                    case FASTPOSTGRES_KIND_FLOAT8: {
                        double x;
                        memcpy(&x, s + p, 8);
                        p += 8;
                        napi_create_double(env, x, &v);
                        break;
                    }
                    default: {
                        uint32_t l;
                        memcpy(&l, s + p, 4);
                        p += 4;
                        napi_create_string_utf8(env, s + p, l, &v);
                        p += l;
                        break;
                    }
                }
            }
            napi_set_named_property(env, row, r->push_cols[i].name, v);
        }

        st->tail = (st->tail + 1) % FASTPOSTGRES_PUSH_SLOTS;
        sem_post(&r->push_free);

        if (call_handler)
        {
            napi_value ignored;
            bool       pending = false;
            napi_call_function(env, fastpostgres_mk_undefined(env), cb, 1, &row, &ignored);
            napi_is_exception_pending(env, &pending);
            if (pending) call_handler = false;
        }
    }
    return fastpostgres_mk_undefined(env);
}

void fastpostgres_register_ring(napi_env env, napi_value exports)
{
    napi_value               ring_class;
    napi_property_descriptor ring_props[] = {
        {"start", NULL, ring_start, NULL, NULL, NULL, napi_default, NULL},
        {"open", NULL, ring_open, NULL, NULL, NULL, napi_default, NULL},
        {"next", NULL, ring_next, NULL, NULL, NULL, napi_default, NULL},
        {"close", NULL, ring_close, NULL, NULL, NULL, napi_default, NULL},
        {"write", NULL, ring_write, NULL, NULL, NULL, napi_default, NULL},
        {"stop", NULL, ring_stop, NULL, NULL, NULL, napi_default, NULL},
        {"pushOpen", NULL, ring_push_open, NULL, NULL, NULL, napi_default, NULL},
        {"pushRows", NULL, ring_push_rows, NULL, NULL, NULL, napi_default, NULL},
    };

    napi_define_class(env, "Ring", NAPI_AUTO_LENGTH, ring_new, NULL,
                      sizeof(ring_props) / sizeof(ring_props[0]), ring_props, &ring_class);
    napi_set_named_property(env, exports, "Ring", ring_class);
}
