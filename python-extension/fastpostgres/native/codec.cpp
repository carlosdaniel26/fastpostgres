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

#include <sys/mman.h>
#include <unistd.h>

#include <datetime.h>

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

/* ---- the immortal object pool (H11) ---------------------------------------
 *
 * Values born *in place*: instead of allocate + init + copy, a cell is carved
 * out of one arena, the CPython header is written around a value written on the
 * spot, and the object is handed out. It is measured at 89.9 ns/row -> 20.5
 * ns/row on the benchmark's row shape (the rest of the work is unchanged).
 *
 * An object built this way must never be freed, because its storage belongs to
 * the arena: the refcount is set to this build's immortal sentinel, the same
 * one the interpreter uses for None and the small ints, so Py_INCREF and
 * Py_DECREF return early and the deallocator is unreachable. For the same
 * reason the arena is append-only — a cell can never be recycled under a live
 * reference, so the memory is never returned. That is the whole price, and it
 * is a policy: FASTPOSTGRES_OBJ_ARENA=<bytes> sizes it, 0 turns it off, and
 * unset takes half of the cgroup limit (8 GiB at most).
 *
 * Every layout fact here can rot with a CPython release, so none of it is
 * assumed: the string header is *copied* from a compact-ASCII string the
 * interpreter built, the int tags are copied from longs the interpreter built,
 * and fp_objpool_init() proves the born objects behave (repr, hash, compare,
 * round-trip) and that decrefs do not move the refcount. Anything unexpected
 * leaves the pool off and every value falls back to its normal constructor.
 */
#define FP_OBJPOOL_DEFAULT_BYTES ((size_t)512 * 1024 * 1024)

static char      *g_pool_base;
static size_t     g_pool_cap;
static size_t     g_pool_used;
static bool       g_pool_on;
static bool       g_pool_exhausted;
static Py_ssize_t g_pool_immortal;
static size_t     g_pool_births;
static size_t     g_pool_fallbacks;
static uintptr_t  g_pool_long_tag[4][2]; /* [ndigits][negative] */
static char       g_pool_str_hdr[sizeof(PyASCIIObject)];
static Py_ssize_t g_pool_str_data = (Py_ssize_t)sizeof(PyASCIIObject);

static inline void *
pool_take(size_t n)
{
    char *p;

    n = (n + 15) & ~(size_t)15;
    if (!g_pool_on) return nullptr;
    if (g_pool_used + n > g_pool_cap)
    {
        g_pool_exhausted = true; /* once full, everything falls back for good */
        g_pool_on        = false;
        return nullptr;
    }
    p = g_pool_base + g_pool_used;
    g_pool_used += n;
    return p;
}

static inline PyObject *
cell_new(size_t size, PyTypeObject *tp)
{
    char     *cell = (char *)pool_take(size);
    PyObject *o;

    if (cell == nullptr) return nullptr;
    o = (PyObject *)cell;
    /* written directly: Py_SET_TYPE/Py_SET_REFCNT skip an object that already
     * looks immortal, and a zeroed cell does not — but the header of a cell is
     * not something to leave to what the allocator happened to hand back */
    o->ob_type   = tp;
    o->ob_refcnt = g_pool_immortal;
    g_pool_births++;
    return o;
}

static bool pool_verify(void);

/* This build's immortal marker, and the proof that it works: only a value the
 * interpreter recognizes as immortal keeps the refcount still, and that is the
 * whole safety argument for handing out arena memory as an object.
 *
 * The header's own constant is tried first, but it is not enough on every
 * version: 3.13 decides immortality in Py_INCREF with a threshold stricter than
 * Py_DECREF's sign test, so a value the header calls the sentinel can still be
 * incremented (measured: 0xC0000000 becomes 0xC0000001 on 3.13.5, and the pool
 * disabled itself). The interpreter's own immortal objects carry the encoding
 * it actually recognizes, so None is the fallback — and the loser of the two is
 * caught by the decref proof rather than by a version table. */
