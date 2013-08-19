/*
 * Copyright 2013, David Wilson.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include "centidb.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <structmember.h>
#include <sys/types.h>
#include <time.h>


// Reference to uuid.UUID().
static PyTypeObject *UUID_Type;
// Reference to datetime.datetime.utcoffset().
static PyObject *datetime_utcoffset;
// Reference to datetime.timedelta.total_seconds().
static PyObject *timedelta_total_seconds;


static int reader_init(struct reader *rdr, uint8_t *p, Py_ssize_t size)
{
    rdr->p = p;
    rdr->size = size;
    rdr->pos = 0;
    return 1;
}


static int reader_getc(struct reader *rdr, uint8_t *ch)
{
    if(rdr->pos < rdr->size) {
        *ch = rdr->p[rdr->pos++];
        return 1;
    }
    return 0;
}


static int reader_ensure(struct reader *rdr, Py_ssize_t n)
{
    if((rdr->size - rdr->pos) < n) {
        PyErr_Format(PyExc_ValueError,
            "expected %lld bytes at position %lld, but only %lld remain.",
            (long long) n, (long long) rdr->pos,
            (long long) (rdr->size - rdr->pos));
        return 0;
    }
    return 1;
}


static uint64_t reader_getchar(struct reader *rdr)
{
    return rdr->p[rdr->pos++];
}


static int writer_init(struct writer *wtr, Py_ssize_t initial)
{
    wtr->pos = 0;
    wtr->s = PyString_FromStringAndSize(NULL, initial);
    return wtr->s != NULL;
}


static int writer_grow(struct writer *wtr)
{
    Py_ssize_t cursize = PyString_GET_SIZE(wtr->s);
    Py_ssize_t newsize = cursize * 2;
    if(newsize > (cursize + 512)) {
        newsize = cursize + 512;
    }
    if(-1 == _PyString_Resize(&(wtr->s), newsize)) {
        PyErr_NoMemory();
        return 0;
    }
    return 1;
}


/* Append a single ordinal `o` to the buffer, growing it as necessary. */
static int writer_putc(struct writer *wtr, uint8_t o)
{
    if(! wtr->s) {
        return 0;
    }

    if((1 + wtr->pos) == PyString_GET_SIZE(wtr->s)) {
        if(! writer_grow(wtr)) {
            return 0;
        }
    }

    ((uint8_t *restrict)PyString_AS_STRING(wtr->s))[wtr->pos++] = o;
    return 1;
}


static int writer_ensure(struct writer *wtr, Py_ssize_t size)
{
    while((PyString_GET_SIZE(wtr->s) - (wtr->pos + 1)) < size) {
        if(! writer_grow(wtr)) {
            return 0;
        }
    }
    return 1;
}


static void writer_putchar(struct writer *wtr, uint8_t ch)
{
    PyString_AS_STRING(wtr->s)[wtr->pos++] = ch;
}


/* Append a bytestring `b` to the buffer, growing it as necessary. */
static int writer_puts(struct writer *wtr, const char *restrict s, Py_ssize_t size)
{
    assert(wtr->s);
    if(! writer_ensure(wtr, size)) {
        return 0;
    }

    memcpy(PyString_AS_STRING(wtr->s) + wtr->pos, s, size);
    wtr->pos += size;
    return 1;
}


/* Resize the string to its final size, and return it. The StringWriter should
 * be discarded after calling finalize(). */
static PyObject *writer_fini(struct writer *wtr)
{
    if(! wtr->s) {
        return NULL;
    }
    _PyString_Resize(&(wtr->s), wtr->pos);
    PyObject *o = wtr->s;
    wtr->s = NULL;
    return o;
}


static PyObject *tuplize(PyObject *self, PyObject *arg)
{
    if(Py_TYPE(arg) == &PyTuple_Type) {
        Py_INCREF(arg);
    } else {
        PyObject *tup = PyTuple_New(1);
        if(! tup) {
            return NULL;
        }
        Py_INCREF(arg);
        PyTuple_SET_ITEM(tup, 0, arg);
        arg = tup;
    }
    return arg;
}


