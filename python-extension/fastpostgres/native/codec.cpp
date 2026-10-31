/* codec.cpp — turn PostgreSQL text values into Python objects by type OID, so
 * the client returns real types (int, float, bool, Decimal, datetime, bytes)
 * the way a native driver does, not bare strings. */
#include "codec.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>

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
        return PyDateTime_FromDateAndTime(y, m, d, (int)(rem / 3600000000LL),
                                          (int)(rem / 60000000LL % 60), (int)(rem / FP_US_PER_SEC % 60),
                                          (int)(rem % FP_US_PER_SEC));
    return PyDateTimeAPI->DateTime_FromDateAndTime(y, m, d, (int)(rem / 3600000000LL),
                                                   (int)(rem / 60000000LL % 60),
                                                   (int)(rem / FP_US_PER_SEC % 60),
                                                   (int)(rem % FP_US_PER_SEC), s_utc,
                                                   PyDateTimeAPI->DateTimeType);
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
        default: /* text family, json, xml, ... */
            return PyUnicode_FromString(s);
    }
}

PyObject *
fp_decode_column_nonull(const FastpostgresColumn *c, uint64_t row)
{
    switch (c->kind)
    {
        case FASTPOSTGRES_KIND_BOOL:
            return PyBool_FromLong(((const uint8_t *)c->data)[row] != 0);
        case FASTPOSTGRES_KIND_INT16: {
            int16_t v;
            memcpy(&v, c->data + row * 2, 2);
            return PyLong_FromLong(v);
        }
        case FASTPOSTGRES_KIND_INT32: {
            int32_t v;
            memcpy(&v, c->data + row * 4, 4);
            return PyLong_FromLong(v);
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
        /* The binary kinds: wrap the value that is already in the segment. No
         * digits, no hex, no NUL-terminated copy — that is the whole gain. */
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

PyObject *
fp_decode_column(const FastpostgresColumn *c, uint64_t row)
{
    if (fastpostgres_col_isnull(c, row)) Py_RETURN_NONE;
    return fp_decode_column_nonull(c, row);
}
