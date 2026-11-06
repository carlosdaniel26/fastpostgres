/* codec.cpp — turn PostgreSQL text values into Python objects by type OID, so
 * the client returns real types (int, float, bool, Decimal, datetime, bytes)
 * the way a native driver does, not bare strings. */
#include "codec.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <x86intrin.h>

#include <atomic>
#include <datetime.h>
#include <sys/mman.h>
#include <unistd.h>

/* These are written once by fp_codec_init and only read afterwards, so decoder
 * threads can share the borrowed references without a lock. Nothing here may
 * ever be reassigned or DECREF'd. */
static PyObject *s_decimal   = nullptr; /* decimal.Decimal */
static PyObject *s_timezone  = nullptr; /* datetime.timezone */
static PyObject *s_timedelta = nullptr; /* datetime.timedelta */
static PyObject *s_utc       = nullptr; /* datetime.timezone.utc */
static PyObject *s_uuid      = nullptr; /* uuid.UUID */

bool
fp_codec_init(void)
{
    PyDateTime_IMPORT;
    PyObject *dec = PyImport_ImportModule("decimal");
    if (!dec) return false;
    s_decimal = PyObject_GetAttrString(dec, "Decimal");
    Py_DECREF(dec);
    if (!s_decimal) return false;

    PyObject *dt = PyImport_ImportModule("datetime");
    if (!dt) return false;
    s_timezone  = PyObject_GetAttrString(dt, "timezone");
    s_timedelta = PyObject_GetAttrString(dt, "timedelta");
    Py_DECREF(dt);
    if (!s_timezone || !s_timedelta) return false;

    /* timestamptz arrives as UTC microseconds, so its tzinfo is the same object
     * for every row: build it once rather than per value. */
    PyObject *zero = PyObject_CallFunction(s_timedelta, "ii", 0, 0);
    if (!zero) return false;
    s_utc = PyObject_CallFunctionObjArgs(s_timezone, zero, nullptr);
    Py_DECREF(zero);
    if (!s_utc) return false;

    PyObject *ud = PyImport_ImportModule("uuid");
    if (!ud) return false;
    s_uuid = PyObject_GetAttrString(ud, "UUID");
    Py_DECREF(ud);
    return s_uuid != nullptr;
}

/* ---- the binary kinds ---------------------------------------------------- */

/* PostgreSQL counts dates in days and timestamps in microseconds, both from
 * 2000-01-01; the civil calendar needs the same shift for either. */
#define FP_EPOCH_DAYS 10957 /* 2000-01-01 in days since 1970-01-01 */
#define FP_US_PER_DAY 86400000000LL
#define FP_US_PER_SEC 1000000LL

static inline int32_t
load_le32(const uint8_t *p)
{
    int32_t v;

    memcpy(&v, p, 4);
    return v;
}

static inline int64_t
load_le64(const uint8_t *p)
{
    int64_t v;

    memcpy(&v, p, 8);
    return v;
}

/* Days since 1970-01-01 -> y/m/d (Hinnant's civil_from_days). Integer only: no
 * allocation, no library call, which is the whole point of the binary kinds. */
static void
civil_from_days(int64_t z, int *y, int *m, int *d)
{
    int64_t  era;
    unsigned doe, yoe, doy, mp;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned)(z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp  = (5 * doy + 2) / 153;
    *d  = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m  = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y  = (int)((int64_t)yoe + era * 400 + (*m <= 2));
}

static PyObject *
date_from_days(int32_t days)
{
    int y, m, d;

    civil_from_days((int64_t)days + FP_EPOCH_DAYS, &y, &m, &d);
    return PyDate_FromDate(y, m, d);
}

static PyObject *
time_from_us(int64_t us)
{
    return PyTime_FromTime((int)(us / 3600000000LL), (int)(us / 60000000LL % 60),
                           (int)(us / FP_US_PER_SEC % 60), (int)(us % FP_US_PER_SEC));
}

static PyObject *
timestamp_from_us(int64_t us, int with_tz)
{
    int64_t days = us / FP_US_PER_DAY;
    int64_t rem  = us % FP_US_PER_DAY;
    int     y, m, d;

    if (rem < 0) /* floor division: PostgreSQL keeps the remainder positive */
    {
        rem += FP_US_PER_DAY;
        days -= 1;
    }
    civil_from_days(days + FP_EPOCH_DAYS, &y, &m, &d);

    if (!with_tz)
        return PyDateTime_FromDateAndTime(y, m, d, (int)(rem / 3600000000LL), (int)(rem / 60000000LL % 60),
                                          (int)(rem / FP_US_PER_SEC % 60), (int)(rem % FP_US_PER_SEC));
    return PyDateTimeAPI->DateTime_FromDateAndTime(
        y, m, d, (int)(rem / 3600000000LL), (int)(rem / 60000000LL % 60), (int)(rem / FP_US_PER_SEC % 60),
        (int)(rem % FP_US_PER_SEC), s_utc, PyDateTimeAPI->DateTimeType);
}

static PyObject *
uuid_from_bytes(const uint8_t *p)
{
    PyObject *raw = PyBytes_FromStringAndSize((const char *)p, 16);
    PyObject *out;

    if (!raw) return nullptr;
    out = PyObject_CallFunction(s_uuid, "OO", Py_None, raw); /* UUID(hex=None, bytes=raw) */
    Py_DECREF(raw);
    return out;
}

static PyObject *
decimal_from_text(const char *s)
{
    PyObject *text = PyUnicode_FromString(s);
    if (!text) return nullptr;
    PyObject *value = PyObject_CallFunctionObjArgs(s_decimal, text, nullptr);
    Py_DECREF(text);
    return value;
}

static PyObject *
bytes_from_bytea(const char *s)
{
    if (s[0] == '\\' && s[1] == 'x')
    {
        size_t     hexlen = strlen(s + 2);
        Py_ssize_t n      = (Py_ssize_t)(hexlen / 2);
        PyObject  *out    = PyBytes_FromStringAndSize(nullptr, n);
        if (!out) return nullptr;
        char *dst = PyBytes_AS_STRING(out);
        for (Py_ssize_t i = 0; i < n; i++)
        {
            auto nib = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            dst[i] = (char)((nib(s[2 + i * 2]) << 4) | nib(s[2 + i * 2 + 1]));
        }
        return out;
    }
    return PyBytes_FromString(s);
}