static int write_int(struct writer *wtr, uint64_t v, enum ElementKind kind)
{
    if(kind) {
        if(! writer_putc(wtr, kind)) {
            return 0;
        }
    }

    int ok = 1;
    if(v <= 240ULL) {
        ok = writer_putc(wtr, v);
    } else if(v <= 2287ULL) {
        if((ok = writer_ensure(wtr, 2))) {
            v -= 240ULL;
            writer_putchar(wtr, 241 + (uint8_t) (v >> 8));
            writer_putchar(wtr, (uint8_t) (v & 0xff));
        }
    } else if(v <= 67823) {
        if((ok = writer_ensure(wtr, 3))) {
            v -= 2288ULL;
            writer_putchar(wtr, 0xf9);
            writer_putchar(wtr, (uint8_t) (v >> 8));
            writer_putchar(wtr, (uint8_t) (v & 0xff));
        }
    } else if(v <= 0xffffffULL) {
        if((ok = writer_ensure(wtr, 4))) {
            writer_putchar(wtr, 0xfa);
            writer_putchar(wtr, (uint8_t) (v >> 16));
            writer_putchar(wtr, (uint8_t) (v >> 8));
            writer_putchar(wtr, (uint8_t) (v));
        }
    } else if(v <= 0xffffffffULL) {
        if((ok = writer_ensure(wtr, 5))) {
            writer_putchar(wtr, 0xfb);
            writer_putchar(wtr, (uint8_t) (v >> 24));
            writer_putchar(wtr, (uint8_t) (v >> 16));
            writer_putchar(wtr, (uint8_t) (v >> 8));
            writer_putchar(wtr, (uint8_t) (v));
        }
    } else if(v <= 0xffffffffffULL) {
        if((ok = writer_ensure(wtr, 6))) {
            writer_putchar(wtr, 0xfc);
            writer_putchar(wtr, (uint8_t) (v >> 32));
            writer_putchar(wtr, (uint8_t) (v >> 24));
            writer_putchar(wtr, (uint8_t) (v >> 16));
            writer_putchar(wtr, (uint8_t) (v >> 8));
            writer_putchar(wtr, (uint8_t) (v));
        }
    } else if(v <= 0xffffffffffffULL) {
        if((ok = writer_ensure(wtr, 7))) {
            writer_putchar(wtr, 0xfd);
            writer_putchar(wtr, (uint8_t) (v >> 40));
            writer_putchar(wtr, (uint8_t) (v >> 32));
            writer_putchar(wtr, (uint8_t) (v >> 24));
            writer_putchar(wtr, (uint8_t) (v >> 16));
            writer_putchar(wtr, (uint8_t) (v >> 8));
            writer_putchar(wtr, (uint8_t) (v));
        }
    } else if(v <= 0xffffffffffffffULL) {
        if((ok = writer_ensure(wtr, 8))) {
            writer_putchar(wtr, 0xfe);
            writer_putchar(wtr, (uint8_t) (v >> 48));
            writer_putchar(wtr, (uint8_t) (v >> 40));
            writer_putchar(wtr, (uint8_t) (v >> 32));
            writer_putchar(wtr, (uint8_t) (v >> 24));
            writer_putchar(wtr, (uint8_t) (v >> 16));
            writer_putchar(wtr, (uint8_t) (v >> 8));
            writer_putchar(wtr, (uint8_t) (v));
        }
    } else {
        if((ok = writer_ensure(wtr, 9))) {
            writer_putchar(wtr, 0xff);
            writer_putchar(wtr, (uint8_t) (v >> 56));
            writer_putchar(wtr, (uint8_t) (v >> 48));
            writer_putchar(wtr, (uint8_t) (v >> 40));
            writer_putchar(wtr, (uint8_t) (v >> 32));
            writer_putchar(wtr, (uint8_t) (v >> 24));
            writer_putchar(wtr, (uint8_t) (v >> 16));
            writer_putchar(wtr, (uint8_t) (v >> 8));
            writer_putchar(wtr, (uint8_t) (v));
        }
    }
    return ok;
}


static PyObject *py_pack_int(PyObject *self, PyObject *args)
{
    char *prefix;
    Py_ssize_t prefix_len;
    uint64_t v;

    if(! PyArg_ParseTuple(args, "s#l", (char **) &prefix, &prefix_len, &v)) {
        return NULL;
    }

    struct writer wtr;
    if(writer_init(&wtr, 9)) {
        if(writer_puts(&wtr, prefix, prefix_len)) {
            if(write_int(&wtr, v, 0)) {
                return writer_fini(&wtr);
            }
        }
    }
    return NULL;
}