static bool
pool_sentinel(void)
{
    Py_ssize_t candidates[2];
    int        i;

#ifdef _Py_IMMORTAL_INITIAL_REFCNT
    candidates[0] = (Py_ssize_t)_Py_IMMORTAL_INITIAL_REFCNT;
#else
    candidates[0] = (Py_ssize_t)(3ULL << 30);
#endif
    candidates[1] = Py_REFCNT(Py_None);

    for (i = 0; i < 2; i++)
    {
        Py_ssize_t s = candidates[i];

        if (i == 1 && s == candidates[0]) break; /* the same value twice: done */
        /* even if the build does not call it immortal, it has to be far out of
         * decref range or the object would eventually be freed */
        if (s == 0 || s == 1) continue;
        if (!(s < 0 || (uint64_t)s >= (1ULL << 31))) continue;
        g_pool_immortal = s;
        if (pool_verify()) return true;
    }
    return false;
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
    /* the compact-ASCII form is the only one we write by hand; a non-compact
     * build (or a different data offset) means the shortcut is not available */
    if (!PyUnicode_IS_COMPACT_ASCII(ref) ||
        (char *)PyUnicode_DATA(ref) - (char *)ref != (Py_ssize_t)sizeof(PyASCIIObject))
    {
        Py_DECREF(ref);
        return false;
    }
    g_pool_str_data = (char *)PyUnicode_DATA(ref) - (char *)ref;
    memcpy(g_pool_str_hdr, ref, sizeof(PyASCIIObject)); /* the state word comes from the interpreter */
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
            /* the digit count lives in the top bits of the tag; if that is not
             * this build's encoding, the template is not ours to write */
            if ((tag >> 3) != (uintptr_t)nd)
            {
                Py_DECREF(ref);
                return false;
            }
            g_pool_long_tag[nd][neg] = tag;
            Py_DECREF(ref);
        }
    }
    return true;
}

/* Break one of each kind and check it behaves, then break the refcount rule:
 * decrefs must not move the count, or the object is one stray reference away
 * from being handed to the allocator it did not come from. */
static bool
pool_verify(void)
{
    static const long long longs[] = {
        12345,    -12345, ((long long)1 << 30) + 7, -(((long long)1 << 30) + 7), ((long long)1 << 60) + 7,
        1LL << 62};
    PyObject *o;
    size_t    i;

    o = fp_birth_float(1.2345);
    if (o == nullptr || !PyFloat_CheckExact(o)) return false;
    if (PyFloat_AsDouble(o) != 1.2345) return false;
    if (Py_REFCNT(o) != g_pool_immortal) return false;
    Py_INCREF(o);
    Py_DECREF(o);
    Py_DECREF(o);
    if (Py_REFCNT(o) != g_pool_immortal || PyFloat_AsDouble(o) != 1.2345) return false;

    for (i = 0; i < sizeof(longs) / sizeof(longs[0]); i++)
    {
        PyObject *ref = PyLong_FromLongLong(longs[i]);
        PyObject *cmp;

        o = fp_birth_long(longs[i]);
        if (o == nullptr || ref == nullptr || !PyLong_CheckExact(o)) return false;
        if (PyLong_AsLongLong(o) != longs[i]) return false;
        if (PyObject_RichCompareBool(o, ref, Py_EQ) != 1) return false;
        if (PyObject_Hash(o) != PyObject_Hash(ref)) return false; /* exercises the digits */
        cmp = PyObject_Repr(o);                                   /* the decimal conversion reads them too */
        if (cmp == nullptr) return false;
        Py_DECREF(cmp);
        Py_DECREF(ref);
        Py_INCREF(o);
        Py_DECREF(o);
        Py_DECREF(o);
        if (Py_REFCNT(o) != g_pool_immortal) return false;
    }

    o = fp_birth_text("hello world", 11);
    if (o == nullptr || !PyUnicode_CheckExact(o)) return false;
    if (PyUnicode_GET_LENGTH(o) != 11) return false;
    if (PyUnicode_CompareWithASCIIString(o, "hello world") != 0) return false;
    if (PyUnicode_AsUTF8AndSize(o, nullptr) == nullptr) return false;
    {
        PyObject *ref  = PyUnicode_FromString("hello world");
        int       same = ref != nullptr && PyObject_Hash(o) == PyObject_Hash(ref);
        Py_XDECREF(ref);
        if (!same) return false;
    }
    Py_INCREF(o);
    Py_DECREF(o);
    Py_DECREF(o);
    if (Py_REFCNT(o) != g_pool_immortal) return false;
    if (PyUnicode_CompareWithASCIIString(o, "hello world") != 0) return false;
    return true;
}