/* "YYYY-MM-DD [HH:MM:SS[.ffffff]][±HH[:MM]]" -> datetime.date/datetime. */
static PyObject *
parse_datetime(const char *s, int with_time, int with_tz)
{
    const char *p = s;
    int         y = (int)strtol(p, (char **)&p, 10);
    p++; /* '-' */
    int mo = (int)strtol(p, (char **)&p, 10);
    p++; /* '-' */
    int d = (int)strtol(p, (char **)&p, 10);

    if (!with_time) return PyDate_FromDate(y, mo, d);

    p++; /* ' ' */
    int h = (int)strtol(p, (char **)&p, 10);
    p++; /* ':' */
    int mi = (int)strtol(p, (char **)&p, 10);
    p++; /* ':' */
    int sec = (int)strtol(p, (char **)&p, 10);

    int us = 0;
    if (*p == '.')
    {
        p++;
        int scale = 100000;
        while (isdigit((unsigned char)*p))
        {
            us += (*p - '0') * scale;
            scale /= 10;
            p++;
        }
    }

    if (!with_tz) return PyDateTime_FromDateAndTime(y, mo, d, h, mi, sec, us);

    int off = 0;
    if (*p == '+' || *p == '-')
    {
        int sign = (*p == '-') ? -1 : 1;
        p++;
        int oh = (int)strtol(p, (char **)&p, 10);
        int om = 0;
        if (*p == ':')
        {
            p++;
            om = (int)strtol(p, (char **)&p, 10);
        }
        off = sign * (oh * 3600 + om * 60);
    }
    PyObject *delta = PyObject_CallFunction(s_timedelta, "ii", 0, off);
    if (!delta) return nullptr;
    PyObject *tz = PyObject_CallFunctionObjArgs(s_timezone, delta, nullptr);
    Py_DECREF(delta);
    if (!tz) return nullptr;
    PyObject *out =
        PyDateTimeAPI->DateTime_FromDateAndTime(y, mo, d, h, mi, sec, us, tz, PyDateTimeAPI->DateTimeType);
    Py_DECREF(tz);
    return out;
}

static PyObject *
parse_time(const char *s)
{
    const char *p = s;
    int         h = (int)strtol(p, (char **)&p, 10);
    p++;
    int mi = (int)strtol(p, (char **)&p, 10);
    p++;
    int sec = (int)strtol(p, (char **)&p, 10);

    int us = 0;
    if (*p == '.')
    {
        p++;
        int scale = 100000;
        while (isdigit((unsigned char)*p))
        {
            us += (*p - '0') * scale;
            scale /= 10;
            p++;
        }
    }
    return PyTime_FromTime(h, mi, sec, us);
}

PyObject *
fp_decode_value(PGresult *res, int row, int col)
{
    if (PQgetisnull(res, row, col)) Py_RETURN_NONE;

    Oid         type = PQftype(res, col);
    const char *s    = PQgetvalue(res, row, col);

    switch (type)
    {
        case FASTPOSTGRES_OID_BOOL:
            if (s[0] == 't') Py_RETURN_TRUE;
            Py_RETURN_FALSE;
        case FASTPOSTGRES_OID_INT2:
        case FASTPOSTGRES_OID_INT4:
        case FASTPOSTGRES_OID_INT8:
        case FASTPOSTGRES_OID_OID:
            return PyLong_FromString(s, nullptr, 10);
        case FASTPOSTGRES_OID_FLOAT4:
        case FASTPOSTGRES_OID_FLOAT8:
            return PyFloat_FromDouble(strtod(s, nullptr));
        case FASTPOSTGRES_OID_NUMERIC:
        case FASTPOSTGRES_OID_MONEY:
            return decimal_from_text(s);
        case FASTPOSTGRES_OID_BYTEA:
            return bytes_from_bytea(s);
        case FASTPOSTGRES_OID_DATE:
            return parse_datetime(s, 0, 0);
        case FASTPOSTGRES_OID_TIME:
            return parse_time(s);
        case FASTPOSTGRES_OID_TIMESTAMP:
            return parse_datetime(s, 1, 0);
        case FASTPOSTGRES_OID_TIMESTAMPTZ:
            return parse_datetime(s, 1, 1);
        case FASTPOSTGRES_OID_UUID: /* the columnar path returns uuid.UUID; so must this one */
            return PyObject_CallFunction(s_uuid, "s", s);
        default:                    /* text family, json, xml, ... */
            return PyUnicode_FromString(s);
    }
}

/* ---- the per-chunk object pool (safe object reuse) ------------------------
 *
 * The immortal arena this replaces was worth ~2x on the object-birth step and
 * had to be append-only: an object whose storage belongs to the arena can never
 * be deallocated, so every read added resident pages and nothing gave them back
 * (~791 MiB after a 10M-row read). This is the same birth (write the CPython
 * header around a value written on the spot) with a real lifetime:
 *
 *   - values are born as a private *subtype* of float/int/str, whose tp_free
 *     returns the cell to a size-class free list. A subtype is what makes a
 *     differently-allocated object safe: the base type's deallocator calls
 *     Py_TYPE(self)->tp_free, so the free hook runs before anything assumes
 *     PyObject_Free. isinstance(x, float/int/str) stays true; type(x) is the
 *     private subtype, and the types are deliberately not instantiable.
 *   - cells come from one 512 MiB reservation, taken in 2 MiB chunks and reused
 *     from thread-local free lists afterwards. A second read of the same shape
 *     allocates no new pages; the footprint is the peak of concurrently-live
 *     values, not the sum of every read. A process-wide cap
 *     (FASTPOSTGRES_OBJ_ARENA=<bytes>, 0 turns it off) makes the worst case
 *     fixed: past it every value falls back to the normal constructor.
 *
 * The layouts are the part that can rot with a CPython release, so nothing is
 * hard-coded if the interpreter can tell us: the compact-ASCII header is copied
 * from a real string, the int tags from real longs, and fp_objpool_init()
 * proves the born values (repr, hash, compare, round trip) before the pool is
 * used. Anything unexpected leaves it off and the decoder uses the normal
 * constructors.
 */
#define FP_CELL_ALIGN ((size_t)16)
#define FP_CELL_MAX ((size_t)64 * 1024)
#define FP_CELL_BUCKETS (FP_CELL_MAX / FP_CELL_ALIGN)
#define FP_ARENA_BLOCK ((size_t)2 * 1024 * 1024)
#define FP_OBJPOOL_DEFAULT_BYTES ((size_t)512 * 1024 * 1024)

static PyTypeObject FpFloatType   = {PyVarObject_HEAD_INIT(nullptr, 0)};
static PyTypeObject FpLongType    = {PyVarObject_HEAD_INIT(nullptr, 0)};
static PyTypeObject FpUnicodeType = {PyVarObject_HEAD_INIT(nullptr, 0)};

/* copy/deepcopy/pickle should keep working: a born value reduces to a plain
 * exact float/int/str holding the same value, not to the private subtype (which
 * is deliberately not importable or instantiable). */
static PyObject *
fp_reduce_exact(PyObject *self, PyObject *type)
{
    PyObject *value = PyObject_CallFunctionObjArgs(type, self, nullptr);
    PyObject *args;
    PyObject *out;

    if (value == nullptr) return nullptr;
    args = PyTuple_Pack(1, value);
    Py_DECREF(value);
    if (args == nullptr) return nullptr;
    out = PyTuple_Pack(2, type, args);
    Py_DECREF(args);
    return out;
}

static PyObject *
fp_float_reduce(PyObject *self, PyObject *)
{
    return fp_reduce_exact(self, (PyObject *)&PyFloat_Type);
}

static PyObject *
fp_long_reduce(PyObject *self, PyObject *)
{
    return fp_reduce_exact(self, (PyObject *)&PyLong_Type);
}

static PyObject *
fp_unicode_reduce(PyObject *self, PyObject *)
{
    return fp_reduce_exact(self, (PyObject *)&PyUnicode_Type);
}

static PyMethodDef fp_float_methods[] = {
    {"__reduce__", fp_float_reduce, METH_NOARGS, nullptr},
    {"__reduce_ex__", fp_float_reduce, METH_VARARGS, nullptr},
    {nullptr},
};