static int write_str(struct writer *wtr, uint8_t *restrict p, Py_ssize_t length,
                     enum ElementKind kind)
{
    if(kind) {
        if(! writer_putc(wtr, kind)) {
            return 0;
        }
    }

    int shift = 1;
    uint8_t trailer = 0;

    int ret = 1;
    while(ret && length--) {
        uint8_t o = *(p++);
        ret = writer_putc(wtr, 0x80 | trailer | (o >> shift));
        if(shift < 7) {
            trailer = (o << (7 - shift));
            shift++;
        } else {
            ret = writer_putc(wtr, 0x80 | o);
            shift = 1;
            trailer = 0;
        }
    }

    if(shift > 1) {
        ret = writer_putc(wtr, trailer);
        if(trailer != 0) {
            ret = writer_putc(wtr, 0);
        }
    } else {
        ret = writer_putc(wtr, 0);
    }
    return ret;
}


static int timedelta_get_ms(PyObject *td, long *ms)
{
    if(! PyDelta_CheckExact(td)) {
        return -1;
    }
    PyDateTime_Delta *delta = (void *)td;
    long ms_ = delta->days * (60 * 60 * 24 * 1000);
    ms_ += delta->seconds * 1000;
    ms_ += delta->microseconds / 1000;
    *ms = ms_;
    return 0;
}


static int get_utcoffset_secs(PyObject *dt)
{
    PyObject *td = PyObject_CallFunctionObjArgs(datetime_utcoffset, dt, NULL);
    if(! td) {
        return -1;
    } else if(td == Py_None) {
        Py_DECREF(td);
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        time_t local = mktime(&tm);
        time_t utc = timegm(&tm);
        return (int) (utc - local);
    } else {
        PyObject *py_secs;
        py_secs = PyObject_CallFunctionObjArgs(timedelta_total_seconds, td, NULL);
        Py_DECREF(td);
        if(! py_secs) {
            return -1;
        }
        long offset = PyInt_AsLong(py_secs);
        Py_DECREF(py_secs);
        return (int) (offset / (15 * 60));
    }
}


static int write_time(struct writer *wtr, PyObject *dt)
{
    struct tm tm = {
        .tm_sec = PyDateTime_DATE_GET_SECOND(dt),
        .tm_min = PyDateTime_DATE_GET_MINUTE(dt),
        .tm_hour = PyDateTime_DATE_GET_HOUR(dt),
        .tm_mday = PyDateTime_GET_DAY(dt),
        .tm_mon = PyDateTime_GET_MONTH(dt) - 1,
        .tm_year = PyDateTime_GET_YEAR(dt) - 1900
    };

    int64_t ts = (int64_t) timegm(&tm);

    int offset_secs = get_utcoffset_secs(dt);
    if(offset_secs == -1) {
        return -1;
    }

    int offset_bits = UTCOFFSET_SHIFT + (offset_secs / UTCOFFSET_DIV);
    assert(offset_bits <= 0x7f && offset_bits >= 0);
    ts *= 1000;
    ts += PyDateTime_DATE_GET_MICROSECOND(dt) / 1000;
    ts <<= 7;
    ts |= offset_bits;

    if(ts < 0) {
        return write_int(wtr, (uint64_t) -ts, KIND_NEG_TIME);
    } else {
        return write_int(wtr, (uint64_t) ts, KIND_TIME);
    }
}