/* The pool's pages are never returned — an immortal object cannot be recycled —
 * so its cap has to be a fraction of what the process may actually use: the
 * cgroup limit when there is one, physical memory otherwise. Half of it, and no
 * more than 8 GiB, so a big host does not turn one read into a memory test. */
static size_t
pool_default_bytes(void)
{
    unsigned long long limit = 0;
    char               buf[64];
    FILE              *f;

    if ((f = fopen("/sys/fs/cgroup/memory.max", "r")) != nullptr) /* cgroup v2 */
    {
        if (fgets(buf, sizeof(buf), f) != nullptr && buf[0] != 'm') limit = strtoull(buf, nullptr, 10);
        fclose(f);
    }
    if (limit == 0 && (f = fopen("/sys/fs/cgroup/memory/memory.limit_in_bytes", "r")) != nullptr) /* v1 */
    {
        if (fgets(buf, sizeof(buf), f) != nullptr) limit = strtoull(buf, nullptr, 10);
        fclose(f);
    }
    if (limit == 0 || limit > (1ULL << 60)) /* "max", or the v1 "no limit" sentinel */
    {
        long pages = sysconf(_SC_PHYS_PAGES);
        long page  = sysconf(_SC_PAGESIZE);

        if (pages > 0 && page > 0) limit = (unsigned long long)pages * (unsigned long long)page;
    }
    if (limit == 0) return FP_OBJPOOL_DEFAULT_BYTES;

    limit /= 2;
    if (limit > (8ULL << 30)) limit = 8ULL << 30;
    if (limit < (16ULL << 20)) limit = 16ULL << 20;
    return (size_t)limit;
}

void
fp_objpool_init(void)
{
    const char *env = getenv("FASTPOSTGRES_OBJ_ARENA");
    size_t      cap = pool_default_bytes();
    void       *base;

    if (env != nullptr && env[0] != 0)
    {
        unsigned long long want = strtoull(env, nullptr, 10);
        if (want == 0) return; /* off by request */
        cap = (size_t)want;
    }
    cap &= ~(size_t)4095;
    if (cap == 0) return;

    base = mmap(nullptr, cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) return;
#ifdef MADV_HUGEPAGE
    (void)madvise(base, cap, MADV_HUGEPAGE); /* one fault per 2 MiB, not per 4 KiB */
#endif
    g_pool_base = (char *)base;
    g_pool_cap  = cap;
    g_pool_used = 0;
    g_pool_on   = true;

    if (!pool_templates() || !pool_sentinel())
    {
        g_pool_on        = false;
        g_pool_exhausted = false;
        return;               /* the mapped arena is left alone: nothing was handed out */
    }
    g_pool_exhausted = false; /* the verification secs are not a real exhaustion */
}

PyObject *
fp_birth_float(double v)
{
    PyObject *o = cell_new(sizeof(PyFloatObject), &PyFloat_Type);

    if (o != nullptr)
    {
        *(double *)((char *)o + offsetof(PyFloatObject, ob_fval)) = v;
        return o;
    }
    g_pool_fallbacks++;
    return PyFloat_FromDouble(v);
}

PyObject *
fp_birth_long(long long v)
{
    uint64_t  mag;
    int       nd = 1, neg;
    PyObject *o;
    digit    *d;

    if (v >= -5 && v <= 256) return PyLong_FromLong((long)v); /* the cache; immortal already */
    neg = v < 0;
    mag = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    if (mag >> 30) nd = 2;
    if (mag >> 60) nd = 3;

    o = cell_new(offsetof(PyLongObject, long_value.ob_digit) + (size_t)nd * sizeof(digit), &PyLong_Type);
    if (o == nullptr)
    {
        g_pool_fallbacks++;
        return PyLong_FromLongLong(v);
    }
    memcpy((char *)o + offsetof(PyLongObject, long_value.lv_tag), &g_pool_long_tag[nd][neg],
           sizeof(uintptr_t));
    d = (digit *)((char *)o + offsetof(PyLongObject, long_value.ob_digit));
    for (int i = 0; i < nd; i++)
        d[i] = (digit)((mag >> (PyLong_SHIFT * i)) & (uint64_t)PyLong_MASK);
    return o;
}