static PyMethodDef fp_long_methods[] = {
    {"__reduce__", fp_long_reduce, METH_NOARGS, nullptr},
    {"__reduce_ex__", fp_long_reduce, METH_VARARGS, nullptr},
    {nullptr},
};

static PyMethodDef fp_unicode_methods[] = {
    {"__reduce__", fp_unicode_reduce, METH_NOARGS, nullptr},
    {"__reduce_ex__", fp_unicode_reduce, METH_VARARGS, nullptr},
    {nullptr},
};

static uintptr_t  g_long_tag[4][2]; /* [ndigits][negative] */
static char       g_str_hdr[sizeof(PyASCIIObject)];
static Py_ssize_t g_str_data = (Py_ssize_t)sizeof(PyASCIIObject);
static bool       g_pool_on;
static char      *g_pool_base;      /* one reservation; every cell lives in it */
static size_t     g_pool_cap;
static std::atomic<size_t> g_pool_used{0};

/* Per thread, not shared: the decoder runs on one thread and the free lists
 * only ever hold cells that thread may hand out again. Cells are carved from
 * the one process-wide reservation, so a short-lived thread cannot make the
 * footprint unbounded: its blocks stay inside g_pool_cap. */
struct CellPool {
    void  *free_head[FP_CELL_BUCKETS];
    char  *cur;
    size_t left;
    size_t births;
    size_t fallbacks;
};

static thread_local CellPool t_cells;

static inline size_t
cell_round(size_t n)
{
    return (n + FP_CELL_ALIGN - 1) & ~(FP_CELL_ALIGN - 1);
}

/* Take the next chunk of the reservation. The chunks are consecutive, so a
 * pointer is ours iff it lies inside [base, base+cap): tp_free needs exactly
 * that test to tell an arena cell from an instance some caller built with the
 * base type's own __new__ (which the allocator owns and must free normally). */
static bool
cell_block(size_t need)
{
    size_t want = FP_ARENA_BLOCK;
    size_t off;

    if (want < need) want = cell_round(need);

    off = g_pool_used.load(std::memory_order_relaxed);
    for (;;)
    {
        if (off + want > g_pool_cap) return false;
        if (g_pool_used.compare_exchange_weak(off, off + want, std::memory_order_relaxed)) break;
    }
    t_cells.cur  = g_pool_base + off;
    t_cells.left = want;
    return true;
}

static inline void *
cell_take(size_t n)
{
    size_t need = cell_round(n);
    size_t bucket;

    if (need == 0 || need > FP_CELL_MAX) return nullptr;
    bucket = need / FP_CELL_ALIGN - 1;
    if (t_cells.free_head[bucket] != nullptr)
    {
        void *p              = t_cells.free_head[bucket];
        t_cells.free_head[bucket] = *(void **)p;
        return p;
    }
    if (t_cells.left < need && !cell_block(need)) return nullptr;
    void *p = t_cells.cur;
    t_cells.cur += need;
    t_cells.left -= need;
    return p;
}

/* The size of a cell we born, from the object itself. Only our three private
 * types reach here (they are not instantiable and not exported). */
static inline size_t
cell_held(PyObject *op)
{
    PyTypeObject *tp = Py_TYPE(op);

    if (tp == &FpFloatType) return sizeof(PyFloatObject);
    if (tp == &FpLongType)
    {
        PyLongObject *l = (PyLongObject *)op;
        return offsetof(PyLongObject, long_value.ob_digit) +
               ((size_t)(l->long_value.lv_tag >> 3) * sizeof(digit));
    }
    if (tp == &FpUnicodeType) return sizeof(PyASCIIObject) + (size_t)((PyASCIIObject *)op)->length + 1;
    return 0;
}

static void
cell_free(void *v)
{
    PyObject *op = (PyObject *)v;
    size_t    n;

    /* A base-slot allocator or an embedding caller could still hand us an
     * instance the normal allocator built, even though the type itself is not
     * instantiable. Such an instance lives outside the reservation and belongs
     * to PyObject_Free; anything inside it is a cell of ours, so no per-object
     * header is needed. */
    uintptr_t p    = (uintptr_t)v;
    uintptr_t base = (uintptr_t)g_pool_base;
    if (p < base || p >= base + g_pool_cap)
    {
        PyObject_Free(v);
        return;
    }
    n = cell_held(op);
    if (n == 0 || cell_round(n) > FP_CELL_MAX)
    {
        PyObject_Free(v); /* not a shape we handed out: never free it as one */
        return;
    }
    size_t bucket = cell_round(n) / FP_CELL_ALIGN - 1;
    *(void **)v                = t_cells.free_head[bucket];
    t_cells.free_head[bucket]  = v;
}

static inline PyObject *
cell_birth(size_t size, PyTypeObject *tp)
{
    void *p = cell_take(size);

    if (p == nullptr) return nullptr;
    PyObject *o = (PyObject *)p;
    Py_SET_TYPE(o, tp);
    Py_SET_REFCNT(o, 1);
    return o;
}

static PyObject *
birth_float(double v)
{
    PyObject *o;

    if (!g_pool_on) return PyFloat_FromDouble(v);
    o = cell_birth(sizeof(PyFloatObject), &FpFloatType);

    if (o != nullptr)
    {
        ((PyFloatObject *)o)->ob_fval = v;
        t_cells.births++;
        return o;
    }
    t_cells.fallbacks++;
    return PyFloat_FromDouble(v);
}

static PyObject *
birth_long(long long v)
{
    uint64_t  mag;
    int       nd = 1, neg;
    PyObject *o;
    digit    *d;

    /* the interpreter's small-int cache is already immortal: nothing to gain */
    if (v >= -5 && v <= 256) return PyLong_FromLong((long)v);
    if (!g_pool_on) return PyLong_FromLongLong(v);
    neg = v < 0;
    mag = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    if (mag >> 30) nd = 2;
    if (mag >> 60) nd = 3;

    o = cell_birth(offsetof(PyLongObject, long_value.ob_digit) + (size_t)nd * sizeof(digit), &FpLongType);
    if (o == nullptr)
    {
        t_cells.fallbacks++;
        return PyLong_FromLongLong(v);
    }
    memcpy((char *)o + offsetof(PyLongObject, long_value.lv_tag), &g_long_tag[nd][neg], sizeof(uintptr_t));
    d = (digit *)((char *)o + offsetof(PyLongObject, long_value.ob_digit));
    for (int i = 0; i < nd; i++)
        d[i] = (digit)((mag >> (PyLong_SHIFT * i)) & (uint64_t)PyLong_MASK);
    t_cells.births++;
    return o;
}