static int c_encode_value(struct writer *wtr, PyObject *arg)
{
    int ret = 0;
    PyTypeObject *type = Py_TYPE(arg);

    if(arg == Py_None) {
        ret = writer_putc(wtr, KIND_NULL);
    } else if(type == &PyInt_Type) {
        long v = PyInt_AS_LONG(arg);
        if(v < 0) {
            ret = write_int(wtr, -v, KIND_NEG_INTEGER);
        } else {
            ret = write_int(wtr, v, KIND_INTEGER);
        }
    } else if(type == &PyString_Type) {
        ret = write_str(wtr, (uint8_t *)PyString_AS_STRING(arg),
                             PyString_GET_SIZE(arg), KIND_BLOB);
    } else if(type == &PyUnicode_Type) {
        PyObject *utf8 = PyUnicode_EncodeUTF8(PyUnicode_AS_UNICODE(arg),
            PyUnicode_GET_SIZE(arg), "strict");
        if(utf8) {
            ret = write_str(wtr, (uint8_t *)PyString_AS_STRING(utf8),
                                 PyString_GET_SIZE(utf8), KIND_TEXT);
            Py_DECREF(utf8);
        }
    } else if(type == &PyBool_Type) {
        ret = writer_putc(wtr, KIND_BOOL);
        ret = writer_putc(wtr, (uint8_t) (arg == Py_True));
    } else if(type == &PyLong_Type) {
        int64_t i64 = PyLong_AsLongLong(arg);
        if(! PyErr_Occurred()) {
            if(i64 < 0) {
                ret = write_int(wtr, -i64, KIND_NEG_INTEGER);
            } else {
                ret = write_int(wtr, i64, KIND_INTEGER);
            }
        }
    } else if(PyDateTime_CheckExact(arg)) {
        ret = write_time(wtr, arg);
    } else if(type == UUID_Type) {
        PyObject *ss = PyObject_CallMethod(arg, "get_bytes", NULL);
        if(ss) {
            assert(Py_TYPE(ss) == &PyString_Type);
            ret = write_str(wtr, (uint8_t *)PyString_AS_STRING(ss),
                                 PyString_GET_SIZE(ss), KIND_UUID);
            Py_DECREF(ss);
        }
    } else {
        const char *name = arg->ob_type->tp_name;
        PyErr_Format(PyExc_TypeError, "got unsupported type %.200s", name);
    }

    return ret;
}


static int c_encode_key(struct writer *wtr, PyObject *tup)
{
    int ret = 1;
    for(Py_ssize_t i = 0; ret && i < PyTuple_GET_SIZE(tup); i++) {
        ret = c_encode_value(wtr, PyTuple_GET_ITEM(tup, i));
    }
    return ret;
}


static PyObject *packs(PyObject *self, PyObject *args)
{
    uint8_t *prefix = NULL;
    Py_ssize_t prefix_size;

    Py_ssize_t arg_count = PyTuple_GET_SIZE(args);
    if(arg_count != 2) {
        PyErr_SetString(PyExc_TypeError,
            "packs() takes exactly 2 arguments.");
        return NULL;
    }

    PyObject *py_prefix = PyTuple_GET_ITEM(args, 0);
    if(Py_TYPE(py_prefix) != &PyString_Type) {
        PyErr_SetString(PyExc_TypeError, "packs() prefix must be str.");
        return NULL;
    }
    prefix = (uint8_t *) PyString_AS_STRING(py_prefix);
    prefix_size = PyString_GET_SIZE(py_prefix);

    struct writer wtr;
    if(! writer_init(&wtr, 20)) {
        return NULL;
    }

    int ret = 1;
    if(prefix) {
        if(! writer_puts(&wtr, (char *)prefix, prefix_size)) {
            return NULL;
        }
    }

    PyObject *tups = PyTuple_GET_ITEM(args, 1);
    PyTypeObject *type = Py_TYPE(tups);

    if(type != &PyList_Type) {
        if(type != &PyTuple_Type) {
            ret = c_encode_value(&wtr, tups);
        } else {
            ret = c_encode_key(&wtr, tups);
        }
    } else {
        for(int i = 0; ret && i < PyList_GET_SIZE(tups); i++) {
            if(i) {
                ret = writer_putc(&wtr, KIND_SEP);
            }
            PyObject *elem = PyList_GET_ITEM(tups, i);
            type = Py_TYPE(elem);
            if(type != &PyTuple_Type) {
                ret = c_encode_value(&wtr, elem);
            } else {
                ret = c_encode_key(&wtr, elem);
            }
        }
    }

    if(ret) {
        return writer_fini(&wtr);
    }
    return NULL;
}


