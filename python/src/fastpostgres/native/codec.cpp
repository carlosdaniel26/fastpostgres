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

/* The row decode's cost in TSC cycles: two rdtsc per batch, free at this scale,
 * and the one number that says whether the loop is worth hand-writing (perf
 * cannot run in the container, so this stands in for it). */
static unsigned long long g_dec_cycles;
static unsigned long long g_dec_rows;
static unsigned long long g_read_cycles;
static unsigned long long g_read_rows;

/* One line per process, so a benchmark log says how the decode cost broke down. */
void
fp_stats_report(void)
{
    if (g_read_rows != 0)
        fprintf(stderr, "read_rows: %.1f cycles/row over %llu rows (decode %.1f, frame+desc+list %.1f)\n",
                (double)g_read_cycles / (double)g_read_rows, g_read_rows,
                (double)g_dec_cycles / (double)g_dec_rows,
                (double)(g_read_cycles - g_dec_cycles) / (double)g_read_rows);
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
    g_read_cycles += cycles;
    g_read_rows += rows;
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
#define FP_ROWS_BLOCK 8

struct AttLayout {
    int32_t  attlen;   /* -1 for the varlena kinds */
    uint32_t alignby;
    uint32_t nullbyte; /* the tuple byte holding this column's null bit */
    uint8_t  nullbit;
};

/* One block being decoded: the rows, where each row's walk has got to, whether
 * it carries a null bitmap, and the tuple its values are landing in. */
struct RowBlock {
    const uint8_t *row[FP_ROWS_BLOCK];
    PyObject      *tup[FP_ROWS_BLOCK];
    uint32_t       off[FP_ROWS_BLOCK];
    bool           hasnull[FP_ROWS_BLOCK];
    uint32_t       n;
};

template <uint8_t K>
inline PyObject *
tuple_value(const uint8_t *p)
{
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
    else if constexpr (K == FASTPOSTGRES_KIND_UUID)
        return uuid_from_bytes(p);
    else if constexpr (K == FASTPOSTGRES_KIND_BYTEA)
        return PyBytes_FromStringAndSize((const char *)fastpostgres_vardata_any(p),
                                         (Py_ssize_t)fastpostgres_vardata_len(p));
    else
    {
        const char *d = (const char *)fastpostgres_vardata_any(p);

        return PyUnicode_FromStringAndSize(d, (Py_ssize_t)fastpostgres_vardata_len(p));
    }
}

/* One column of a block, in row order. K settles which alignment the value
 * needs and whether it is varlena at compile time, so the body carries no kind
 * test; the layout supplies the rest, resolved once per batch. */
template <uint8_t K>
inline bool
block_column(RowBlock &b, const AttLayout &layout, uint32_t cidx)
{
    for (uint32_t k = 0; k < b.n; k++)
    {
        const uint8_t *row = b.row[k];
        PyObject      *v;

        if (b.hasnull[k] && (row[layout.nullbyte] & layout.nullbit) == 0)
        {
            v = Py_None;
            Py_INCREF(v); /* a null stores nothing: the offset does not move */
        }
        else
        {
            uint32_t       o;
            const uint8_t *p;

            if (fastpostgres_kind_is_varlena(K))
                o = fastpostgres_att_pointer_align(b.off[k], layout.alignby, row + b.off[k]);
            else
                o = fastpostgres_att_align(b.off[k], layout.alignby);

            p = row + o;
            v = tuple_value<K>(p);
            if (v == nullptr) return false;
            b.off[k] = fastpostgres_att_addlength(o, layout.attlen, p);
        }
        PyTuple_SET_ITEM(b.tup[k], (Py_ssize_t)cidx, v);
    }
    return true;
}

/* One block: the switch runs once per column, and each column's body then walks
 * every row of the block without it. */
inline bool
decode_block(const FastpostgresColumn *cols, uint32_t ncols, const AttLayout *layout, RowBlock &b)
{
    for (uint32_t c = 0; c < ncols; c++)
    {
        bool ok;

        switch (cols[c].kind)
        {
            case FASTPOSTGRES_KIND_BOOL:
                ok = block_column<FASTPOSTGRES_KIND_BOOL>(b, layout[c], c);
                break;
            case FASTPOSTGRES_KIND_INT16:
                ok = block_column<FASTPOSTGRES_KIND_INT16>(b, layout[c], c);
                break;
            case FASTPOSTGRES_KIND_INT32:
                ok = block_column<FASTPOSTGRES_KIND_INT32>(b, layout[c], c);
                break;
            case FASTPOSTGRES_KIND_INT64:
                ok = block_column<FASTPOSTGRES_KIND_INT64>(b, layout[c], c);
                break;
            case FASTPOSTGRES_KIND_FLOAT4:
                ok = block_column<FASTPOSTGRES_KIND_FLOAT4>(b, layout[c], c);
                break;
            case FASTPOSTGRES_KIND_FLOAT8:
                ok = block_column<FASTPOSTGRES_KIND_FLOAT8>(b, layout[c], c);
                break;
            case FASTPOSTGRES_KIND_DATE:
                ok = block_column<FASTPOSTGRES_KIND_DATE>(b, layout[c], c);
                break;
            case FASTPOSTGRES_KIND_TIME:
                ok = block_column<FASTPOSTGRES_KIND_TIME>(b, layout[c], c);
                break;
            case FASTPOSTGRES_KIND_TIMESTAMP:
                ok = block_column<FASTPOSTGRES_KIND_TIMESTAMP>(b, layout[c], c);
                break;
            case FASTPOSTGRES_KIND_TIMESTAMPTZ:
                ok = block_column<FASTPOSTGRES_KIND_TIMESTAMPTZ>(b, layout[c], c);
                break;
            case FASTPOSTGRES_KIND_UUID:
                ok = block_column<FASTPOSTGRES_KIND_UUID>(b, layout[c], c);
                break;
            case FASTPOSTGRES_KIND_BYTEA:
                ok = block_column<FASTPOSTGRES_KIND_BYTEA>(b, layout[c], c);
                break;
            default:
                ok = block_column<FASTPOSTGRES_KIND_TEXT>(b, layout[c], c);
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
    RowBlock               block;
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
        layout[c].attlen   = width > 0 ? width : -1;
        layout[c].alignby  = (uint32_t)fastpostgres_kind_alignby(cols[c].kind);
        layout[c].nullbyte = FASTPOSTGRES_HT_BITS + (c >> 3);
        layout[c].nullbit  = (uint8_t)(1u << (c & 7));
    }

    unsigned long long t0 = __rdtsc();

    for (;;)
    {
        /* The frame checks are made where the tuple is made, once per row; the
         * values are decoded after the block is complete, so each tuple goes
         * into the caller's list before it is filled. A half-filled tuple is
         * only reachable through that list, and both the GC and the deallocator
         * walk its items null-safely. */
        block.n = 0;
        while (block.n < FP_ROWS_BLOCK && fastpostgres_rows_next(rd, &row, &len))
        {
            PyObject *tup = PyTuple_New((Py_ssize_t)ncols);
            uint32_t  hoff;

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
            block.row[block.n]     = row;
            block.off[block.n]     = hoff;
            block.hasnull[block.n] = fastpostgres_tuple_hasnull(row) != 0;
            block.tup[block.n]     = tup;
            PyList_SET_ITEM(rows, (Py_ssize_t)(r + block.n), tup);
            block.n++;
        }
        if (block.n == 0) break;

        if (!decode_block(cols, ncols, layout.data(), block)) return false;
        r += block.n;
    }
    g_dec_cycles += __rdtsc() - t0;
    g_dec_rows += r;

    if (r != expect)
    {
        /* a short frame would leave NULL slots in the caller's list */
        PyErr_SetString(PyExc_ValueError, "truncated row payload");
        return false;
    }
    return true;
}
