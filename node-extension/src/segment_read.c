/* segment_read.c — read(segName[, options]): expose a columnar segment as JS
 * typed arrays. Zero-copy by default (the arrays alias the mapped segment on
 * Node-owned buffers when { copy: true }). */
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "fastpostgres_shm.h"

#include "fastpostgres_map.h"
#include "napi_util.h"

/* Read-only segments are mapped once and cached for the process lifetime: the
 * worker rewrites the same segment in place, so there is nothing to re-map per
 * chunk (each read revalidates the seqlock header instead). */
typedef struct {
    char   name[256];
    void  *base;
    size_t size;
} ReadMap;

static ReadMap read_maps[32];
static int     read_map_count = 0;

static void *read_map_get(const char *path, const char *name, size_t *size_out)
{
    int i;

    for (i = 0; i < read_map_count; i++)
    {
        if (strcmp(read_maps[i].name, name) == 0)
        {
            *size_out = read_maps[i].size;
            return read_maps[i].base;
        }
    }
    if (read_map_count >= (int)(sizeof(read_maps) / sizeof(read_maps[0]))) return NULL;

    {
        void *base = fastpostgres_map_ro(path, size_out);
        if (base == NULL) return NULL;
        snprintf(read_maps[read_map_count].name, sizeof(read_maps[0].name), "%s", name);
        read_maps[read_map_count].base = base;
        read_maps[read_map_count].size = *size_out;
        read_map_count++;
        return base;
    }
}

static napi_value typed_over(napi_env env, napi_value ab, napi_typedarray_type type, size_t count,
                             size_t offset)
{
    napi_value v;
    if (napi_create_typedarray(env, type, count, ab, offset, &v) != napi_ok) return NULL;
    return v;
}