static int read_plain_int(struct reader *rdr, uint64_t *u64)
{
    uint8_t ch = 0;
    if(! reader_getc(rdr, &ch)) {
        return 0;
    }

    uint64_t v;
    int ok = 1;

    if(ch <= 240) {
        *u64 = ch;
    } else if(ch <= 248) {
        if((ok = reader_ensure(rdr, 1))) {
            v  = 240;
            v += 256 * (ch - 241);
            v += reader_getchar(rdr);
        }
    } else if(ch == 249) {
        if((ok = reader_ensure(rdr, 2))) {
            v  = 2288;
            v += 256 * reader_getchar(rdr);
            v += reader_getchar(rdr);
        }
    } else if(ch == 250) {
        if((ok = reader_ensure(rdr, 3))) {
            v  = reader_getchar(rdr) << 16;
            v |= reader_getchar(rdr) << 8;
            v |= reader_getchar(rdr);
        }
    } else if(ch == 251) {
        if((ok = reader_ensure(rdr, 4))) {
            v  = reader_getchar(rdr) << 24;
            v |= reader_getchar(rdr) << 16;
            v |= reader_getchar(rdr) << 8;
            v |= reader_getchar(rdr);
        }
    } else if(ch == 252) {
        if((ok = reader_ensure(rdr, 5))) {
            v  = reader_getchar(rdr) << 32;
            v |= reader_getchar(rdr) << 24;
            v |= reader_getchar(rdr) << 16;
            v |= reader_getchar(rdr) << 8;
            v |= reader_getchar(rdr);
        }
    } else if(ch == 253) {
        if((ok = reader_ensure(rdr, 6))) {
            v  = reader_getchar(rdr) << 40;
            v |= reader_getchar(rdr) << 32;
            v |= reader_getchar(rdr) << 24;
            v |= reader_getchar(rdr) << 16;
            v |= reader_getchar(rdr) << 8;
            v |= reader_getchar(rdr);
        }
    } else if(ch == 254) {
        if((ok = reader_ensure(rdr, 7))) {
            v  = reader_getchar(rdr) << 48;
            v |= reader_getchar(rdr) << 40;
            v |= reader_getchar(rdr) << 32;
            v |= reader_getchar(rdr) << 24;
            v |= reader_getchar(rdr) << 16;
            v |= reader_getchar(rdr) << 8;
            v |= reader_getchar(rdr);
        }
    } else if(ch == 255) {
        if((ok = reader_ensure(rdr, 8))) {
            v  = reader_getchar(rdr) << 56;
            v |= reader_getchar(rdr) << 48;
            v |= reader_getchar(rdr) << 40;
            v |= reader_getchar(rdr) << 32;
            v |= reader_getchar(rdr) << 24;
            v |= reader_getchar(rdr) << 16;
            v |= reader_getchar(rdr) << 8;
            v |= reader_getchar(rdr);
        }
    }
    *u64 = v;
    return ok;
}


static PyObject *read_int(struct reader *rdr, int negate)
{
    uint64_t u64;
    if(! read_plain_int(rdr, &u64)) {
        return NULL;
    }
    PyObject *v = PyLong_FromUnsignedLongLong(u64);
    if(v && negate) {
        PyObject *v2 = PyNumber_Negative(v);
        Py_DECREF(v);
        v = v2;
    }
    return v;
}


static PyObject *read_str(struct reader *rdr)
{
    struct writer wtr;
    if(! writer_init(&wtr, 20)) {
        return NULL;
    }

    uint8_t lb = 0;
    uint8_t cb;

    int shift = 1;

    int ret = reader_getc(rdr, &lb);
    if(! ret) {
        return 0;
    }
    if(! lb) {
        return writer_fini(&wtr);
    }

    while(ret && reader_getc(rdr, &cb) && (cb != 0)) {
        uint8_t ch = lb << shift;
        ch |= (cb & 0x7f) >> (7 - shift);
        ret = writer_putc(&wtr, ch);
        if(shift < 7) {
            shift++;
            lb = cb;
        } else {
            shift = 1;
            ret = reader_getc(rdr, &lb);
            if(ret && !lb) {
                break;
            }
        }
    }
    return writer_fini(&wtr);
}


static PyObject *read_time(struct reader *rdr, enum ElementKind kind)
{
    assert(0);
}


