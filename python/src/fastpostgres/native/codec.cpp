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
            return PyLong_FromLongLong(v);
        }
        case FASTPOSTGRES_KIND_INT32: {
            int32_t v;
            memcpy(&v, c->data + row * 4, 4);
            return PyLong_FromLongLong(v);
        }
        case FASTPOSTGRES_KIND_INT64: {
            int64_t v;
            memcpy(&v, c->data + row * 8, 8);
            return PyLong_FromLongLong(v);
        }
        case FASTPOSTGRES_KIND_FLOAT4: {
            float v;
            memcpy(&v, c->data + row * 4, 4);
            return PyFloat_FromDouble((double)v);
        }
        case FASTPOSTGRES_KIND_FLOAT8: {
            double v;
            memcpy(&v, c->data + row * 8, 8);
            return PyFloat_FromDouble(v);
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

            return PyUnicode_FromStringAndSize(base + s0, (Py_ssize_t)(s1 - s0));
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
        return PyLong_FromLongLong(v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_INT32)
    {
        int32_t v;
        memcpy(&v, data + r * 4, 4);
        return PyLong_FromLongLong(v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_INT64)
    {
        int64_t v;
        memcpy(&v, data + r * 8, 8);
        return PyLong_FromLongLong(v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_FLOAT4)
    {
        float v;
        memcpy(&v, data + r * 4, 4);
        return PyFloat_FromDouble((double)v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_FLOAT8)
    {
        double v;
        memcpy(&v, data + r * 8, 8);
        return PyFloat_FromDouble(v);
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
            return PyUnicode_FromStringAndSize(base + s0, (Py_ssize_t)(s1 - s0));
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
        return PyLong_FromLongLong(v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_INT32)
    {
        int32_t v;
        memcpy(&v, p, 4);
        return PyLong_FromLongLong(v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_INT64)
    {
        int64_t v;
        memcpy(&v, p, 8);
        return PyLong_FromLongLong(v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_FLOAT4)
    {
        float v;
        memcpy(&v, p, 4);
        return PyFloat_FromDouble((double)v);
    }
    else if constexpr (K == FASTPOSTGRES_KIND_FLOAT8)
    {
        double v;
        memcpy(&v, p, 8);
        return PyFloat_FromDouble(v);
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
        return PyUnicode_FromStringAndSize((const char *)data, (Py_ssize_t)len);
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