static PyObject *
birth_text(const char *p, Py_ssize_t n)
{
    size_t     cell;
    PyObject  *o;
    Py_ssize_t i = 0;

    if (n <= 0) goto fallback; /* the empty string is the interpreter's singleton */
    if (!g_pool_on) goto fallback;

    /* the cell layout below is compact ASCII; every other form uses the real
     * constructor, which owns whatever storage that encoding needs */
    for (; i + 8 <= n; i += 8)
    {
        uint64_t w;
        memcpy(&w, p + i, 8);
        if (w & 0x8080808080808080ULL) goto fallback;
    }
    for (; i < n; i++)
        if (((unsigned char)p[i] & 0x80) != 0) goto fallback;

    cell = (size_t)n + (size_t)g_str_data + 1;
    o    = cell_birth(cell, &FpUnicodeType);
    if (o == nullptr) goto fallback;
    {
        char *c = (char *)o;
        memcpy(c, g_str_hdr, sizeof(PyASCIIObject));
        Py_SET_TYPE(o, &FpUnicodeType);
        Py_SET_REFCNT(o, 1);
        ((PyASCIIObject *)o)->length = n;
        ((PyASCIIObject *)o)->hash   = -1;
        memcpy(c + g_str_data, p, (size_t)n);
        c[g_str_data + n] = 0;
    }
    t_cells.births++;
    return o;

fallback:
    t_cells.fallbacks++;
    return PyUnicode_FromStringAndSize(p, n);
}

static bool
pool_templates(void)
{
    static const long long probe[4] = {0, 12345, ((long long)1 << 30) + 7, ((long long)1 << 60) + 7};
    PyObject              *ref;
    const char            *ascii = "abc";
    int                    nd, neg;

    ref = PyUnicode_FromStringAndSize(ascii, 3);
    if (ref == nullptr) return false;
    if (!PyUnicode_IS_COMPACT_ASCII(ref) ||
        (char *)PyUnicode_DATA(ref) - (char *)ref != (Py_ssize_t)sizeof(PyASCIIObject))
    {
        Py_DECREF(ref);
        return false;
    }
    g_str_data = (char *)PyUnicode_DATA(ref) - (char *)ref;
    memcpy(g_str_hdr, ref, sizeof(PyASCIIObject));
    Py_DECREF(ref);

    for (nd = 1; nd <= 3; nd++)
    {
        for (neg = 0; neg <= 1; neg++)
        {
            long long v = neg ? -probe[nd] : probe[nd];
            uintptr_t tag;

            ref = PyLong_FromLongLong(v);
            if (ref == nullptr) return false;
            memcpy(&tag, (const char *)ref + offsetof(PyLongObject, long_value.lv_tag), sizeof(tag));
            if ((tag >> 3) != (uintptr_t)nd)
            {
                Py_DECREF(ref);
                return false;
            }
            g_long_tag[nd][neg] = tag;
            Py_DECREF(ref);
        }
    }
    return true;
}

static bool
pool_verify(void)
{
    static const long long longs[] = {
        12345, -12345, ((long long)1 << 30) + 7, -(((long long)1 << 30) + 7), ((long long)1 << 60) + 7};
    PyObject *o;
    size_t    i;

    o = birth_float(1.2345);
    if (o == nullptr || !PyFloat_Check(o) || PyFloat_AsDouble(o) != 1.2345) return false;
    PyObject *fr = PyObject_Repr(o);
    if (fr == nullptr || PyUnicode_CompareWithASCIIString(fr, "1.2345") != 0)
    {
        Py_XDECREF(fr);
        return false;
    }
    Py_DECREF(fr);
    Py_DECREF(o);
    o = birth_float(1.2345); /* prove the returned cell is reusable */
    if (o == nullptr || PyFloat_AsDouble(o) != 1.2345) return false;
    Py_DECREF(o);

    for (i = 0; i < sizeof(longs) / sizeof(longs[0]); i++)
    {
        PyObject *ref = PyLong_FromLongLong(longs[i]);
        PyObject *cmp;

        o = birth_long(longs[i]);
        if (o == nullptr || ref == nullptr || !PyLong_Check(o)) return false;
        if (PyLong_AsLongLong(o) != longs[i]) return false;
        if (PyObject_RichCompareBool(o, ref, Py_EQ) != 1) return false;
        if (PyObject_Hash(o) != PyObject_Hash(ref)) return false;
        cmp = PyObject_Repr(o);
        if (cmp == nullptr) return false;
        Py_DECREF(cmp);
        Py_DECREF(ref);
        Py_DECREF(o);
    }

    o = birth_text("hello world", 11);
    if (o == nullptr || !PyUnicode_Check(o)) return false;
    if (PyUnicode_GET_LENGTH(o) != 11) return false;
    if (PyUnicode_CompareWithASCIIString(o, "hello world") != 0) return false;
    if (PyUnicode_AsUTF8AndSize(o, nullptr) == nullptr) return false;
    {
        PyObject *ref  = PyUnicode_FromString("hello world");
        int       same = ref != nullptr && PyObject_Hash(o) == PyObject_Hash(ref);
        Py_XDECREF(ref);
        if (!same) return false;
    }
    Py_DECREF(o);
    o = birth_text("hello world", 11);
    if (o == nullptr || PyUnicode_CompareWithASCIIString(o, "hello world") != 0) return false;
    Py_DECREF(o);
    return true;
}

bool
fp_objpool_init(void)
{
#ifdef Py_GIL_DISABLED
    /* the free-threaded build has a different object header and refcount
     * protocol; keep the normal constructors rather than guess at it */
    return false;
#else
    const char *env = getenv("FASTPOSTGRES_OBJ_ARENA");
    size_t      cap = FP_OBJPOOL_DEFAULT_BYTES;

    g_pool_on = false;
    if (env != nullptr && env[0] != 0)
    {
        unsigned long long want = strtoull(env, nullptr, 10);

        if (want == 0) return false;
        cap = (size_t)want;
    }
    cap &= ~(size_t)4095;
    if (cap < FP_ARENA_BLOCK) return false;

    FpFloatType.tp_name     = "_fastpostgres.born_float";
    FpFloatType.tp_base     = &PyFloat_Type;
    FpFloatType.tp_basicsize = sizeof(PyFloatObject);
    FpFloatType.tp_flags    = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION;
    FpFloatType.tp_free     = cell_free;
    FpFloatType.tp_methods  = fp_float_methods;

    FpLongType.tp_name      = "_fastpostgres.born_int";
    FpLongType.tp_base      = &PyLong_Type;
    FpLongType.tp_basicsize = sizeof(PyLongObject);
    FpLongType.tp_itemsize  = sizeof(digit);
    FpLongType.tp_flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION;
    FpLongType.tp_free      = cell_free;
    FpLongType.tp_methods   = fp_long_methods;

    FpUnicodeType.tp_name      = "_fastpostgres.born_str";
    FpUnicodeType.tp_base      = &PyUnicode_Type;
    FpUnicodeType.tp_basicsize = sizeof(PyUnicodeObject);
    FpUnicodeType.tp_flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION;
    FpUnicodeType.tp_free      = cell_free;
    FpUnicodeType.tp_methods   = fp_unicode_methods;

    if (PyType_Ready(&FpFloatType) < 0 || PyType_Ready(&FpLongType) < 0 ||
        PyType_Ready(&FpUnicodeType) < 0)
        return false;

    g_pool_base = (char *)mmap(nullptr, cap, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_pool_base == MAP_FAILED)
    {
        g_pool_base = nullptr;
        return false;
    }
#ifdef MADV_HUGEPAGE
    (void)madvise(g_pool_base, cap, MADV_HUGEPAGE);
#endif
    g_pool_cap  = cap;
    g_pool_used.store(0, std::memory_order_relaxed);
    g_pool_on   = true; /* the verification births need the real path */
    if (!pool_templates() || !pool_verify())
    {
        g_pool_on = false;
        t_cells.births = t_cells.fallbacks = 0;
        return false;
    }
    return true;
#endif
}

