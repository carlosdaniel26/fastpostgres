/* synth.cpp — native generators for the benchmark, so the Python benchmark does
 * no per-row work: it calls these and consumes the result.
 *
 *   synth_segment(seg, n)  generate the bench rows straight into /dev/shm
 *                          (no intermediate Python objects, no extra copy)
 *   synth_copy(n)          -> [bytes, ...] for Connection.copy_from()
 */
#include "synth.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "fastpostgres_shm.h"
#include "shm_map.h"

static size_t
dec_len(uint64_t v)
{
    size_t n = 1;
    while (v >= 10)
    {
        v /= 10;
        n++;
    }
    return n;
}

static size_t
put_dec(char *p, uint64_t v)
{
    char   tmp[24];
    size_t n = 0, i;
    if (v == 0)
    {
        p[0] = '0';
        return 1;
    }
    while (v)
    {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (i = 0; i < n; i++)
        p[i] = tmp[n - 1 - i];
    return n;
}

/* The bench table: id, account_id, balance, active, name, email, score, age. */
static const char *COLS_NAME[8] = {"id", "account_id", "balance", "active", "name", "email", "score", "age"};
static const int COLS_KIND[8] = {FASTPOSTGRES_KIND_INT32,  FASTPOSTGRES_KIND_INT64, FASTPOSTGRES_KIND_FLOAT8,
                                 FASTPOSTGRES_KIND_BOOL,   FASTPOSTGRES_KIND_TEXT,  FASTPOSTGRES_KIND_TEXT,
                                 FASTPOSTGRES_KIND_FLOAT8, FASTPOSTGRES_KIND_INT32};
static const int COLS_PGTYPE[8] = {23, 20, 701, 16, 25, 25, 701, 23};

/* Write "user<r><suffix>" as an int32 offset table followed by the bytes. */
static void
put_text_col(char *p, uint64_t n, const char *prefix, size_t pre, const char *suffix, size_t suf)
{
    int32_t *off = (int32_t *)p;
    char    *b = p + (n + 1) * 4, *w = b;
    uint64_t r;
    off[0] = 0;
    for (r = 0; r < n; r++)
    {
        memcpy(w, prefix, pre);
        w += pre;
        w += put_dec(w, r);
        if (suf)
        {
            memcpy(w, suffix, suf);
            w += suf;
        }
        off[r + 1] = (int32_t)(w - b);
    }
}

static PyObject *
synth_segment(PyObject *, PyObject *args)
{
    const char        *name;
    unsigned long long n;
    if (!PyArg_ParseTuple(args, "sK", &name, &n)) return nullptr;

    std::string path = "/" + std::string(name);
    uint64_t    data_len[8], null_bytes, off, total;
    size_t      pre_user = 4, suf_mail = sizeof("@example.com") - 1;

    data_len[0] = n * 4;
    data_len[1] = n * 8;
    data_len[2] = n * 8;
    data_len[3] = n * 1;
    data_len[6] = n * 8;
    data_len[7] = n * 4;
    {
        uint64_t name_bytes = 0, mail_bytes = 0, r;
        for (r = 0; r < n; r++)
        {
            size_t d = dec_len(r);
            name_bytes += pre_user + d;
            mail_bytes += pre_user + d + suf_mail;
        }
        data_len[4] = (n + 1) * 4 + name_bytes;
        data_len[5] = (n + 1) * 4 + mail_bytes;
    }

    null_bytes = (n + 7) / 8;
    off        = 8 + 4 + 4 + 4 + 8;
    for (int c = 0; c < 8; c++)
    {
        off += 4 + strlen(COLS_NAME[c]) + 1 + 4 + null_bytes + 8;
        off = FASTPOSTGRES_ALIGN8(off);
        off += data_len[c];
    }
    total = off;
    if (total + FASTPOSTGRES_SHM_HEADER_SIZE > (uint64_t)FASTPOSTGRES_SEGMENT_BYTES)
    {
        PyErr_SetString(PyExc_ValueError, "synth_segment: result exceeds the 1 GiB segment");
        return nullptr;
    }
    size_t map_size = (size_t)(FASTPOSTGRES_SHM_HEADER_SIZE + total);

    void *map = fp_map_rw(path.c_str(), FASTPOSTGRES_SEGMENT_BYTES, map_size);
    if (map == nullptr)
    {
        PyErr_SetString(PyExc_OSError, "synth_segment: cannot map /dev/shm segment");
        return nullptr;
    }

    char *base = (char *)map + FASTPOSTGRES_SHM_HEADER_SIZE;
    char *p    = base;
    memcpy(p, FASTPOSTGRES_COLUMNAR_MAGIC, 8);
    p += 8;
    fastpostgres_put_u32(p, FASTPOSTGRES_COLUMNAR_VERSION);
    p += 4;
    fastpostgres_put_u32(p, 1);
    p += 4; /* flags: little-endian */
    fastpostgres_put_u32(p, 8);
    p += 4;
    fastpostgres_put_u64(p, n);
    p += 8;

    for (int c = 0; c < 8; c++)
    {
        size_t nl = strlen(COLS_NAME[c]);
        fastpostgres_put_u32(p, (uint32_t)nl);
        p += 4;
        memcpy(p, COLS_NAME[c], nl);
        p += nl;
        *p++ = (char)COLS_KIND[c];
        fastpostgres_put_u32(p, (uint32_t)COLS_PGTYPE[c]);
        p += 4;
        memset(p, 0, null_bytes);
        p += null_bytes; /* no nulls */
        fastpostgres_put_u64(p, data_len[c]);
        p += 8;
        while (((size_t)(p - base)) & 7)
            *p++ = 0; /* 8-byte aligned column data */

        uint64_t r;
        switch (c)
        {
            case 0: {
                int32_t *v = (int32_t *)p;
                for (r = 0; r < n; r++)
                    v[r] = (int32_t)r;
                break;
            }
            case 1: {
                int64_t *v = (int64_t *)p;
                for (r = 0; r < n; r++)
                    v[r] = (int64_t)(r * 1000);
                break;
            }
            case 2: {
                double *v = (double *)p;
                for (r = 0; r < n; r++)
                    v[r] = (double)r * 1.5;
                break;
            }
            case 3: {
                uint8_t *v = (uint8_t *)p;
                for (r = 0; r < n; r++)
                    v[r] = (r & 1) ? 0 : 1;
                break;
            }
            case 4:
                put_text_col(p, n, "user", pre_user, "", 0);
                break;
            case 5:
                put_text_col(p, n, "user", pre_user, "@example.com", suf_mail);
                break;
            case 6: {
                double *v = (double *)p;
                for (r = 0; r < n; r++)
                    v[r] = (double)r * 0.25;
                break;
            }
            default: {
                int32_t *v = (int32_t *)p;
                for (r = 0; r < n; r++)
                    v[r] = (int32_t)(r % 100);
                break;
            }
        }
        p += data_len[c];
    }

    fastpostgres_shm_publish(map, FASTPOSTGRES_SEGMENT_BYTES, total);
    munmap(map, map_size);
    return PyLong_FromUnsignedLongLong(total);
}

/* COPY FROM STDIN payload (tab-separated), built natively in 1 MiB chunks. */
static PyObject *
synth_copy(PyObject *, PyObject *args)
{
    unsigned long long n;
    if (!PyArg_ParseTuple(args, "K", &n)) return nullptr;

    const size_t cap   = 1 << 20;
    char        *chunk = (char *)malloc(cap);
    PyObject    *list  = PyList_New(0);
    if (!chunk || !list)
    {
        free(chunk);
        Py_XDECREF(list);
        return PyErr_NoMemory();
    }

    size_t off = 0;
    for (unsigned long long i = 0; i < n; i++)
    {
        if (cap - off < 256)
        {
            PyObject *b = PyBytes_FromStringAndSize(chunk, (Py_ssize_t)off);
            if (PyList_Append(list, b) != 0)
            {
                Py_DECREF(b);
                goto fail;
            }
            Py_DECREF(b);
            off = 0;
        }
        off += (size_t)snprintf(chunk + off, cap - off,
                                "%llu\t%lld\t%.1f\t%c\tuser%llu\tuser%llu@example.com\t%.2f\t%llu\n", i,
                                (long long)(i * 1000), (double)i * 1.5, (i & 1) ? 'f' : 't', i, i,
                                (double)i * 0.25, (unsigned long long)(i % 100));
    }
    if (off)
    {
        PyObject *b = PyBytes_FromStringAndSize(chunk, (Py_ssize_t)off);
        if (PyList_Append(list, b) != 0)
        {
            Py_DECREF(b);
            goto fail;
        }
        Py_DECREF(b);
    }
    free(chunk);
    return list;

fail:
    free(chunk);
    Py_DECREF(list);
    return nullptr;
}

static PyMethodDef synth_methods[] = {
    {"synth_segment", synth_segment, METH_VARARGS, "synth_segment(seg, n) -> int: bench rows into /dev/shm"},
    {"synth_copy", synth_copy, METH_VARARGS, "synth_copy(n) -> list[bytes]: COPY FROM payload"},
    {nullptr},
};

void
fp_register_synth(PyObject *module)
{
    for (PyMethodDef *m = synth_methods; m->ml_name != nullptr; m++)
    {
        PyObject *fn = PyCFunction_NewEx(m, nullptr, nullptr);
        if (fn == nullptr) continue;
        PyModule_AddObject(module, m->ml_name, fn);
    }
}