static PyObject *unpack(struct reader *rdr)
{
    PyObject *tup = PyTuple_New(TUPLE_START_SIZE);
    if(! tup) {
        return NULL;
    }

    Py_ssize_t tpos = 0;
    uint8_t ch;
    uint64_t u64;

    int go = 1;
    while(go && (rdr->pos < rdr->size)) {
        if(! reader_getc(rdr, &ch)) {
            break;
        }

        PyObject *arg = NULL;
        PyObject *tmp = NULL;

        switch(ch) {
        case KIND_NULL:
            arg = Py_None;
            Py_INCREF(arg);
            break;
        case KIND_INTEGER:
            arg = read_int(rdr, 0);
            break;
        case KIND_NEG_INTEGER:
            arg = read_int(rdr, 1);
            break;
        case KIND_BOOL:
            if(read_plain_int(rdr, &u64)) {
                arg = u64 ? Py_True : Py_False;
                Py_INCREF(arg);
            }
            break;
        case KIND_BLOB:
            arg = read_str(rdr);
            break;
        case KIND_TEXT:
            tmp = read_str(rdr);
            if(tmp) {
                arg = PyUnicode_DecodeUTF8(PyString_AS_STRING(tmp),
                                           PyString_GET_SIZE(tmp), "strict");
            }
            break;
        case KIND_NEG_TIME:
        case KIND_TIME:
            tmp = read_time(rdr, (enum ElementKind) ch);
            break;
        case KIND_UUID:
            tmp = read_str(rdr);
            if(tmp) {
                arg = PyObject_CallFunctionObjArgs(
                    (PyObject *)UUID_Type, Py_None, tmp, NULL);
            }
            break;
        case KIND_SEP:
            go = 0;
            break;
        default:
            PyErr_Format(PyExc_ValueError, "bad kind %d; key corrupt?", ch);
            Py_DECREF(tup);
            return NULL;
        }

        if(! go) {
            break;
        }

        Py_CLEAR(tmp);
        if(! arg) {
            Py_DECREF(tup);
            return NULL;
        }
        if(tpos == PyTuple_GET_SIZE(tup)) {
            if(-1 == _PyTuple_Resize(&tup, PyTuple_GET_SIZE(tup) + 2)) {
                return NULL;
            }
        }
        PyTuple_SET_ITEM(tup, tpos++, arg);
    }
    PyTuple_GET_SIZE(tup) = tpos;
    return tup;
}


static PyObject *py_unpack(PyObject *self, PyObject *args)
{
    uint8_t *prefix;
    uint8_t *s;
    Py_ssize_t s_len;
    Py_ssize_t prefix_len;

    if(! PyArg_ParseTuple(args, "s#s#", (char **) &prefix, &prefix_len,
                                        (char **) &s, &s_len)) {
        return NULL;
    }
    if(s_len < prefix_len) {
        PyErr_SetString(PyExc_ValueError,
            "unpacks() input smaller than prefix.");
        return NULL;
    }
    if(memcmp(prefix, s, prefix_len)) {
        Py_RETURN_NONE;
    }

    struct reader rdr;
    if(! reader_init(&rdr, s, s_len)) {
        return NULL;
    }
    rdr.pos += prefix_len;
    return unpack(&rdr);
}


static PyObject *unpacks(PyObject *self, PyObject *args)
{
    uint8_t *prefix;
    uint8_t *s;
    Py_ssize_t prefix_len;
    Py_ssize_t s_len;

    if(! PyArg_ParseTuple(args, "s#s#", (char **) &prefix, &prefix_len,
                                        (char **) &s, &s_len)) {
        return NULL;
    }
    if(s_len < prefix_len) {
        PyErr_SetString(PyExc_ValueError,
            "unpacks() prefix smaller than input.");
        return NULL;
    }
    if(memcmp(prefix, s, prefix_len)) {
        Py_RETURN_NONE;
    }

    struct reader rdr;
    if(! reader_init(&rdr, s, s_len)) {
        return NULL;
    }

    rdr.pos += prefix_len;
    PyObject *tups = PyList_New(LIST_START_SIZE);
    if(! tups) {
        return NULL;
    }

    Py_ssize_t lpos = 0;
    while(rdr.pos < rdr.size) {
        PyObject *tup = unpack(&rdr);
        if(! tup) {
            Py_DECREF(tups);
            return NULL;
        }

        if(lpos < LIST_START_SIZE) {
            PyList_SET_ITEM(tups, lpos++, tup);
        } else {
            if(-1 == PyList_Append(tups, tup)) {
                Py_DECREF(tups);
                Py_DECREF(tup);
                return NULL;
            }
            Py_DECREF(tup);
            lpos++;
        }
    }
    PyTuple_GET_SIZE(tups) = lpos;
    return tups;
}