static const char *
fp_objpool_mode(void)
{
    if (!g_pool_on) return "off";
    return "reuse";
}

/* The row decode's cost in TSC cycles: two rdtsc per batch, free at this scale,
 * and the one number that says whether the loop is worth hand-writing (perf
 * cannot run in the container, so this stands in for it). Relaxed atomics: the
 * counters only need to not lose updates when decoder threads run in more than
 * one, and the report is a plain read of a done deal. */
static std::atomic<unsigned long long> g_dec_cycles{0};
static std::atomic<unsigned long long> g_dec_rows{0};
static std::atomic<unsigned long long> g_read_cycles{0};
static std::atomic<unsigned long long> g_read_rows{0};

/* One line per process, so a benchmark log says how the decode cost broke down. */
void
fp_stats_report(void)
{
    unsigned long long read_rows = g_read_rows.load(std::memory_order_relaxed);

    if (read_rows != 0)
    {
        unsigned long long read_cycles = g_read_cycles.load(std::memory_order_relaxed);
        unsigned long long dec_cycles  = g_dec_cycles.load(std::memory_order_relaxed);
        unsigned long long dec_rows    = g_dec_rows.load(std::memory_order_relaxed);

        fprintf(stderr, "read_rows: %.1f cycles/row over %llu rows (decode %.1f, frame+desc+list %.1f)\n",
                (double)read_cycles / (double)read_rows, read_rows,
                (double)dec_cycles / (double)(dec_rows ? dec_rows : 1),
                (double)(read_cycles - dec_cycles) / (double)read_rows);
    }
    if (g_pool_base != nullptr)
        fprintf(stderr, "objpool: %s cap=%zu MiB used=%zu KiB born=%zu fallbacks=%zu\n", fp_objpool_mode(),
                g_pool_cap >> 20, g_pool_used.load(std::memory_order_relaxed) >> 10, t_cells.births,
                t_cells.fallbacks);
}

/* ---- decoding a whole batch ----------------------------------------------
 *
 * A column's values share one kind, so the kind switch sits outside the row
 * loop: `row_column<K>` instantiates once per wire kind and the per-value
 * constructors inline into the loop body. These loops live here rather than in
 * segment.cpp so the compiler sees them whole.
 */

/* One value, no null test: the single-value path behind Column.value_at(). */
static PyObject *
decode_value(const FastpostgresColumn *c, uint64_t row)
{
    switch (c->kind)
    {
        case FASTPOSTGRES_KIND_BOOL:
            return PyBool_FromLong(((const uint8_t *)c->data)[row] != 0);
        case FASTPOSTGRES_KIND_INT16: {
            int16_t v;
            memcpy(&v, c->data + row * 2, 2);
            return birth_long(v);
        }
        case FASTPOSTGRES_KIND_INT32: {
            int32_t v;
            memcpy(&v, c->data + row * 4, 4);
            return birth_long(v);
        }
        case FASTPOSTGRES_KIND_INT64: {
            int64_t v;
            memcpy(&v, c->data + row * 8, 8);
            return birth_long(v);
        }
        case FASTPOSTGRES_KIND_FLOAT4: {
            float v;
            memcpy(&v, c->data + row * 4, 4);
            return birth_float((double)v);
        }
        case FASTPOSTGRES_KIND_FLOAT8: {
            double v;
            memcpy(&v, c->data + row * 8, 8);
            return birth_float(v);
        }
        case FASTPOSTGRES_KIND_DATE:
            return date_from_days(load_le32(c->data + row * 4));
        case FASTPOSTGRES_KIND_TIME:
            return time_from_us(load_le64(c->data + row * 8));
        case FASTPOSTGRES_KIND_TIMESTAMP:
            return timestamp_from_us(load_le64(c->data + row * 8), 0);
        case FASTPOSTGRES_KIND_TIMESTAMPTZ:
            return timestamp_from_us(load_le64(c->data + row * 8), 1);
        case FASTPOSTGRES_KIND_UUID:
            return uuid_from_bytes(c->data + row * 16);
        case FASTPOSTGRES_KIND_BYTEA: { /* the offset table, raw bytes out */
            const int32_t *off  = (const int32_t *)c->data;
            const char    *base = (const char *)c->bytes;
            int32_t        s0   = off[row];
            int32_t        s1   = off[row + 1];

            return PyBytes_FromStringAndSize(base + s0, (Py_ssize_t)(s1 - s0));
        }
        default: { /* text: int32 offset table, then the raw bytes */
            const int32_t *off  = (const int32_t *)c->data;
            const char    *base = (const char *)c->bytes;
            int32_t        s0   = off[row];
            int32_t        s1   = off[row + 1];

            return birth_text(base + s0, (Py_ssize_t)(s1 - s0));
        }
    }
}

void
fp_stats_add(unsigned long long cycles, unsigned long long rows)
{
    g_read_cycles.fetch_add(cycles, std::memory_order_relaxed);
    g_read_rows.fetch_add(rows, std::memory_order_relaxed);
}

PyObject *
fp_decode_column(const FastpostgresColumn *c, uint64_t row)
{
    if (fastpostgres_col_isnull(c, row)) Py_RETURN_NONE;
    return decode_value(c, row);
}

