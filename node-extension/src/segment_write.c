/* segment_write.c — writeSegment(segName, columns): publish JS typed arrays as
 * a columnar segment. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fastpostgres_shm.h"

#include "fastpostgres_map.h"
#include "napi_util.h"

/* columns: [{ name, kind, pgtype, value }]; value is a TypedArray, or for text
 * { bytes: Uint8Array, offsets: Int32Array }. No nulls. */
typedef struct {
    char       *name;
    uint32_t    name_len;
    uint32_t    kind;
    uint32_t    pgtype;
    const void *data;
    uint64_t    data_len;
} WCol;

static int ta_elem_size(napi_typedarray_type t)
{
    switch (t)
    {
        case napi_int8_array:
        case napi_uint8_array:
        case napi_uint8_clamped_array:
            return 1;
        case napi_int16_array:
        case napi_uint16_array:
            return 2;
        case napi_int32_array:
        case napi_uint32_array:
        case napi_float32_array:
            return 4;
        case napi_float64_array:
        case napi_bigint64_array:
        case napi_biguint64_array:
            return 8;
        default:
            return 0;
    }
}

static napi_value write_segment(napi_env env, napi_callback_info info)
{
    size_t         argc = 2;
    napi_value     argv[2], cols, out;
    napi_valuetype ot;
    char           name[256];
    size_t         nlen = sizeof(name);
    char           path[300];
    uint32_t       ncols     = 0, c;
    uint64_t       nrows     = 0, null_bytes, off, total;
    bool           have_rows = false;
    size_t         seg_size, map_size;
    void          *map;
    char          *p, *base;
    WCol          *wc = NULL;

    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    if (argc < 2)
    {
        napi_throw_type_error(env, NULL, "writeSegment(segName, columns)");
        return fastpostgres_mk_undefined(env);
    }
    napi_get_value_string_utf8(env, argv[0], name, sizeof(name), &nlen);
    snprintf(path, sizeof(path), "/%s", name);
    napi_typeof(env, argv[1], &ot);
    if (ot != napi_object)
    {
        napi_throw_type_error(env, NULL, "columns must be an array");
        return fastpostgres_mk_undefined(env);
    }
    cols = argv[1];
    napi_get_array_length(env, cols, &ncols);
    if (ncols == 0)
    {
        napi_throw_error(env, NULL, "writeSegment: no columns");
        return fastpostgres_mk_undefined(env);
    }

    wc = calloc(ncols, sizeof(*wc));
    for (c = 0; c < ncols; c++)
    {
        napi_value col, v, nm;
        char       cname[128];
        size_t     cname_len = sizeof(cname);
        int32_t    kind = 0, pgtype = 0;

        napi_get_element(env, cols, c, &col);
        napi_get_named_property(env, col, "name", &nm);
        napi_get_value_string_utf8(env, nm, cname, sizeof(cname), &cname_len);
        wc[c].name = malloc(cname_len + 1);
        memcpy(wc[c].name, cname, cname_len);
        wc[c].name[cname_len] = 0;
        wc[c].name_len        = (uint32_t)cname_len;

        napi_get_named_property(env, col, "kind", &v);
        napi_get_value_int32(env, v, &kind);
        wc[c].kind = (uint32_t)kind;
        napi_get_named_property(env, col, "pgtype", &v);
        napi_get_value_int32(env, v, &pgtype);
        wc[c].pgtype = (uint32_t)pgtype;

        napi_get_named_property(env, col, "value", &v);
        if (wc[c].kind == FASTPOSTGRES_KIND_TEXT)
        {
            napi_value           nb, no, ab;
            napi_typedarray_type t;
            void                *bp = NULL, *op = NULL;
            size_t               bl = 0, ol = 0, bo = 0;
            napi_get_named_property(env, v, "bytes", &nb);
            napi_get_named_property(env, v, "offsets", &no);
            if (napi_get_typedarray_info(env, nb, &t, &bl, &bp, &ab, &bo) != napi_ok ||
                napi_get_typedarray_info(env, no, &t, &ol, &op, &ab, &bo) != napi_ok)
            {
                napi_throw_type_error(env, NULL, "text value must be {bytes, offsets}");
                goto fail;
            }
            wc[c].data     = op; /* the int32 offset table; bytes follow it */
            wc[c].data_len = (uint64_t)ol * 4 + bl;
            if (ol == 0)
            {
                napi_throw_error(env, NULL, "text offsets must be non-empty");
                goto fail;
            }
            if (!have_rows)
            {
                nrows     = ol - 1;
                have_rows = true;
            }
            else if (ol - 1 != nrows)
            {
                napi_throw_error(env, NULL, "columns have different row counts");
                goto fail;
            }
        }
        else
        {
            napi_value           ab;
            napi_typedarray_type t;
            void                *dp  = NULL;
            size_t               len = 0, bo = 0;
            if (napi_get_typedarray_info(env, v, &t, &len, &dp, &ab, &bo) != napi_ok)
            {
                napi_throw_type_error(env, NULL, "value must be a TypedArray");
                goto fail;
            }
            if (ta_elem_size(t) != fastpostgres_kind_width((uint8_t)wc[c].kind))
            {
                napi_throw_type_error(env, NULL, "value type does not match kind");
                goto fail;
            }
            wc[c].data     = dp;
            wc[c].data_len = (uint64_t)len * (uint64_t)fastpostgres_kind_width((uint8_t)wc[c].kind);
            if (!have_rows)
            {
                nrows     = len;
                have_rows = true;
            }
            else if (len != nrows)
            {
                napi_throw_error(env, NULL, "columns have different row counts");
                goto fail;
            }
        }
    }

    null_bytes = (nrows + 7) / 8;
    off        = 8 + 4 + 4 + 4 + 8;
    for (c = 0; c < ncols; c++)
    {
        off += 4 + wc[c].name_len + 1 + 4 + null_bytes + 8;
        off = FASTPOSTGRES_ALIGN8(off);
        off += wc[c].data_len;
    }
    total = off;
    if (total + FASTPOSTGRES_SHM_HEADER_SIZE > (uint64_t)FASTPOSTGRES_SEGMENT_BYTES)
    {
        napi_throw_error(env, NULL, "writeSegment: result exceeds the 1 GiB segment");
        goto fail;
    }
    seg_size = FASTPOSTGRES_SEGMENT_BYTES;

    map_size = (size_t)(FASTPOSTGRES_SHM_HEADER_SIZE + total);
    map      = fastpostgres_map_rw(path, seg_size, map_size);
    if (map == NULL)
    {
        napi_throw_error(env, NULL, "writeSegment: cannot map /dev/shm segment");
        goto fail;
    }

    base = (char *)map + FASTPOSTGRES_SHM_HEADER_SIZE;
    p    = base;
    memcpy(p, FASTPOSTGRES_COLUMNAR_MAGIC, 8);
    p += 8;
    fastpostgres_put_u32(p, FASTPOSTGRES_COLUMNAR_VERSION);
    p += 4;
    fastpostgres_put_u32(p, 1);
    p += 4; /* flags: little-endian */
    fastpostgres_put_u32(p, ncols);
    p += 4;
    fastpostgres_put_u64(p, nrows);
    p += 8;
    for (c = 0; c < ncols; c++)
    {
        fastpostgres_put_u32(p, wc[c].name_len);
        p += 4;
        memcpy(p, wc[c].name, wc[c].name_len);
        p += wc[c].name_len;
        *p++ = (char)wc[c].kind;
        fastpostgres_put_u32(p, wc[c].pgtype);
        p += 4;
        memset(p, 0, null_bytes);
        p += null_bytes; /* no nulls */
        fastpostgres_put_u64(p, wc[c].data_len);
        p += 8;
        while (((size_t)(p - base)) & 7)
            *p++ = 0; /* 8-byte aligned column data */
        memcpy(p, wc[c].data, (size_t)wc[c].data_len);
        p += wc[c].data_len;
    }
    fastpostgres_shm_publish(map, seg_size, total);
    munmap(map, map_size);

    for (c = 0; c < ncols; c++)
        free(wc[c].name);
    free(wc);

    napi_create_double(env, (double)total, &out);
    return out;

fail:
    if (wc)
    {
        for (c = 0; c < ncols; c++)
            free(wc[c].name);
        free(wc);
    }
    return fastpostgres_mk_undefined(env);
}

void fastpostgres_register_write(napi_env env, napi_value exports)
{
    napi_value write_fn;
    napi_create_function(env, "writeSegment", NAPI_AUTO_LENGTH, write_segment, NULL, &write_fn);
    napi_set_named_property(env, exports, "writeSegment", write_fn);
}