static PyObject *py_decode_offsets(PyObject *self, PyObject *args)
{
    uint8_t *s;
    Py_ssize_t s_len;

    if(! PyArg_ParseTuple(args, "s#", (char **) &s, &s_len)) {
        return NULL;
    }

    struct reader rdr;
    reader_init(&rdr, s, s_len);

    uint64_t count;
    if(! read_plain_int(&rdr, &count)) {
        return NULL;
    }

    uint64_t pos = 0;
    PyObject *out = PyList_New(1 + (int) count);
    PyObject *tmp = PyInt_FromLong(0);
    if(! (out && tmp)) {
        Py_CLEAR(out);
        Py_CLEAR(tmp);
        return NULL;
    }
    PyList_SET_ITEM(out, 0, tmp);

    for(uint64_t i = 0; i < count; i++) {
        uint64_t offset;
        if(! read_plain_int(&rdr, &offset)) {
            return NULL;
        }
        pos += offset;
        tmp = PyInt_FromLong((long) pos);
        if(! tmp) {
            Py_CLEAR(out);
            return NULL;
        }
        PyList_SET_ITEM(out, 1 + i, tmp);
    }

    PyObject *tmpi = PyInt_FromLong(rdr.pos);
    tmp = PyTuple_New(2);
    if(! (tmp && tmpi)) {
        Py_CLEAR(tmp);
        Py_CLEAR(tmpi);
        return NULL;
    }
    PyTuple_SET_ITEM(tmp, 0, out);
    PyTuple_SET_ITEM(tmp, 1, tmpi);
    return tmp;
}


static struct KeyCoderModule C_API = {
    .reader_init = reader_init,
    .writer_init = writer_init,
    .writer_putc = writer_putc,
    .writer_puts = writer_puts,
    .writer_fini = writer_fini,
    .c_encode_value = c_encode_value,
    .c_encode_key = c_encode_key
};


static PyMethodDef KeyCoderMethods[] = {
    {"tuplize", tuplize, METH_O, "tuplize"},
    {"unpack", py_unpack, METH_VARARGS, "unpack"},
    {"unpacks", unpacks, METH_VARARGS, "unpacks"},
    {"pack", packs, METH_VARARGS, "pack"},
    {"packs", packs, METH_VARARGS, "packs"},
    {"pack_int", py_pack_int, METH_VARARGS, "pack_int"},
    {"decode_offsets", py_decode_offsets, METH_VARARGS, "decode_offsets"},
    {NULL, NULL, 0, NULL}
};


static PyObject *
import_object(const char *module, ...)
{
    va_list ap;
    va_start(ap, module);

    PyObject *obj = PyImport_ImportModule(module);
    if(! obj) {
        return NULL;
    }

    va_start(ap, module);
    const char *name;
    while((name = va_arg(ap, const char *)) != NULL) {
        PyObject *obj2 = PyObject_GetAttrString(obj, name);
        Py_DECREF(obj);
        if(! obj2) {
            va_end(ap);
            return NULL;
        }
        obj = obj2;
    }
    return obj;
}


PyMODINIT_FUNC
init_keycoder(void)
{
    PyDateTime_IMPORT;

    UUID_Type = (PyTypeObject *) import_object("uuid", "UUID", NULL);
    assert(PyType_CheckExact((PyObject *) UUID_Type));

    datetime_utcoffset = import_object("datetime",
        "datetime", "utcoffset", NULL);
    timedelta_total_seconds = import_object("datetime",
        "timedelta", "total_seconds", NULL);
    assert(datetime_utcoffset && timedelta_total_seconds);

    PyObject *mod = Py_InitModule("centidb._keycoder", KeyCoderMethods);
    if(! mod) {
        return;
    }

    PyObject *dct = PyModule_GetDict(mod);
    if(! dct) {
        return;
    }

    PyObject *capi = PyCapsule_New(&C_API, "centidb._keycoder._C_API", NULL);
    if(capi) {
        PyDict_SetItemString(dct, "_C_API", capi);
    }
}