namespace
{

template <uint8_t K>
inline PyObject *
row_value(const FastpostgresColumn *c, uint64_t r)
{
    const uint8_t *data = c->data;

    if constexpr (K == FASTPOSTGRES_KIND_BOOL)
        return PyBool_FromLong(data[r] != 0);
    else if constexpr (K == FASTPOSTGRES_KIND_INT16)
    {
        int16_t v;
        memcpy(&v, data + r * 2, 2);
        return birth_long(v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_INT32)
    {
        int32_t v;
        memcpy(&v, data + r * 4, 4);
        return birth_long(v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_INT64)
    {
        int64_t v;
        memcpy(&v, data + r * 8, 8);
        return birth_long(v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_FLOAT4)
    {
        float v;
        memcpy(&v, data + r * 4, 4);
        return birth_float((double)v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_FLOAT8)
    {
        double v;
        memcpy(&v, data + r * 8, 8);
        return birth_float(v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_DATE)
        return date_from_days(load_le32(data + r * 4));
    else if constexpr (K == FASTPOSTGRES_KIND_TIME)
        return time_from_us(load_le64(data + r * 8));
    else if constexpr (K == FASTPOSTGRES_KIND_TIMESTAMP)
        return timestamp_from_us(load_le64(data + r * 8), 0);
    else if constexpr (K == FASTPOSTGRES_KIND_TIMESTAMPTZ)
        return timestamp_from_us(load_le64(data + r * 8), 1);
    else if constexpr (K == FASTPOSTGRES_KIND_UUID)
        return uuid_from_bytes(data + r * 16);
    else
    {
        const int32_t *off  = (const int32_t *)data;
        const char    *base = (const char *)c->bytes;
        int32_t        s0   = off[r];
        int32_t        s1   = off[r + 1];

        if constexpr (K == FASTPOSTGRES_KIND_BYTEA)
            return PyBytes_FromStringAndSize(base + s0, (Py_ssize_t)(s1 - s0));
        else
            return birth_text(base + s0, (Py_ssize_t)(s1 - s0));
    }
}

template <uint8_t K>
inline bool
row_column(PyObject *rows, uint64_t start, uint64_t nrows, const FastpostgresColumn *c, uint32_t cidx,
           bool nullfree)
{
    for (uint64_t i = 0; i < nrows; i++)
    {
        uint64_t  r   = start + i;
        PyObject *row = PyList_GET_ITEM(rows, (Py_ssize_t)i);
        PyObject *v;

        if (!nullfree && fastpostgres_col_isnull(c, r))
        {
            v = Py_None;
            Py_INCREF(v);
        }
        else
        {
            v = row_value<K>(c, r);
            if (v == nullptr) return false;
        }
        PyTuple_SET_ITEM(row, (Py_ssize_t)cidx, v);
    }
    return true;
}

} /* namespace */

bool
fp_decode_columns_range(const FastpostgresColumn *cols, uint32_t ncols, uint64_t start, uint64_t nrows,
                        PyObject *rows)
{
    for (uint32_t c = 0; c < ncols; c++)
    {
        /* The null map is a per-column property of the range, not of the whole
         * frame: test the bytes the window actually reads. */
        bool nullfree;
        bool ok;

        if (nrows == 0)
            nullfree = true;
        else
        {
            size_t first = (size_t)(start >> 3);
            size_t last  = (size_t)((start + nrows - 1) >> 3);

            nullfree = cols[c].nulls == nullptr ||
                       !fastpostgres_nullmap_any(cols[c].nulls + first, last - first + 1);
        }

        switch (cols[c].kind)
        {
            case FASTPOSTGRES_KIND_BOOL:
                ok = row_column<FASTPOSTGRES_KIND_BOOL>(rows, start, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_INT16:
                ok = row_column<FASTPOSTGRES_KIND_INT16>(rows, start, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_INT32:
                ok = row_column<FASTPOSTGRES_KIND_INT32>(rows, start, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_INT64:
                ok = row_column<FASTPOSTGRES_KIND_INT64>(rows, start, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_FLOAT4:
                ok = row_column<FASTPOSTGRES_KIND_FLOAT4>(rows, start, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_FLOAT8:
                ok = row_column<FASTPOSTGRES_KIND_FLOAT8>(rows, start, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_DATE:
                ok = row_column<FASTPOSTGRES_KIND_DATE>(rows, start, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_TIME:
                ok = row_column<FASTPOSTGRES_KIND_TIME>(rows, start, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_TIMESTAMP:
                ok = row_column<FASTPOSTGRES_KIND_TIMESTAMP>(rows, start, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_TIMESTAMPTZ:
                ok = row_column<FASTPOSTGRES_KIND_TIMESTAMPTZ>(rows, start, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_UUID:
                ok = row_column<FASTPOSTGRES_KIND_UUID>(rows, start, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_BYTEA:
                ok = row_column<FASTPOSTGRES_KIND_BYTEA>(rows, start, nrows, &cols[c], c, nullfree);
                break;
            default:
                ok = row_column<FASTPOSTGRES_KIND_TEXT>(rows, start, nrows, &cols[c], c, nullfree);
                break;
        }
        if (!ok) return false;
    }
    return true;
}

bool
fp_decode_columns(const FastpostgresColumn *cols, uint32_t ncols, uint64_t nrows, PyObject *rows)
{
    return fp_decode_columns_range(cols, ncols, 0, nrows, rows);
}

/* ---- decoding a row frame -------------------------------------------------
 *
 * The server sends PostgreSQL's own tuple bytes (rows.c), so the walk here
 * mirrors heap_deform_tuple: the offset advances by each attribute's alignment
 * and length, a null attribute takes no space at all, and the null bitmap sits
 * at t_bits rather than at the end of the header area.
 *
 * The per-column layout (and the null bit's place in the tuple) is worked out
 * once per batch, and the kind is dispatched once per column per *block* of
 * rows: a block is walked column by column, each column through the one body
 * specialised for its kind, so no value pays for a switch — the columnar path's
 * row_column<K>, over a tuple walk. A block also leaves the walk with one
 * independent offset chain per row instead of one per row's values, and keeps
 * the frame bytes it touches in L1: a pass spanning the whole batch would come
 * back to a row's bytes only after the rest of the batch had gone by.
 */

namespace
{

/* Rows one kind dispatch covers: enough for the switch not to be per value,
 * small enough that a block's pointers, offsets and tuples stay in registers. */
#define FP_ROWS_BLOCK 16

/* No attribute offset is a property of the schema here. */
#define FP_NO_OFF UINT32_MAX

struct AttLayout {
    int32_t  attlen;   /* -1 for the varlena kinds */
    uint32_t alignby;
    uint32_t nullbyte; /* the tuple byte holding this column's null bit */
    uint8_t  nullbit;
    bool     varlena;
};

/* One block being decoded: the rows, the attribute area of each (t_hoff is
 * MAXALIGNed, so offsets relative to it align as heap_deform_tuple's do),
 * where each row's walk has got to, whether it carries a null bitmap, and the
 * tuple its values are landing in. */
struct RowBlock {
    const uint8_t *row[FP_ROWS_BLOCK];
    const uint8_t *base[FP_ROWS_BLOCK]; /* row + t_hoff: offsets are relative to it */
    PyObject      *tup[FP_ROWS_BLOCK];
    uint32_t       off[FP_ROWS_BLOCK];
    uint8_t        hasnull[FP_ROWS_BLOCK];
    uint32_t       n;
};

/* A varlena header decoded once: the value's total size (header included) and
 * its data length. The old body asked for the data pointer and the length
 * separately and then decoded the same header a third time to advance the
 * offset; that is two dependent loads on the walk's critical path per value.
 *
 * The header form is data-dependent on a mixed batch (short values use one byte,
 * long ones four), so the choice is a cmov rather than a branch: the shape the
 * mixed meter prices at ~3 conditional mispredicts a row. Reading four bytes
 * from a one-byte header is safe here - the row is padded to MAXALIGN and the
 * value is never the last mapped byte. */
static inline uint32_t
fp_varatt_size(const uint8_t *p, uint32_t *len)
{
#if FASTPOSTGRES_BIG_ENDIAN
    uint32_t one = (uint32_t)(p[0] >> 7);
    uint32_t s1  = (uint32_t)(p[0] & 0x7Fu);
    uint32_t s4  = fastpostgres_peek_u32(p) & 0x3FFFFFFFu;
#else
    uint32_t one = (uint32_t)(p[0] & 1u);
    uint32_t s1  = (uint32_t)(p[0] >> 1);
    uint32_t s4  = fastpostgres_peek_u32(p) >> 2;
#endif
    uint32_t mask = (uint32_t)0 - one; /* all ones for the one-byte form */
    uint32_t size = (s4 & ~mask) | (s1 & mask);
    uint32_t hdr  = 4u - 3u * one;

    *len = size - hdr;
    return size;
}

/* att_align_pointer's choice without the branch: a value whose first byte is
 * zero is aligned, anything else is taken where it lies. */
static inline uint32_t
fp_att_pointer_align(uint32_t off, uint32_t alignby, const uint8_t *ptr)
{
    uint32_t aligned = (off + alignby - 1) & ~(alignby - 1);
    uint32_t mask    = (uint32_t)0 - (uint32_t)(*ptr == 0);

    return off + ((aligned - off) & mask);
}

template <uint8_t K>
inline PyObject *
tuple_fixed(const uint8_t *p)
{
    static_assert(K != FASTPOSTGRES_KIND_TEXT && K != FASTPOSTGRES_KIND_BYTEA,
                  "varlena kinds are decoded by tuple_varlena()");

    if constexpr (K == FASTPOSTGRES_KIND_BOOL)
        return PyBool_FromLong(*p != 0);
    else if constexpr (K == FASTPOSTGRES_KIND_INT16)
    {
        int16_t v;
        memcpy(&v, p, 2);
        return birth_long(v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_INT32)
    {
        int32_t v;
        memcpy(&v, p, 4);
        return birth_long(v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_INT64)
    {
        int64_t v;
        memcpy(&v, p, 8);
        return birth_long(v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_FLOAT4)
    {
        float v;
        memcpy(&v, p, 4);
        return birth_float((double)v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_FLOAT8)
    {
        double v;
        memcpy(&v, p, 8);
        return birth_float(v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_DATE)
        return date_from_days(load_le32(p));
    else if constexpr (K == FASTPOSTGRES_KIND_TIME)
        return time_from_us(load_le64(p));
    else if constexpr (K == FASTPOSTGRES_KIND_TIMESTAMP)
        return timestamp_from_us(load_le64(p), 0);
    else if constexpr (K == FASTPOSTGRES_KIND_TIMESTAMPTZ)
        return timestamp_from_us(load_le64(p), 1);
    else
        return uuid_from_bytes(p);
}

template <uint8_t K>
inline PyObject *
tuple_varlena(const uint8_t *data, uint32_t len)
{
    if constexpr (K == FASTPOSTGRES_KIND_BYTEA)
        return PyBytes_FromStringAndSize((const char *)data, (Py_ssize_t)len);
    else
        return birth_text((const char *)data, (Py_ssize_t)len);
}

/* One column of a block, in row order, for a fixed-width kind. STATIC_OFF says
 * the batch's layout made this attribute's offset constant, so the value is
 * read where it lies with no align/addlength chain; NULLFREE says no row in the
 * block has a null here, so the body carries no null test either. */
template <uint8_t K, bool NULLFREE, bool STATIC_OFF>
inline bool
block_fixed_loop(RowBlock &b, const AttLayout &layout, uint32_t cidx, uint32_t fixed_off)
{
    for (uint32_t k = 0; k < b.n; k++)
    {
        const uint8_t *row = b.row[k];

        if constexpr (!NULLFREE)
        {
            if (b.hasnull[k] && (row[layout.nullbyte] & layout.nullbit) == 0)
            {
                PyObject *none = Py_None;
                Py_INCREF(none); /* a null stores nothing: the offset does not move */
                PyTuple_SET_ITEM(b.tup[k], (Py_ssize_t)cidx, none);
                continue;
            }
        }

        uint32_t o;

        if constexpr (STATIC_OFF)
            o = fixed_off;
        else
            o = fastpostgres_att_align(b.off[k], layout.alignby);

        PyObject *v = tuple_fixed<K>(b.base[k] + o);
        if (v == nullptr) return false;
        b.off[k] = o + (uint32_t)layout.attlen;
        PyTuple_SET_ITEM(b.tup[k], (Py_ssize_t)cidx, v);
    }
    return true;
}

/* The same walk for a varlena kind. The header is decoded once and its total
 * size advances the offset, so the value and the next attribute share one load.
 * A schema-static offset is only used when that offset is already aligned: when
 * it is not, att_align_pointer's answer depends on the value's own first byte. */
template <uint8_t K, bool NULLFREE, bool STATIC_OFF>
inline bool
block_varlena_loop(RowBlock &b, const AttLayout &layout, uint32_t cidx, uint32_t fixed_off)
{
    for (uint32_t k = 0; k < b.n; k++)
    {
        const uint8_t *row = b.row[k];

        if constexpr (!NULLFREE)
        {
            if (b.hasnull[k] && (row[layout.nullbyte] & layout.nullbit) == 0)
            {
                PyObject *none = Py_None;
                Py_INCREF(none);
                PyTuple_SET_ITEM(b.tup[k], (Py_ssize_t)cidx, none);
                continue;
            }
        }

        uint32_t o;

        if constexpr (STATIC_OFF)
            o = fixed_off;
        else
            o = fp_att_pointer_align(b.off[k], layout.alignby, b.base[k] + b.off[k]);

        const uint8_t *p    = b.base[k] + o;
        uint32_t       len  = 0;
        uint32_t       size = fp_varatt_size(p, &len);
        PyObject      *v    = tuple_varlena<K>(p + (size - len), len);

        if (v == nullptr) return false;
        b.off[k] = o + size;
        PyTuple_SET_ITEM(b.tup[k], (Py_ssize_t)cidx, v);
    }
    return true;
}

/* One column of a block. K settles the kind at compile time; the layout and the
 * block's null map settle the rest before the row loop starts. */
template <uint8_t K>
inline bool
block_column(RowBlock &b, const AttLayout &layout, uint32_t cidx, uint32_t fixed_off, bool nullfree)
{
    constexpr bool VARLENA = (K == FASTPOSTGRES_KIND_TEXT || K == FASTPOSTGRES_KIND_BYTEA);
    bool           fixed   = fixed_off != FP_NO_OFF;

    if constexpr (VARLENA)
    {
        if (fixed)
        {
            if (nullfree) return block_varlena_loop<K, true, true>(b, layout, cidx, fixed_off);
            return block_varlena_loop<K, false, true>(b, layout, cidx, fixed_off);
        }
        if (nullfree) return block_varlena_loop<K, true, false>(b, layout, cidx, 0);
        return block_varlena_loop<K, false, false>(b, layout, cidx, 0);
    }
    else
    {
        if (fixed)
        {
            if (nullfree) return block_fixed_loop<K, true, true>(b, layout, cidx, fixed_off);
            return block_fixed_loop<K, false, true>(b, layout, cidx, fixed_off);
        }
        if (nullfree) return block_fixed_loop<K, true, false>(b, layout, cidx, 0);
        return block_fixed_loop<K, false, false>(b, layout, cidx, 0);
    }
}

/* One block: the switch runs once per column, and each column's body then walks
 * every row of the block without it. The per-block layout is resolved here,
 * after the block's null bytes are known: a fixed-width attribute whose
 * predecessors are all fixed-width and null-free in this block has one offset,
 * and a column with no null bit anywhere in the block needs no null test. */
inline bool
decode_block(const FastpostgresColumn *cols, uint32_t ncols, const AttLayout *layout, uint32_t *fixed,
             const uint8_t *nonnull, RowBlock &b)
{
    uint32_t off = 0;
    bool     dyn = false;

    for (uint32_t c = 0; c < ncols; c++)
    {
        bool anynull = (nonnull[c >> 3] & (uint8_t)(1u << (c & 7))) == 0;

        if (dyn)
            fixed[c] = FP_NO_OFF;
        else if (layout[c].varlena)
        {
            if ((off & (layout[c].alignby - 1)) != 0)
            {
                /* att_align_pointer reads the value's own header to choose */
                fixed[c] = FP_NO_OFF;
                dyn      = true;
            }
            else
                fixed[c] = off;
        }
        else
        {
            off      = fastpostgres_att_align(off, layout[c].alignby);
            fixed[c] = off;
        }

        if (layout[c].varlena || anynull)
            dyn = true;
        else
            off += (uint32_t)layout[c].attlen;
    }

    for (uint32_t c = 0; c < ncols; c++)
    {
        bool     nullfree = (nonnull[c >> 3] & (uint8_t)(1u << (c & 7))) != 0;
        uint32_t f        = fixed[c];
        bool     ok;

        switch (cols[c].kind)
        {
            case FASTPOSTGRES_KIND_BOOL:
                ok = block_column<FASTPOSTGRES_KIND_BOOL>(b, layout[c], c, f, nullfree);
                break;
            case FASTPOSTGRES_KIND_INT16:
                ok = block_column<FASTPOSTGRES_KIND_INT16>(b, layout[c], c, f, nullfree);
                break;
            case FASTPOSTGRES_KIND_INT32:
                ok = block_column<FASTPOSTGRES_KIND_INT32>(b, layout[c], c, f, nullfree);
                break;
            case FASTPOSTGRES_KIND_INT64:
                ok = block_column<FASTPOSTGRES_KIND_INT64>(b, layout[c], c, f, nullfree);
                break;
            case FASTPOSTGRES_KIND_FLOAT4:
                ok = block_column<FASTPOSTGRES_KIND_FLOAT4>(b, layout[c], c, f, nullfree);
                break;
            case FASTPOSTGRES_KIND_FLOAT8:
                ok = block_column<FASTPOSTGRES_KIND_FLOAT8>(b, layout[c], c, f, nullfree);
                break;
            case FASTPOSTGRES_KIND_DATE:
                ok = block_column<FASTPOSTGRES_KIND_DATE>(b, layout[c], c, f, nullfree);
                break;
            case FASTPOSTGRES_KIND_TIME:
                ok = block_column<FASTPOSTGRES_KIND_TIME>(b, layout[c], c, f, nullfree);
                break;
            case FASTPOSTGRES_KIND_TIMESTAMP:
                ok = block_column<FASTPOSTGRES_KIND_TIMESTAMP>(b, layout[c], c, f, nullfree);
                break;
            case FASTPOSTGRES_KIND_TIMESTAMPTZ:
                ok = block_column<FASTPOSTGRES_KIND_TIMESTAMPTZ>(b, layout[c], c, f, nullfree);
                break;
            case FASTPOSTGRES_KIND_UUID:
                ok = block_column<FASTPOSTGRES_KIND_UUID>(b, layout[c], c, f, nullfree);
                break;
            case FASTPOSTGRES_KIND_BYTEA:
                ok = block_column<FASTPOSTGRES_KIND_BYTEA>(b, layout[c], c, f, nullfree);
                break;
            default:
                ok = block_column<FASTPOSTGRES_KIND_TEXT>(b, layout[c], c, f, nullfree);
                break;
        }
        if (!ok) return false;
    }
    return true;
}

} /* namespace */

bool
fp_decode_tuples(const FastpostgresColumn *cols, uint32_t ncols, FastpostgresRowsReader *rd, PyObject *rows)
{
    std::vector<AttLayout> layout(ncols);
    std::vector<uint32_t>  fixed(ncols);
    std::vector<uint8_t>   nonnull((size_t)((ncols + 7) / 8)); /* one bit-byte per 8 columns */
    RowBlock               block;
    const uint8_t         *row;
    uint32_t               len;
    uint64_t               expect   = rd->left;
    uint64_t               r        = 0;
    uint32_t               nulbytes = (ncols + 7) / 8;

    for (uint32_t c = 0; c < ncols; c++)
    {
        int width = fastpostgres_kind_width(cols[c].kind);

        if (width == 0 && !fastpostgres_kind_is_varlena(cols[c].kind))
        {
            PyErr_SetString(PyExc_ValueError, "row frame: unknown wire kind");
            return false;
        }
        layout[c].attlen   = width > 0 ? width : -1;
        layout[c].alignby  = (uint32_t)fastpostgres_kind_alignby(cols[c].kind);
        layout[c].nullbyte = FASTPOSTGRES_HT_BITS + (c >> 3);
        layout[c].nullbit  = (uint8_t)(1u << (c & 7));
        layout[c].varlena  = fastpostgres_kind_is_varlena(cols[c].kind) != 0;
    }

    unsigned long long t0 = __rdtsc();

    for (;;)
    {
        /* The frame checks are made where the tuple is made, once per row; the
         * values are decoded after the block is complete, so each tuple goes
         * into the caller's list before it is filled. A half-filled tuple is
         * only reachable through that list, and both the GC and the deallocator
         * walk its items null-safely. */
        /* The block's non-null bits: heap_fill_tuple sets a bit for every
         * attribute that is NOT null, so ANDing over the rows that carry a
         * bitmap leaves a set bit exactly when no row in the block has a null
         * here. Such a column gets a body with no per-value null test. A tuple
         * without a bitmap has no nulls, so it contributes nothing. */
        memset(nonnull.data(), 0xFF, (size_t)nulbytes);
        block.n = 0;
        while (block.n < FP_ROWS_BLOCK && fastpostgres_rows_next(rd, &row, &len))
        {
            PyObject *tup = PyTuple_New((Py_ssize_t)ncols);
            uint32_t  hoff;
            uint8_t   hasnull;

            if (tup == nullptr) return false;
            hoff = fastpostgres_tuple_hoff(row);
            if (hoff < FASTPOSTGRES_HT_BITS || hoff > len)
            {
                Py_DECREF(tup);
                PyErr_SetString(PyExc_ValueError, "row tuple: bad header offset");
                return false;
            }
            /* the writer expands short tuples before sending them, so anything
             * else is a frame we do not understand */
            if (fastpostgres_tuple_natts(row) < ncols)
            {
                Py_DECREF(tup);
                PyErr_SetString(PyExc_ValueError, "row tuple: fewer attributes than columns");
                return false;
            }
            hasnull = (uint8_t)(fastpostgres_tuple_hasnull(row) != 0);
            if (hasnull)
            {
                for (uint32_t j = 0; j < nulbytes; j++)
                    nonnull[j] &= row[FASTPOSTGRES_HT_BITS + j];
            }
            block.row[block.n]     = row;
            block.base[block.n]    = row + hoff;
            block.off[block.n]     = 0;
            block.hasnull[block.n] = hasnull;
            block.tup[block.n]     = tup;
            PyList_SET_ITEM(rows, (Py_ssize_t)(r + block.n), tup);
            block.n++;
        }
        if (block.n == 0) break;

        if (!decode_block(cols, ncols, layout.data(), fixed.data(), nonnull.data(), block)) return false;
        r += block.n;
    }
    g_dec_cycles.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
    g_dec_rows.fetch_add(r, std::memory_order_relaxed);

    if (r != expect)
    {
        /* a short frame would leave NULL slots in the caller's list */
        PyErr_SetString(PyExc_ValueError, "truncated row payload");
        return false;
    }
    return true;
}
