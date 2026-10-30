/* codec.cpp — turn PostgreSQL text values into Python objects by type OID, so
 * the client returns real types (int, float, bool, Decimal, datetime, bytes)
 * the way a native driver does, not bare strings. */
#include "codec.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>

#include <datetime.h>

/* PostgreSQL builtin type OIDs (fixed for a major version). */
enum
{
    OID_BOOL        = 16,
    OID_BYTEA       = 17,
    OID_CHAR        = 18,
    OID_NAME        = 19,
    OID_INT8        = 20,
    OID_INT2        = 21,
    OID_INT4        = 23,
    OID_TEXT        = 25,
    OID_OID         = 26,
    OID_JSON        = 114,
    OID_FLOAT4      = 700,
    OID_FLOAT8      = 701,
    OID_MONEY       = 790,
    OID_BPCHAR      = 1042,
    OID_VARCHAR     = 1043,
    OID_DATE        = 1082,
    OID_TIME        = 1083,
    OID_TIMESTAMP   = 1114,
    OID_TIMESTAMPTZ = 1184,
    OID_NUMERIC     = 1700,
    OID_UUID        = 2950,
    OID_JSONB       = 3802,
};

static PyObject *s_decimal = nullptr;   /* decimal.Decimal */
static PyObject *s_timezone = nullptr;  /* datetime.timezone */
static PyObject *s_timedelta = nullptr; /* datetime.timedelta */

bool fp_codec_init(void)
{
    PyDateTime_IMPORT;
    PyObject *dec = PyImport_ImportModule("decimal");
    if (!dec) return false;
    s_decimal = PyObject_GetAttrString(dec, "Decimal");
    Py_DECREF(dec);
    if (!s_decimal) return false;

    PyObject *dt = PyImport_ImportModule("datetime");
    if (!dt) return false;
    s_timezone = PyObject_GetAttrString(dt, "timezone");
    s_timedelta = PyObject_GetAttrString(dt, "timedelta");
    Py_DECREF(dt);
    return s_timezone != nullptr && s_timedelta != nullptr;
}

static PyObject *decimal_from_text(const char *s)
{
    PyObject *text = PyUnicode_FromString(s);
    if (!text) return nullptr;
    PyObject *value = PyObject_CallFunctionObjArgs(s_decimal, text, nullptr);
    Py_DECREF(text);
    return value;
}

static PyObject *bytes_from_bytea(const char *s)
{
    if (s[0] == '\\' && s[1] == 'x')
    {
        size_t      hexlen = strlen(s + 2);
        Py_ssize_t  n = (Py_ssize_t)(hexlen / 2);
        PyObject   *out = PyBytes_FromStringAndSize(nullptr, n);
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
static PyObject *parse_datetime(const char *s, int with_time, int with_tz)
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
    PyObject *out = PyDateTimeAPI->DateTime_FromDateAndTime(y, mo, d, h, mi, sec, us, tz,
                                                            PyDateTimeAPI->DateTimeType);
    Py_DECREF(tz);
    return out;
}

static PyObject *parse_time(const char *s)
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

PyObject *fp_decode_value(PGresult *res, int row, int col)
{
    if (PQgetisnull(res, row, col)) Py_RETURN_NONE;

    Oid         type = PQftype(res, col);
    const char *s = PQgetvalue(res, row, col);

    switch (type)
    {
        case OID_BOOL:
            if (s[0] == 't') Py_RETURN_TRUE;
            Py_RETURN_FALSE;
        case OID_INT2:
        case OID_INT4:
        case OID_INT8:
        case OID_OID:
            return PyLong_FromString(s, nullptr, 10);
        case OID_FLOAT4:
        case OID_FLOAT8:
            return PyFloat_FromDouble(strtod(s, nullptr));
        case OID_NUMERIC:
        case OID_MONEY:
            return decimal_from_text(s);
        case OID_BYTEA:
            return bytes_from_bytea(s);
        case OID_DATE:
            return parse_datetime(s, 0, 0);
        case OID_TIME:
            return parse_time(s);
        case OID_TIMESTAMP:
            return parse_datetime(s, 1, 0);
        case OID_TIMESTAMPTZ:
            return parse_datetime(s, 1, 1);
        default: /* text family, uuid, json, xml, ... */
            return PyUnicode_FromString(s);
    }
}

PyObject *fp_decode_column(const FastpostgresColumn *c, uint64_t row)
{
    if (fastpostgres_col_isnull(c, row)) Py_RETURN_NONE;

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
        default: { /* text: int32 offset table, then the raw bytes */
            const int32_t *off = (const int32_t *)c->data;
            const char    *base = (const char *)c->bytes;
            int32_t        s0 = off[row];
            int32_t        s1 = off[row + 1];
            const char    *s = base + s0;
            Py_ssize_t     len = (Py_ssize_t)(s1 - s0);

            switch (c->pgtype)
            {
                case OID_DATE:
                case OID_TIME:
                case OID_TIMESTAMP:
                case OID_TIMESTAMPTZ:
                case OID_BYTEA: {
                    /* the parsers need a NUL-terminated string */
                    char      buf[96];
                    Py_ssize_t n = len < (Py_ssize_t)sizeof(buf) - 1 ? len : (Py_ssize_t)sizeof(buf) - 1;
                    memcpy(buf, s, (size_t)n);
                    buf[n] = 0;
                    switch (c->pgtype)
                    {
                        case OID_DATE:
                            return parse_datetime(buf, 0, 0);
                        case OID_TIME:
                            return parse_time(buf);
                        case OID_TIMESTAMP:
                            return parse_datetime(buf, 1, 0);
                        case OID_TIMESTAMPTZ:
                            return parse_datetime(buf, 1, 1);
                        default:
                            return bytes_from_bytea(buf);
                    }
                }
                default:
                    return PyUnicode_FromStringAndSize(s, len);
            }
        }
    }
}