static napi_value read_impl(napi_env env, napi_callback_info info)
{
    size_t             argc = 2;
    napi_value         argv[2], handlers = NULL, on_kind = NULL, on_column = NULL;
    char               name[256];
    size_t             nlen = sizeof(name);
    char               path[300];
    size_t             size = 0;
    void              *map;
    const uint8_t     *payload;
    uint64_t           payload_len = 0;
    FastpostgresReader rd;
    FastpostgresColumn col;
    bool               copy = false;
    napi_value         columns, result, ext_ab = NULL;
    uint32_t           c;

    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    if (argc < 1)
    {
        napi_throw_type_error(env, NULL, "read(segName[, options])");
        return fastpostgres_mk_undefined(env);
    }

    napi_get_value_string_utf8(env, argv[0], name, sizeof(name), &nlen);
    snprintf(path, sizeof(path), "/%s", name);

    if (argc >= 2)
    {
        napi_valuetype ot;
        napi_typeof(env, argv[1], &ot);
        if (ot == napi_object)
        {
            bool       b;
            napi_value v;
            if (napi_get_named_property(env, argv[1], "copy", &v) == napi_ok &&
                napi_get_value_bool(env, v, &b) == napi_ok)
                copy = b;
            if (napi_get_named_property(env, argv[1], "handlers", &handlers) == napi_ok)
            {
                napi_valuetype ht;
                napi_typeof(env, handlers, &ht);
                if (ht == napi_object)
                {
                    napi_valuetype kt;
                    napi_get_named_property(env, handlers, "onKind", &on_kind);
                    if (napi_typeof(env, on_kind, &kt) != napi_ok || kt != napi_object) on_kind = NULL;
                    napi_get_named_property(env, handlers, "onColumn", &on_column);
                    if (napi_typeof(env, on_column, &kt) != napi_ok || kt != napi_object) on_column = NULL;
                }
            }
        }
    }

    map = read_map_get(path, name, &size);
    if (map == NULL)
    {
        napi_throw_error(env, NULL, "cannot map /dev/shm segment");
        return fastpostgres_mk_undefined(env);
    }

    if (!fastpostgres_shm_validate(map, size, &payload_len) ||
        !fastpostgres_reader_init(&rd, (const uint8_t *)map + FASTPOSTGRES_SHM_HEADER_SIZE, payload_len))
    {
        napi_throw_error(env, NULL, "not a fastpostgres segment");
        return fastpostgres_mk_undefined(env);
    }
    payload = (const uint8_t *)map + FASTPOSTGRES_SHM_HEADER_SIZE;
#ifdef MADV_POPULATE_READ
    /* fault just the payload (the mapping covers the whole 1 GiB segment) */
    (void)madvise((void *)payload, payload_len, MADV_POPULATE_READ);
#endif

    if (!copy)
    {
        /* the cached mapping outlives the buffer, so no finalizer is needed */
        if (napi_create_external_arraybuffer(env, (void *)payload, payload_len, NULL, NULL, &ext_ab) !=
            napi_ok)
        {
            napi_throw_error(env, NULL, "cannot create arraybuffer");
            return fastpostgres_mk_undefined(env);
        }
    }

    napi_create_array_with_length(env, rd.ncols, &columns);

    for (c = 0; c < rd.ncols; c++)
    {
        napi_value entry, jname, ab = NULL, value = NULL;
        size_t     base_off = 0;
        napi_value cb       = NULL;

        if (!fastpostgres_reader_next(&rd, &col)) break;

        napi_create_string_utf8(env, col.name, col.name_len, &jname);
        if (!copy) base_off = (size_t)(col.data - payload);

        if (col.kind == FASTPOSTGRES_KIND_TEXT)
        {
            size_t     offs_bytes = (size_t)(col.nrows + 1) * 4;
            size_t     text_bytes = (size_t)col.data_len - offs_bytes;
            napi_value bytes_v, offsets_v;

            if (copy)
            {
                void *dst;
                napi_create_arraybuffer(env, (size_t)col.data_len, &dst, &ab);
                memcpy(dst, col.data, (size_t)col.data_len);
            }
            else
            {
                ab = ext_ab;
            }
            bytes_v   = typed_over(env, ab, napi_uint8_array, text_bytes, base_off + offs_bytes);
            offsets_v = typed_over(env, ab, napi_int32_array, (size_t)col.nrows + 1, base_off);
            if (bytes_v == NULL || offsets_v == NULL)
            {
                napi_throw_error(env, NULL, "column data is not aligned for a typed array");
                return fastpostgres_mk_undefined(env);
            }
            napi_create_object(env, &value);
            napi_set_named_property(env, value, "bytes", bytes_v);
            napi_set_named_property(env, value, "offsets", offsets_v);
        }
        else
        {
            napi_typedarray_type type;
            switch (col.kind)
            {
                case FASTPOSTGRES_KIND_BOOL:
                    type = napi_uint8_array;
                    break;
                case FASTPOSTGRES_KIND_INT16:
                    type = napi_int16_array;
                    break;
                case FASTPOSTGRES_KIND_INT32:
                    type = napi_int32_array;
                    break;
                case FASTPOSTGRES_KIND_INT64:
                    type = napi_bigint64_array;
                    break;
                case FASTPOSTGRES_KIND_FLOAT4:
                    type = napi_float32_array;
                    break;
                default:
                    type = napi_float64_array;
                    break;
            }
            if (copy)
            {
                void *dst;
                napi_create_arraybuffer(env, (size_t)col.data_len, &dst, &ab);
                memcpy(dst, col.data, (size_t)col.data_len);
            }
            else
            {
                ab = ext_ab;
            }
            value = typed_over(env, ab, type, (size_t)col.nrows, base_off);
            if (value == NULL)
            {
                napi_throw_error(env, NULL, "column data is not aligned for a typed array");
                return fastpostgres_mk_undefined(env);
            }
        }

        napi_create_object(env, &entry);
        napi_set_named_property(env, entry, "name", jname);
        napi_set_named_property(env, entry, "pgtype", fastpostgres_mk_i32(env, (int32_t)col.pgtype));
        napi_set_named_property(env, entry, "kind", fastpostgres_mk_i32(env, (int32_t)col.kind));
        napi_set_named_property(env, entry, "value", value);
        napi_set_element(env, columns, c, entry);

        /* dispatch: onColumn[name] wins over onKind[kind] */
        if (on_column)
        {
            napi_value fn;
            if (napi_get_property(env, on_column, jname, &fn) == napi_ok && fastpostgres_is_function(env, fn))
                cb = fn;
        }
        if (cb == NULL && on_kind)
        {
            char       kbuf[16];
            napi_value key, fn;
            snprintf(kbuf, sizeof(kbuf), "%d", col.kind);
            napi_create_string_utf8(env, kbuf, NAPI_AUTO_LENGTH, &key);
            if (napi_get_property(env, on_kind, key, &fn) == napi_ok && fastpostgres_is_function(env, fn))
                cb = fn;
        }
        if (cb)
        {
            napi_value args[2] = {entry, value};
            napi_value ignored;
            bool       pending = false;
            napi_call_function(env, fastpostgres_mk_undefined(env), cb, 2, args, &ignored);
            napi_is_exception_pending(env, &pending);
            if (pending)
            {
                return fastpostgres_mk_undefined(env);
            }
        }
    }

    napi_create_object(env, &result);
    {
        napi_value rc;
        napi_create_double(env, (double)rd.nrows, &rc);
        napi_set_named_property(env, result, "rowCount", rc);
    }
    napi_set_named_property(env, result, "columns", columns);
    return result;
}

void fastpostgres_register_read(napi_env env, napi_value exports)
{
    napi_value read_fn;
    napi_create_function(env, "read", NAPI_AUTO_LENGTH, read_impl, NULL, &read_fn);
    napi_set_named_property(env, exports, "read", read_fn);
}