PyObject *
fp_birth_text(const char *p, Py_ssize_t n)
{
    Py_ssize_t i = 0;
    char      *cell;
    PyObject  *o;

    if (n <= 0) goto fallback; /* the empty string is the interpreter's singleton */
    for (; i + 8 <= n; i += 8)
    {
        uint64_t w;
        memcpy(&w, p + i, 8);
        if (w & 0x8080808080808080ULL) goto fallback; /* not ASCII: the decode is the normal path's job */
    }
    for (; i < n; i++)
        if (((unsigned char)p[i] & 0x80) != 0) goto fallback;

    cell = (char *)pool_take((size_t)n + (size_t)g_pool_str_data + 1);
    if (cell == nullptr) goto fallback;
    memcpy(cell, g_pool_str_hdr, sizeof(PyASCIIObject));
    o                                                       = (PyObject *)cell;
    o->ob_type                                              = &PyUnicode_Type;
    o->ob_refcnt                                            = g_pool_immortal;
    *(Py_ssize_t *)(cell + offsetof(PyASCIIObject, length)) = n;
    *(Py_hash_t *)(cell + offsetof(PyASCIIObject, hash))    = -1; /* computed on demand, as usual */
    memcpy(cell + g_pool_str_data, p, (size_t)n);
    cell[g_pool_str_data + n] = 0;
    g_pool_births++;
    return o;

fallback:
    g_pool_fallbacks++;
    return nullptr;
}

/* One line per process, so a benchmark log says how it was read. */
const char *
fp_objpool_mode(void)
{
    if (g_pool_base == nullptr) return "off";
    return g_pool_on ? "immortal" : (g_pool_exhausted ? "exhausted" : "off");
}

void
fp_objpool_report(void)
{
    if (g_pool_base == nullptr) return;
    fprintf(stderr, "objpool: %s cap=%zu MiB used=%zu MiB objects=%zu fallbacks=%zu%s\n", fp_objpool_mode(),
            g_pool_cap >> 20, g_pool_used >> 20, g_pool_births, g_pool_fallbacks,
            g_pool_on ? "" : " (values use the normal constructors)");
}

/* ---- decoding a whole batch ----------------------------------------------
 *
 * A column's values share one kind, so the kind switch belongs outside the row
 * loop, and the births have to be visible here for the compiler to inline them
 * into the loop body. The per-value call into one generic decode(kind, row)
 * cost more than the births themselves on the benchmark's row shape (~175 of
 * 183 ns/row), which is why these loops live next to the pool rather than in
 * segment.cpp. `row_column<K>` instantiates once per wire kind.
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
            return fp_birth_long(v);
        }
        case FASTPOSTGRES_KIND_INT32: {
            int32_t v;
            memcpy(&v, c->data + row * 4, 4);
            return fp_birth_long(v);
        }
        case FASTPOSTGRES_KIND_INT64: {
            int64_t v;
            memcpy(&v, c->data + row * 8, 8);
            return fp_birth_long(v);
        }
        case FASTPOSTGRES_KIND_FLOAT4: {
            float v;
            memcpy(&v, c->data + row * 4, 4);
            return fp_birth_float((double)v);
        }
        case FASTPOSTGRES_KIND_FLOAT8: {
            double v;
            memcpy(&v, c->data + row * 8, 8);
            return fp_birth_float(v);
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
            PyObject      *born = fp_birth_text(base + s0, (Py_ssize_t)(s1 - s0));

            if (born != nullptr) return born;
            return PyUnicode_FromStringAndSize(base + s0, (Py_ssize_t)(s1 - s0));
        }
    }
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
        return fp_birth_long(v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_INT32)
    {
        int32_t v;
        memcpy(&v, data + r * 4, 4);
        return fp_birth_long(v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_INT64)
    {
        int64_t v;
        memcpy(&v, data + r * 8, 8);
        return fp_birth_long(v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_FLOAT4)
    {
        float v;
        memcpy(&v, data + r * 4, 4);
        return fp_birth_float((double)v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_FLOAT8)
    {
        double v;
        memcpy(&v, data + r * 8, 8);
        return fp_birth_float(v);
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
        {
            PyObject *born = fp_birth_text(base + s0, (Py_ssize_t)(s1 - s0));

            if (born != nullptr) return born;
            return PyUnicode_FromStringAndSize(base + s0, (Py_ssize_t)(s1 - s0));
        }
    }
}

template <uint8_t K>
inline bool
row_column(PyObject *rows, uint64_t nrows, const FastpostgresColumn *c, uint32_t cidx, bool nullfree)
{
    for (uint64_t r = 0; r < nrows; r++)
    {
        PyObject *row = PyList_GET_ITEM(rows, (Py_ssize_t)r);
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
fp_decode_columns(const FastpostgresColumn *cols, uint32_t ncols, uint64_t nrows, PyObject *rows)
{
    for (uint32_t c = 0; c < ncols; c++)
    {
        bool nullfree = !fastpostgres_nullmap_any(cols[c].nulls, (size_t)((nrows + 7) / 8));
        bool ok;

        switch (cols[c].kind)
        {
            case FASTPOSTGRES_KIND_BOOL:
                ok = row_column<FASTPOSTGRES_KIND_BOOL>(rows, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_INT16:
                ok = row_column<FASTPOSTGRES_KIND_INT16>(rows, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_INT32:
                ok = row_column<FASTPOSTGRES_KIND_INT32>(rows, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_INT64:
                ok = row_column<FASTPOSTGRES_KIND_INT64>(rows, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_FLOAT4:
                ok = row_column<FASTPOSTGRES_KIND_FLOAT4>(rows, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_FLOAT8:
                ok = row_column<FASTPOSTGRES_KIND_FLOAT8>(rows, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_DATE:
                ok = row_column<FASTPOSTGRES_KIND_DATE>(rows, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_TIME:
                ok = row_column<FASTPOSTGRES_KIND_TIME>(rows, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_TIMESTAMP:
                ok = row_column<FASTPOSTGRES_KIND_TIMESTAMP>(rows, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_TIMESTAMPTZ:
                ok = row_column<FASTPOSTGRES_KIND_TIMESTAMPTZ>(rows, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_UUID:
                ok = row_column<FASTPOSTGRES_KIND_UUID>(rows, nrows, &cols[c], c, nullfree);
                break;
            case FASTPOSTGRES_KIND_BYTEA:
                ok = row_column<FASTPOSTGRES_KIND_BYTEA>(rows, nrows, &cols[c], c, nullfree);
                break;
            default:
                ok = row_column<FASTPOSTGRES_KIND_TEXT>(rows, nrows, &cols[c], c, nullfree);
                break;
        }
        if (!ok) return false;
    }
    return true;
}

/* ---- decoding a row frame -------------------------------------------------
 *
 * The server sends PostgreSQL's own tuple bytes (rows.c), so the walk here
 * mirrors heap_deform_tuple: the offset advances by each attribute's alignment
 * and length, a null attribute takes no space at all, and the null bitmap sits
 * at t_bits rather than at the end of the header area. The per-column layout is
 * worked out once per batch, so a value costs the walk plus one jump-table
 * switch.
 */

namespace
{

struct AttLayout {
    int32_t  attlen; /* -1 for the varlena kinds */
    uint32_t alignby;
};

inline PyObject *
tuple_value(uint8_t kind, const uint8_t *p)
{
    switch (kind)
    {
        case FASTPOSTGRES_KIND_BOOL:
            return PyBool_FromLong(*p != 0);
        case FASTPOSTGRES_KIND_INT16: {
            int16_t v;
            memcpy(&v, p, 2);
            return fp_birth_long(v);
        }
        case FASTPOSTGRES_KIND_INT32: {
            int32_t v;
            memcpy(&v, p, 4);
            return fp_birth_long(v);
        }
        case FASTPOSTGRES_KIND_INT64: {
            int64_t v;
            memcpy(&v, p, 8);
            return fp_birth_long(v);
        }
        case FASTPOSTGRES_KIND_FLOAT4: {
            float v;
            memcpy(&v, p, 4);
            return fp_birth_float((double)v);
        }
        case FASTPOSTGRES_KIND_FLOAT8: {
            double v;
            memcpy(&v, p, 8);
            return fp_birth_float(v);
        }
        case FASTPOSTGRES_KIND_DATE:
            return date_from_days(load_le32(p));
        case FASTPOSTGRES_KIND_TIME:
            return time_from_us(load_le64(p));
        case FASTPOSTGRES_KIND_TIMESTAMP:
            return timestamp_from_us(load_le64(p), 0);
        case FASTPOSTGRES_KIND_TIMESTAMPTZ:
            return timestamp_from_us(load_le64(p), 1);
        case FASTPOSTGRES_KIND_UUID:
            return uuid_from_bytes(p);
        case FASTPOSTGRES_KIND_BYTEA:
            return PyBytes_FromStringAndSize((const char *)fastpostgres_vardata_any(p),
                                             (Py_ssize_t)fastpostgres_vardata_len(p));
        default: {
            const char *d    = (const char *)fastpostgres_vardata_any(p);
            Py_ssize_t  n    = (Py_ssize_t)fastpostgres_vardata_len(p);
            PyObject   *born = fp_birth_text(d, n);

            if (born != nullptr) return born;
            return PyUnicode_FromStringAndSize(d, n);
        }
    }
}

PyObject *
decode_tuple(const FastpostgresColumn *cols, uint32_t ncols, const AttLayout *layout, const uint8_t *row,
             uint32_t len)
{
    PyObject *out = PyTuple_New((Py_ssize_t)ncols);
    uint32_t  natts, off;
    bool      hasnull;

    if (out == nullptr) return nullptr;

    off = fastpostgres_tuple_hoff(row);
    if (off < FASTPOSTGRES_HT_BITS || off > len)
    {
        Py_DECREF(out);
        PyErr_SetString(PyExc_ValueError, "row tuple: bad header offset");
        return nullptr;
    }
    natts   = fastpostgres_tuple_natts(row);
    hasnull = fastpostgres_tuple_hasnull(row) != 0;

    for (uint32_t c = 0; c < ncols; c++)
    {
        PyObject *v;

        if (c >= natts)
        {
            /* the writer expands short tuples before sending them, so anything
             * else is a frame we do not understand */
            Py_DECREF(out);
            PyErr_SetString(PyExc_ValueError, "row tuple: fewer attributes than columns");
            return nullptr;
        }

        if (hasnull && fastpostgres_tuple_isnull(row, c))
        {
            v = Py_None;
            Py_INCREF(v); /* a null stores nothing: the offset does not move */
        }
        else
        {
            const uint8_t *p;

            if (layout[c].attlen < 0)
                off = fastpostgres_att_pointer_align(off, layout[c].alignby, row + off);
            else
                off = fastpostgres_att_align(off, layout[c].alignby);

            p = row + off;
            v = tuple_value(cols[c].kind, p);
            if (v == nullptr)
            {
                Py_DECREF(out);
                return nullptr;
            }
            off = fastpostgres_att_addlength(off, layout[c].attlen, p);
        }
        PyTuple_SET_ITEM(out, (Py_ssize_t)c, v);
    }
    return out;
}

} /* namespace */

bool
fp_decode_tuples(const FastpostgresColumn *cols, uint32_t ncols, FastpostgresRowsReader *rd, PyObject *rows)
{
    std::vector<AttLayout> layout(ncols);
    const uint8_t         *row;
    uint32_t               len;
    uint64_t               expect = rd->left;
    uint64_t               r      = 0;

    for (uint32_t c = 0; c < ncols; c++)
    {
        int width = fastpostgres_kind_width(cols[c].kind);

        if (width == 0 && !fastpostgres_kind_is_varlena(cols[c].kind))
        {
            PyErr_SetString(PyExc_ValueError, "row frame: unknown wire kind");
            return false;
        }
        layout[c].attlen  = width > 0 ? width : -1;
        layout[c].alignby = (uint32_t)fastpostgres_kind_alignby(cols[c].kind);
    }

    while (fastpostgres_rows_next(rd, &row, &len))
    {
        PyObject *tup = decode_tuple(cols, ncols, layout.data(), row, len);

        if (tup == nullptr) return false;
        PyList_SET_ITEM(rows, (Py_ssize_t)r, tup);
        r++;
    }
    if (r != expect)
    {
        /* a short frame would leave NULL slots in the caller's list */
        PyErr_SetString(PyExc_ValueError, "truncated row payload");
        return false;
    }
    return true;
}
