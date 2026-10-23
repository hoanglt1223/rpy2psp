#include "value.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool verr_set(verr *e, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    va_end(ap);
    return false;
}

/* ---- construction and lifetime -------------------------------------------------- */

static uint32_t count_chars(const char *s, size_t len)
{
    uint32_t n = 0;
    for (size_t i = 0; i < len; i++)
        if (((uint8_t)s[i] & 0xC0) != 0x80) n++;
    return n;
}

value v_str(const char *s, size_t len)
{
    vstr *o = vn_alloc(sizeof *o + len + 1);
    o->h.refs = 1;
    o->h.type = V_STR;
    o->len = (uint32_t)len;
    if (len) memcpy(o->data, s, len);
    o->data[len] = '\0';
    o->chars = count_chars(o->data, len);
    value v = {V_STR, {0}};
    v.u.o = &o->h;
    return v;
}

value v_cstr(const char *s)
{
    return v_str(s, strlen(s));
}

value v_list_new(vtype t, uint32_t cap)
{
    vlist *o = vn_alloc(sizeof *o);
    o->h.refs = 1;
    o->h.type = (uint8_t)t;
    o->n = 0;
    o->cap = cap;
    o->items = cap ? vn_alloc(sizeof(value) * cap) : NULL;
    value v = {(uint8_t)t, {0}};
    v.u.o = &o->h;
    return v;
}

value v_dict_new(uint32_t cap)
{
    vdict *o = vn_alloc(sizeof *o);
    o->h.refs = 1;
    o->h.type = V_DICT;
    o->n = 0;
    o->cap = cap;
    o->keys = cap ? vn_alloc(sizeof(value) * cap) : NULL;
    o->vals = cap ? vn_alloc(sizeof(value) * cap) : NULL;
    value v = {V_DICT, {0}};
    v.u.o = &o->h;
    return v;
}

void v_dec(value v)
{
    if (!v_is_obj(v)) return;
    vobj *o = v.u.o;
    if (--o->refs) return;
    if (v.t == V_LIST || v.t == V_TUPLE || v.t == V_SET) {
        vlist *l = (vlist *)o;
        for (uint32_t i = 0; i < l->n; i++) v_dec(l->items[i]);
        free(l->items);
    } else if (v.t == V_DICT) {
        vdict *d = (vdict *)o;
        for (uint32_t i = 0; i < d->n; i++) {
            v_dec(d->keys[i]);
            v_dec(d->vals[i]);
        }
        free(d->keys);
        free(d->vals);
    }
    free(o);
}

void v_list_push(value list, value item)
{
    vlist *l = v_as_list(list);
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4;
        l->items = vn_realloc(l->items, sizeof(value) * l->cap);
    }
    l->items[l->n++] = item;
}

static int64_t list_find(vlist *l, value item)
{
    for (uint32_t i = 0; i < l->n; i++)
        if (v_equal(l->items[i], item)) return i;
    return -1;
}

bool v_hashable(value v)
{
    if (v.t == V_LIST || v.t == V_SET || v.t == V_DICT) return false;
    if (v.t == V_TUPLE) {
        vlist *l = v_as_list(v);
        for (uint32_t i = 0; i < l->n; i++)
            if (!v_hashable(l->items[i])) return false;
    }
    return true;
}

bool v_set_add(value set, value item, verr *e)
{
    if (!v_hashable(item)) {
        v_dec(item);
        return verr_set(e, "TypeError: unhashable type: '%s'", v_type_name(item));
    }
    if (list_find(v_as_list(set), item) >= 0) {
        v_dec(item);
        return true;
    }
    v_list_push(set, item);
    return true;
}

int64_t v_dict_find(value dict, value key)
{
    vdict *d = v_as_dict(dict);
    for (uint32_t i = 0; i < d->n; i++)
        if (v_equal(d->keys[i], key)) return i;
    return -1;
}

bool v_dict_set(value dict, value key, value val, verr *e)
{
    if (!v_hashable(key)) {
        const char *tn = v_type_name(key);
        v_dec(key);
        v_dec(val);
        return verr_set(e, "TypeError: unhashable type: '%s'", tn);
    }
    vdict *d = v_as_dict(dict);
    int64_t i = v_dict_find(dict, key);
    if (i >= 0) {
        v_dec(key);
        v_dec(d->vals[i]);
        d->vals[i] = val;
        return true;
    }
    if (d->n == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 4;
        d->keys = vn_realloc(d->keys, sizeof(value) * d->cap);
        d->vals = vn_realloc(d->vals, sizeof(value) * d->cap);
    }
    d->keys[d->n] = key;
    d->vals[d->n] = val;
    d->n++;
    return true;
}

/* ---- predicates ---------------------------------------------------------------- */

const char *v_type_name(value v)
{
    static const char *const names[] = {"unset", "NoneType", "bool", "int", "float", "str",
                                        "list", "tuple", "set", "dict"};
    return v.t < VN_ARRAY_LEN(names) ? names[v.t] : "?";
}

static bool is_num(value v)
{
    return v.t == V_BOOL || v.t == V_INT || v.t == V_FLOAT;
}

static bool is_intlike(value v)
{
    return v.t == V_BOOL || v.t == V_INT;
}

static int64_t as_int(value v)
{
    return v.t == V_BOOL ? (int64_t)v.u.b : v.u.i;
}

static double as_double(value v)
{
    return v.t == V_FLOAT ? v.u.f : (double)as_int(v);
}

bool v_truthy(value v)
{
    switch (v.t) {
    case V_BOOL: return v.u.b;
    case V_INT: return v.u.i != 0;
    case V_FLOAT: return v.u.f != 0.0;
    case V_STR: return v_as_str(v)->len != 0;
    case V_LIST: case V_TUPLE: case V_SET: return v_as_list(v)->n != 0;
    case V_DICT: return v_as_dict(v)->n != 0;
    default: return false;
    }
}

bool v_equal(value a, value b)
{
    if (is_num(a) && is_num(b)) {
        if (is_intlike(a) && is_intlike(b)) return as_int(a) == as_int(b);
        return as_double(a) == as_double(b);
    }
    if (a.t != b.t) return false;
    switch (a.t) {
    case V_NONE: case V_UNSET: return true;
    case V_STR: {
        vstr *x = v_as_str(a), *y = v_as_str(b);
        return x->len == y->len && memcmp(x->data, y->data, x->len) == 0;
    }
    case V_LIST: case V_TUPLE: {
        vlist *x = v_as_list(a), *y = v_as_list(b);
        if (x->n != y->n) return false;
        for (uint32_t i = 0; i < x->n; i++)
            if (!v_equal(x->items[i], y->items[i])) return false;
        return true;
    }
    case V_SET: {
        vlist *x = v_as_list(a), *y = v_as_list(b);
        if (x->n != y->n) return false;
        for (uint32_t i = 0; i < x->n; i++)
            if (list_find(y, x->items[i]) < 0) return false;
        return true;
    }
    case V_DICT: {
        vdict *x = v_as_dict(a);
        if (x->n != v_as_dict(b)->n) return false;
        for (uint32_t i = 0; i < x->n; i++) {
            int64_t j = v_dict_find(b, x->keys[i]);
            if (j < 0 || !v_equal(x->vals[i], v_as_dict(b)->vals[j])) return false;
        }
        return true;
    }
    default: return false;
    }
}

/* Three-way ordering for <, <=, >, >=; false with a TypeError if unorderable. */
static bool order(value a, value b, int *out, verr *e)
{
    if (is_num(a) && is_num(b)) {
        if (is_intlike(a) && is_intlike(b)) {
            int64_t x = as_int(a), y = as_int(b);
            *out = x < y ? -1 : x > y;
        } else {
            double x = as_double(a), y = as_double(b);
            if (isnan(x) || isnan(y)) {
                *out = 2;   /* every comparison with NaN is false */
                return true;
            }
            *out = x < y ? -1 : x > y;
        }
        return true;
    }
    if (a.t == V_STR && b.t == V_STR) {
        vstr *x = v_as_str(a), *y = v_as_str(b);
        int c = memcmp(x->data, y->data, x->len < y->len ? x->len : y->len);
        *out = c ? (c < 0 ? -1 : 1) : (x->len < y->len ? -1 : x->len > y->len);
        return true;
    }
    if ((a.t == V_LIST || a.t == V_TUPLE) && a.t == b.t) {
        vlist *x = v_as_list(a), *y = v_as_list(b);
        uint32_t n = x->n < y->n ? x->n : y->n;
        for (uint32_t i = 0; i < n; i++) {
            if (v_equal(x->items[i], y->items[i])) continue;
            return order(x->items[i], y->items[i], out, e);
        }
        *out = x->n < y->n ? -1 : x->n > y->n;
        return true;
    }
    return verr_set(e, "TypeError: '<' not supported between instances of '%s' and '%s'", v_type_name(a),
                    v_type_name(b));
}

static bool contains(value c, value item, bool *out, verr *e)
{
    switch (c.t) {
    case V_STR:
        if (item.t != V_STR)
            return verr_set(e, "TypeError: 'in <string>' requires string as left operand, not %s",
                            v_type_name(item));
        if (v_as_str(item)->len == 0) {
            *out = true;
            return true;
        }
        *out = strstr(v_as_str(c)->data, v_as_str(item)->data) != NULL;
        return true;
    case V_LIST: case V_TUPLE: case V_SET:
        *out = list_find(v_as_list(c), item) >= 0;
        return true;
    case V_DICT:
        *out = v_dict_find(c, item) >= 0;
        return true;
    default:
        return verr_set(e, "TypeError: argument of type '%s' is not iterable", v_type_name(c));
    }
}

bool v_compare(int op, value a, value b, value *out, verr *e)
{
    bool r;
    int c;
    switch (op) {
    case CMP_EQ: r = v_equal(a, b); break;
    case CMP_NE: r = !v_equal(a, b); break;
    case CMP_LT: case CMP_LE: case CMP_GT: case CMP_GE:
        if (!order(a, b, &c, e)) return false;
        if (c == 2)
            r = false;
        else
            r = op == CMP_LT ? c < 0 : op == CMP_LE ? c <= 0 : op == CMP_GT ? c > 0 : c >= 0;
        break;
    case CMP_IN: case CMP_NOTIN:
        if (!contains(b, a, &r, e)) return false;
        if (op == CMP_NOTIN) r = !r;
        break;
    case CMP_IS: case CMP_ISNOT:
        if (a.t != b.t)
            r = false;
        else if (v_is_obj(a))
            r = a.u.o == b.u.o;
        else
            r = v_equal(a, b);
        if (op == CMP_ISNOT) r = !r;
        break;
    default: return verr_set(e, "bad compare op %d", op);
    }
    *out = v_bool(r);
    return true;
}

/* ---- arithmetic ------------------------------------------------------------------ */

static bool int_result(int64_t r, bool overflow, value *out, verr *e)
{
    if (overflow) return verr_set(e, "OverflowError: integer result does not fit in 64 bits");
    *out = v_int(r);
    return true;
}

static int64_t floordiv64(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

static int64_t mod64(int64_t a, int64_t b)
{
    int64_t m = a % b;
    if (m != 0 && ((m < 0) != (b < 0))) m += b;
    return m;
}

static value seq_concat(value a, value b)
{
    vlist *x = v_as_list(a), *y = v_as_list(b);
    value r = v_list_new((vtype)a.t, x->n + y->n);
    for (uint32_t i = 0; i < x->n; i++) v_list_push(r, v_inc(x->items[i]));
    for (uint32_t i = 0; i < y->n; i++) v_list_push(r, v_inc(y->items[i]));
    return r;
}

static bool repeat(value seq, int64_t n, value *out)
{
    if (n < 0) n = 0;
    if (seq.t == V_STR) {
        vstr *s = v_as_str(seq);
        if (n && s->len > (1u << 24) / (uint64_t)n) return false;
        vn_buf b = {0};
        for (int64_t i = 0; i < n; i++) vn_buf_put(&b, s->data, s->len);
        *out = v_str((const char *)b.data, b.len);
        vn_buf_free(&b);
        return true;
    }
    vlist *l = v_as_list(seq);
    if (n && l->n > (1u << 20) / (uint64_t)n) return false;
    value r = v_list_new((vtype)seq.t, (uint32_t)(l->n * n));
    for (int64_t k = 0; k < n; k++)
        for (uint32_t i = 0; i < l->n; i++) v_list_push(r, v_inc(l->items[i]));
    *out = r;
    return true;
}

static bool percent_format(vstr *fmt, value args, value *out, verr *e);

bool v_binary(int op, value a, value b, value *out, verr *e)
{
    if (is_num(a) && is_num(b)) {
        if (is_intlike(a) && is_intlike(b) && op != BIN_DIV) {
            int64_t x = as_int(a), y = as_int(b), r = 0;
            switch (op) {
            case BIN_ADD: r = (int64_t)((uint64_t)x + (uint64_t)y);
                return int_result(r, (x > 0 && y > 0 && r < 0) || (x < 0 && y < 0 && r >= 0), out, e);
            case BIN_SUB: r = (int64_t)((uint64_t)x - (uint64_t)y);
                return int_result(r, (x >= 0 && y < 0 && r < 0) || (x < 0 && y > 0 && r >= 0), out, e);
            case BIN_MUL:
                if (x != 0 && (llabs(x) > INT64_MAX / (llabs(y) ? llabs(y) : 1) || x == INT64_MIN || y == INT64_MIN))
                    return int_result(0, true, out, e);
                return int_result(x * y, false, out, e);
            case BIN_FLOORDIV:
                if (y == 0) return verr_set(e, "ZeroDivisionError: integer division or modulo by zero");
                return int_result(floordiv64(x, y), x == INT64_MIN && y == -1, out, e);
            case BIN_MOD:
                if (y == 0) return verr_set(e, "ZeroDivisionError: integer division or modulo by zero");
                return int_result(y == -1 ? 0 : mod64(x, y), false, out, e);
            }
        }
        double x = as_double(a), y = as_double(b);
        switch (op) {
        case BIN_ADD: *out = v_float(x + y); return true;
        case BIN_SUB: *out = v_float(x - y); return true;
        case BIN_MUL: *out = v_float(x * y); return true;
        case BIN_DIV:
            if (y == 0.0) return verr_set(e, "ZeroDivisionError: division by zero");
            *out = v_float(x / y);
            return true;
        case BIN_FLOORDIV:
            if (y == 0.0) return verr_set(e, "ZeroDivisionError: float floor division by zero");
            *out = v_float(floor(x / y));
            return true;
        case BIN_MOD: {
            if (y == 0.0) return verr_set(e, "ZeroDivisionError: float modulo");
            double m = fmod(x, y);
            if (m != 0.0 && ((m < 0) != (y < 0))) m += y;
            *out = v_float(m);
            return true;
        }
        }
    }
    if (op == BIN_ADD) {
        if (a.t == V_STR && b.t == V_STR) {
            vstr *x = v_as_str(a), *y = v_as_str(b);
            vn_buf buf = {0};
            vn_buf_put(&buf, x->data, x->len);
            vn_buf_put(&buf, y->data, y->len);
            *out = v_str((const char *)buf.data, buf.len);
            vn_buf_free(&buf);
            return true;
        }
        if ((a.t == V_LIST || a.t == V_TUPLE) && a.t == b.t) {
            *out = seq_concat(a, b);
            return true;
        }
    }
    if (op == BIN_SUB && a.t == V_SET && b.t == V_SET) {
        vlist *x = v_as_list(a);
        value r = v_list_new(V_SET, x->n);
        for (uint32_t i = 0; i < x->n; i++)
            if (list_find(v_as_list(b), x->items[i]) < 0) v_list_push(r, v_inc(x->items[i]));
        *out = r;
        return true;
    }
    if (op == BIN_MUL) {
        value seq = a, n = b;
        if (is_intlike(a)) {
            seq = b;
            n = a;
        }
        if (is_intlike(n) && (seq.t == V_STR || seq.t == V_LIST || seq.t == V_TUPLE)) {
            if (!repeat(seq, as_int(n), out)) return verr_set(e, "MemoryError: repeated sequence too large");
            return true;
        }
    }
    if (op == BIN_MOD && a.t == V_STR) return percent_format(v_as_str(a), b, out, e);
    static const char *const sym[] = {"+", "-", "*", "/", "//", "%"};
    return verr_set(e, "TypeError: unsupported operand type(s) for %s: '%s' and '%s'", op >= 0 && op < 6 ? sym[op] : "?",
                    v_type_name(a), v_type_name(b));
}

bool v_unary(int op, value a, value *out, verr *e)
{
    if (op == UN_NOT) {
        *out = v_bool(!v_truthy(a));
        return true;
    }
    if (!is_num(a)) return verr_set(e, "TypeError: bad operand type for unary %s: '%s'", op == UN_NEG ? "-" : "+",
                                    v_type_name(a));
    if (a.t == V_FLOAT) {
        *out = v_float(op == UN_NEG ? -a.u.f : a.u.f);
        return true;
    }
    int64_t x = as_int(a);
    if (op == UN_NEG && x == INT64_MIN) return int_result(0, true, out, e);
    *out = v_int(op == UN_NEG ? -x : x);
    return true;
}

/* ---- strings: code point indexing ----------------------------------------------- */

static size_t char_offset(vstr *s, int64_t index)
{
    if (s->chars == s->len) return (size_t)index;
    size_t pos = 0;
    for (int64_t k = 0; k < index && pos < s->len; k++) vn_utf8_next(s->data, s->len, &pos);
    return pos;
}

static bool norm_index(value k, int64_t n, int64_t *out, const char *what, verr *e)
{
    if (!is_intlike(k)) return verr_set(e, "TypeError: %s indices must be integers, not %s", what, v_type_name(k));
    int64_t i = as_int(k);
    if (i < 0) i += n;
    if (i < 0 || i >= n) return verr_set(e, "IndexError: %s index out of range", what);
    *out = i;
    return true;
}

bool v_subscr(value c, value k, value *out, verr *e)
{
    int64_t i;
    switch (c.t) {
    case V_LIST: case V_TUPLE:
        if (!norm_index(k, v_as_list(c)->n, &i, c.t == V_LIST ? "list" : "tuple", e)) return false;
        *out = v_inc(v_as_list(c)->items[i]);
        return true;
    case V_STR: {
        vstr *s = v_as_str(c);
        if (!norm_index(k, s->chars, &i, "string", e)) return false;
        size_t a = char_offset(s, i), b = a;
        vn_utf8_next(s->data, s->len, &b);
        *out = v_str(s->data + a, b - a);
        return true;
    }
    case V_DICT: {
        int64_t j = v_dict_find(c, k);
        if (j < 0) {
            vn_buf b = {0};
            v_repr(k, &b);
            vn_buf_u8(&b, 0);
            verr_set(e, "KeyError: %s", (const char *)b.data);
            vn_buf_free(&b);
            return false;
        }
        *out = v_inc(v_as_dict(c)->vals[j]);
        return true;
    }
    default:
        return verr_set(e, "TypeError: '%s' object is not subscriptable", v_type_name(c));
    }
}

static bool slice_bound(value v, int64_t n, int64_t dflt, int64_t *out, verr *e)
{
    if (v.t == V_NONE) {
        *out = dflt;
        return true;
    }
    if (!is_intlike(v)) return verr_set(e, "TypeError: slice indices must be integers or None");
    int64_t i = as_int(v);
    if (i < 0) i += n;
    *out = i < 0 ? 0 : i > n ? n : i;
    return true;
}

bool v_slice(value c, value lo, value hi, value *out, verr *e)
{
    int64_t n;
    if (c.t == V_STR)
        n = v_as_str(c)->chars;
    else if (c.t == V_LIST || c.t == V_TUPLE)
        n = v_as_list(c)->n;
    else
        return verr_set(e, "TypeError: '%s' object is not subscriptable", v_type_name(c));
    int64_t a, b;
    if (!slice_bound(lo, n, 0, &a, e) || !slice_bound(hi, n, n, &b, e)) return false;
    if (b < a) b = a;
    if (c.t == V_STR) {
        vstr *s = v_as_str(c);
        size_t x = char_offset(s, a), y = char_offset(s, b);
        *out = v_str(s->data + x, y - x);
        return true;
    }
    vlist *l = v_as_list(c);
    value r = v_list_new((vtype)c.t, (uint32_t)(b - a));
    for (int64_t i = a; i < b; i++) v_list_push(r, v_inc(l->items[i]));
    *out = r;
    return true;
}

bool v_store_subscr(value c, value k, value v, verr *e)
{
    if (c.t == V_LIST) {
        int64_t i;
        if (!norm_index(k, v_as_list(c)->n, &i, "list assignment", e)) return false;
        v_dec(v_as_list(c)->items[i]);
        v_as_list(c)->items[i] = v_inc(v);
        return true;
    }
    if (c.t == V_DICT) return v_dict_set(c, v_inc(k), v_inc(v), e);
    return verr_set(e, "TypeError: '%s' object does not support item assignment", v_type_name(c));
}

/* ---- str() and repr() ------------------------------------------------------------- */

static void float_repr(double x, vn_buf *b)
{
    if (isnan(x)) {
        vn_buf_put(b, "nan", 3);
        return;
    }
    if (isinf(x)) {
        if (x < 0)
            vn_buf_put(b, "-inf", 4);
        else
            vn_buf_put(b, "inf", 3);
        return;
    }
    char tmp[40];
    int prec = 1;
    for (; prec <= 17; prec++) {
        snprintf(tmp, sizeof tmp, "%.*e", prec - 1, x);
        if (strtod(tmp, NULL) == x) break;
    }
    /* tmp is "[-]d[.ddd]e[+-]XX": collect the digits and the decimal exponent. */
    char digits[24];
    int nd = 0, exp10 = 0;
    bool neg = false;
    const char *p = tmp;
    if (*p == '-') {
        neg = true;
        p++;
    }
    for (; *p && *p != 'e'; p++)
        if (*p >= '0' && *p <= '9') digits[nd++] = *p;
    if (*p == 'e') exp10 = atoi(p + 1);
    while (nd > 1 && digits[nd - 1] == '0') nd--;
    if (neg) vn_buf_u8(b, '-');
    if (exp10 >= -5 + 1 && exp10 < 16) {
        if (exp10 < 0) {
            vn_buf_put(b, "0.", 2);
            for (int i = 0; i < -exp10 - 1; i++) vn_buf_u8(b, '0');
            vn_buf_put(b, digits, (size_t)nd);
        } else {
            for (int i = 0; i <= exp10; i++) vn_buf_u8(b, i < nd ? (uint8_t)digits[i] : '0');
            vn_buf_u8(b, '.');
            if (nd > exp10 + 1)
                vn_buf_put(b, digits + exp10 + 1, (size_t)(nd - exp10 - 1));
            else
                vn_buf_u8(b, '0');
        }
    } else {
        vn_buf_u8(b, (uint8_t)digits[0]);
        if (nd > 1) {
            vn_buf_u8(b, '.');
            vn_buf_put(b, digits + 1, (size_t)(nd - 1));
        }
        char e[8];
        int n = snprintf(e, sizeof e, "e%c%02d", exp10 < 0 ? '-' : '+', exp10 < 0 ? -exp10 : exp10);
        vn_buf_put(b, e, (size_t)n);
    }
}

static void str_repr(vstr *s, vn_buf *b)
{
    bool has_sq = memchr(s->data, '\'', s->len) != NULL, has_dq = memchr(s->data, '"', s->len) != NULL;
    char q = (has_sq && !has_dq) ? '"' : '\'';
    vn_buf_u8(b, (uint8_t)q);
    size_t pos = 0;
    while (pos < s->len) {
        size_t start = pos;
        uint32_t cp = vn_utf8_next(s->data, s->len, &pos);
        char esc[8];
        if (cp == (uint32_t)q || cp == '\\') {
            vn_buf_u8(b, '\\');
            vn_buf_u8(b, (uint8_t)cp);
        } else if (cp == '\n') {
            vn_buf_put(b, "\\n", 2);
        } else if (cp == '\r') {
            vn_buf_put(b, "\\r", 2);
        } else if (cp == '\t') {
            vn_buf_put(b, "\\t", 2);
        } else if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) {
            int n = snprintf(esc, sizeof esc, "\\x%02x", cp);
            vn_buf_put(b, esc, (size_t)n);
        } else {
            vn_buf_put(b, s->data + start, pos - start);
        }
    }
    vn_buf_u8(b, (uint8_t)q);
}

void v_repr(value v, vn_buf *b)
{
    char tmp[32];
    switch (v.t) {
    case V_STR: str_repr(v_as_str(v), b); return;
    case V_LIST: case V_TUPLE: case V_SET: {
        vlist *l = v_as_list(v);
        if (v.t == V_SET && l->n == 0) {
            vn_buf_put(b, "set()", 5);
            return;
        }
        vn_buf_u8(b, v.t == V_LIST ? '[' : v.t == V_TUPLE ? '(' : '{');
        for (uint32_t i = 0; i < l->n; i++) {
            if (i) vn_buf_put(b, ", ", 2);
            v_repr(l->items[i], b);
        }
        if (v.t == V_TUPLE && l->n == 1) vn_buf_u8(b, ',');
        vn_buf_u8(b, v.t == V_LIST ? ']' : v.t == V_TUPLE ? ')' : '}');
        return;
    }
    case V_DICT: {
        vdict *d = v_as_dict(v);
        vn_buf_u8(b, '{');
        for (uint32_t i = 0; i < d->n; i++) {
            if (i) vn_buf_put(b, ", ", 2);
            v_repr(d->keys[i], b);
            vn_buf_put(b, ": ", 2);
            v_repr(d->vals[i], b);
        }
        vn_buf_u8(b, '}');
        return;
    }
    case V_NONE: case V_UNSET: vn_buf_put(b, "None", 4); return;
    case V_BOOL:
        if (v.u.b)
            vn_buf_put(b, "True", 4);
        else
            vn_buf_put(b, "False", 5);
        return;
    case V_INT: {
        int n = snprintf(tmp, sizeof tmp, "%lld", (long long)v.u.i);
        vn_buf_put(b, tmp, (size_t)n);
        return;
    }
    case V_FLOAT: float_repr(v.u.f, b); return;
    }
}

void v_to_str(value v, vn_buf *b)
{
    if (v.t == V_STR) {
        vn_buf_put(b, v_as_str(v)->data, v_as_str(v)->len);
        return;
    }
    v_repr(v, b);
}

value v_str_of(value v)
{
    if (v.t == V_STR) return v_inc(v);
    vn_buf b = {0};
    v_to_str(v, &b);
    value r = v_str((const char *)b.data, b.len);
    vn_buf_free(&b);
    return r;
}

/* ---- formatting --------------------------------------------------------------------- */

static void pad_put(vn_buf *b, const char *s, size_t len, size_t width, char align, const char *fill, size_t filllen,
                    uint32_t nchars)
{
    size_t pad = width > nchars ? width - nchars : 0;
    size_t left = align == '<' ? 0 : align == '^' ? pad / 2 : pad;
    for (size_t i = 0; i < left; i++) vn_buf_put(b, fill, filllen);
    vn_buf_put(b, s, len);
    for (size_t i = 0; i < pad - left; i++) vn_buf_put(b, fill, filllen);
}

static void group_thousands(vn_buf *num)
{
    /* Inserts ',' into the integer part of the formatted number in place. */
    size_t start = (num->len && (num->data[0] == '-' || num->data[0] == '+')) ? 1 : 0, end = start;
    while (end < num->len && num->data[end] >= '0' && num->data[end] <= '9') end++;
    size_t digits = end - start;
    if (digits <= 3) return;
    vn_buf out = {0};
    vn_buf_put(&out, num->data, start);
    for (size_t i = 0; i < digits; i++) {
        if (i && (digits - i) % 3 == 0) vn_buf_u8(&out, ',');
        vn_buf_u8(&out, num->data[start + i]);
    }
    vn_buf_put(&out, num->data + end, num->len - end);
    vn_buf_free(num);
    *num = out;
}

bool v_format(value v, const char *spec, size_t n, vn_buf *b, verr *e)
{
    if (n == 0) {
        v_to_str(v, b);
        return true;
    }
    const char *fill = " ";
    size_t filllen = 1, i = 0;
    char align = 0, sign = '-', type = 0;
    bool zero = false, comma = false;
    int width = 0, prec = -1;
    /* [[fill]align] where fill may be any character */
    if (n >= 1) {
        size_t p = 0;
        vn_utf8_next(spec, n, &p);
        if (p < n && strchr("<>^=", spec[p])) {
            fill = spec;
            filllen = p;
            align = spec[p];
            i = p + 1;
        } else if (strchr("<>^=", spec[0])) {
            align = spec[0];
            i = 1;
        }
    }
    if (i < n && strchr("+- ", spec[i])) sign = spec[i++];
    if (i < n && spec[i] == '0') {
        zero = true;
        i++;
    }
    while (i < n && spec[i] >= '0' && spec[i] <= '9') width = width * 10 + (spec[i++] - '0');
    if (i < n && spec[i] == ',') {
        comma = true;
        i++;
    }
    if (i < n && spec[i] == '.') {
        i++;
        prec = 0;
        while (i < n && spec[i] >= '0' && spec[i] <= '9') prec = prec * 10 + (spec[i++] - '0');
    }
    if (i < n) type = spec[i++];
    if (i != n || width > 1000 || prec > 100) return verr_set(e, "ValueError: invalid format spec '%.*s'", (int)n, spec);

    vn_buf body = {0};
    bool numeric = is_num(v);
    char tmp[400];
    if (v.t == V_STR || (!numeric && (type == 0 || type == 's'))) {
        if (type && type != 's') {
            vn_buf_free(&body);
            return verr_set(e, "ValueError: unknown format code '%c' for object of type '%s'", type, v_type_name(v));
        }
        v_to_str(v, &body);
        if (prec >= 0 && body.len) {
            size_t pos = 0;
            for (int k = 0; k < prec && pos < body.len; k++) vn_utf8_next((const char *)body.data, body.len, &pos);
            body.len = pos;
        }
        if (!align) align = '<';
    } else if (numeric) {
        double x = as_double(v);
        if (type == 0 && prec < 0) {
            v_to_str(v.t == V_BOOL ? v_int(v.u.b) : v, &body);
        } else if (type == 'd' || (type == 0 && is_intlike(v))) {
            if (!is_intlike(v)) {
                vn_buf_free(&body);
                return verr_set(e, "ValueError: unknown format code 'd' for object of type 'float'");
            }
            int k = snprintf(tmp, sizeof tmp, "%lld", (long long)as_int(v));
            vn_buf_put(&body, tmp, (size_t)k);
        } else if (type == 'x' || type == 'X' || type == 'o' || type == 'b') {
            if (!is_intlike(v)) {
                vn_buf_free(&body);
                return verr_set(e, "ValueError: unknown format code '%c' for object of type 'float'", type);
            }
            int64_t iv = as_int(v);
            uint64_t u = iv < 0 ? (uint64_t)(-(iv + 1)) + 1 : (uint64_t)iv;
            if (iv < 0) vn_buf_u8(&body, '-');
            if (type == 'b') {
                char bits[65];
                int k = 64;
                bits[k] = 0;
                do {
                    bits[--k] = (char)('0' + (u & 1));
                    u >>= 1;
                } while (u);
                vn_buf_put(&body, bits + k, (size_t)(64 - k));
            } else {
                int k = snprintf(tmp, sizeof tmp, type == 'x' ? "%llx" : type == 'X' ? "%llX" : "%llo",
                                 (unsigned long long)u);
                vn_buf_put(&body, tmp, (size_t)k);
            }
        } else if (type == 'f' || type == 'F' || type == 'e' || type == 'E' || type == '%' || type == 'g' ||
                   type == 'G' || type == 0) {
            int p = prec < 0 ? 6 : prec;
            int k;
            if (type == '%')
                k = snprintf(tmp, sizeof tmp, "%.*f%%", p, x * 100.0);
            else if (type == 0)
                k = snprintf(tmp, sizeof tmp, "%.*g", p ? p : 1, x);
            else {
                char f[8] = {'%', '.', '*', type, 0};
                k = snprintf(tmp, sizeof tmp, f, p, x);
            }
            vn_buf_put(&body, tmp, (size_t)(k > 0 && (size_t)k < sizeof tmp ? k : 0));
        } else {
            vn_buf_free(&body);
            return verr_set(e, "ValueError: unknown format code '%c' for object of type '%s'", type, v_type_name(v));
        }
        if (comma) group_thousands(&body);
        if (sign != '-' && body.len && body.data[0] != '-') {
            vn_buf signed_body = {0};
            vn_buf_u8(&signed_body, (uint8_t)sign);
            vn_buf_put(&signed_body, body.data, body.len);
            vn_buf_free(&body);
            body = signed_body;
        }
        if (!align) align = zero ? '=' : '>';
        if (zero && align == '=') {
            fill = "0";
            filllen = 1;
        }
    } else {
        vn_buf_free(&body);
        return verr_set(e, "TypeError: unsupported format string passed to %s.__format__", v_type_name(v));
    }
    uint32_t nchars = count_chars((const char *)body.data, body.len);
    if (align == '=') {
        size_t signlen = body.len && (body.data[0] == '-' || body.data[0] == '+' || body.data[0] == ' ') ? 1 : 0;
        vn_buf_put(b, body.data, signlen);
        pad_put(b, (const char *)body.data + signlen, body.len - signlen, width > (int)signlen ? (size_t)width - signlen : 0,
                '>', fill, filllen, nchars - (uint32_t)signlen);
    } else {
        pad_put(b, (const char *)body.data, body.len, (size_t)width, align, fill, filllen, nchars);
    }
    vn_buf_free(&body);
    return true;
}

/* str % args: %s %r %d %i %f %e %g %x %% with flags, width and precision. */
static bool percent_format(vstr *fmt, value args, value *out, verr *e)
{
    value *items = &args;
    uint32_t nitems = 1, used = 0;
    if (args.t == V_TUPLE) {
        items = v_as_list(args)->items;
        nitems = v_as_list(args)->n;
    }
    vn_buf b = {0};
    const char *s = fmt->data;
    size_t n = fmt->len;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '%') {
            vn_buf_u8(&b, (uint8_t)s[i]);
            continue;
        }
        if (++i >= n) goto bad;
        char flags[8];
        int nf = 0;
        while (i < n && strchr("-+ 0#", s[i]) && nf < 6) flags[nf++] = s[i++];
        flags[nf] = 0;
        int width = 0, prec = -1;
        while (i < n && s[i] >= '0' && s[i] <= '9') width = width * 10 + (s[i++] - '0');
        if (i < n && s[i] == '.') {
            i++;
            prec = 0;
            while (i < n && s[i] >= '0' && s[i] <= '9') prec = prec * 10 + (s[i++] - '0');
        }
        if (i >= n || width > 1000 || prec > 100) goto bad;
        char c = s[i];
        if (c == '%') {
            vn_buf_u8(&b, '%');
            continue;
        }
        if (used >= nitems) {
            vn_buf_free(&b);
            return verr_set(e, "TypeError: not enough arguments for format string");
        }
        value a = items[used++];
        char spec[24], tmp[400];
        if (c == 's' || c == 'r' || c == 'a') {
            vn_buf sb = {0};
            if (c == 's')
                v_to_str(a, &sb);
            else
                v_repr(a, &sb);
            size_t len = sb.len;
            if (prec >= 0) {
                size_t pos = 0;
                for (int k = 0; k < prec && pos < sb.len; k++) vn_utf8_next((const char *)sb.data, sb.len, &pos);
                len = pos;
            }
            uint32_t chars = count_chars((const char *)sb.data, len);
            pad_put(&b, (const char *)sb.data, len, (size_t)width, strchr(flags, '-') ? '<' : '>', " ", 1, chars);
            vn_buf_free(&sb);
        } else if (strchr("diouxXeEfFgG", c)) {
            if (!is_num(a)) {
                vn_buf_free(&b);
                return verr_set(e, "TypeError: %%%c format: a real number is required, not %s", c, v_type_name(a));
            }
            int k;
            if (strchr("diouxX", c)) {
                long long iv = a.t == V_FLOAT ? (long long)a.u.f : (long long)as_int(a);
                char conv = c == 'i' || c == 'u' ? 'd' : c;
                snprintf(spec, sizeof spec, "%%%s%d%sll%c", flags, width, prec >= 0 ? ".*" : "", conv);
                k = prec >= 0 ? snprintf(tmp, sizeof tmp, spec, prec, iv) : snprintf(tmp, sizeof tmp, spec, iv);
            } else {
                snprintf(spec, sizeof spec, "%%%s%d.*%c", flags, width, c);
                k = snprintf(tmp, sizeof tmp, spec, prec < 0 ? 6 : prec, as_double(a));
            }
            if (k > 0 && (size_t)k < sizeof tmp) vn_buf_put(&b, tmp, (size_t)k);
        } else {
            goto bad;
        }
    }
    if (used < nitems && args.t == V_TUPLE) {
        vn_buf_free(&b);
        return verr_set(e, "TypeError: not all arguments converted during string formatting");
    }
    *out = v_str((const char *)b.data, b.len);
    vn_buf_free(&b);
    return true;
bad:
    vn_buf_free(&b);
    return verr_set(e, "ValueError: unsupported format character in '%s'", fmt->data);
}

/* ---- iteration and builtins ---------------------------------------------------------- */

bool v_iter_items(value v, value **items, uint32_t *n, verr *e)
{
    *items = NULL;
    *n = 0;
    switch (v.t) {
    case V_LIST: case V_TUPLE: case V_SET: {
        vlist *l = v_as_list(v);
        *items = vn_alloc(sizeof(value) * (l->n ? l->n : 1));
        for (uint32_t i = 0; i < l->n; i++) (*items)[i] = v_inc(l->items[i]);
        *n = l->n;
        return true;
    }
    case V_DICT: {
        vdict *d = v_as_dict(v);
        *items = vn_alloc(sizeof(value) * (d->n ? d->n : 1));
        for (uint32_t i = 0; i < d->n; i++) (*items)[i] = v_inc(d->keys[i]);
        *n = d->n;
        return true;
    }
    case V_STR: {
        vstr *s = v_as_str(v);
        *items = vn_alloc(sizeof(value) * (s->chars ? s->chars : 1));
        size_t pos = 0;
        while (pos < s->len) {
            size_t a = pos;
            vn_utf8_next(s->data, s->len, &pos);
            (*items)[(*n)++] = v_str(s->data + a, pos - a);
        }
        return true;
    }
    default:
        return verr_set(e, "TypeError: '%s' object is not iterable", v_type_name(v));
    }
}

static void free_items(value *items, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) v_dec(items[i]);
    free(items);
}

static bool from_iterable(vtype t, value *args, int argc, value *out, verr *e)
{
    if (argc == 0) {
        *out = v_list_new(t, 0);
        return true;
    }
    value *items;
    uint32_t n;
    if (!v_iter_items(args[0], &items, &n, e)) return false;
    value r = v_list_new(t, n);
    bool ok = true;
    for (uint32_t i = 0; i < n; i++) {
        if (t == V_SET) {
            if (ok && !v_set_add(r, items[i], e)) ok = false;
            else if (!ok) v_dec(items[i]);
        } else {
            v_list_push(r, items[i]);
        }
    }
    free(items);
    if (!ok) {
        v_dec(r);
        return false;
    }
    *out = r;
    return true;
}

static bool is_space(uint32_t c)
{
    return c == ' ' || (c >= 9 && c <= 13) || c == 0x1C || c == 0x1D || c == 0x1E || c == 0x1F || c == 0x85 ||
           c == 0xA0 || c == 0x3000 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F ||
           c == 0x205F || c == 0x1680;
}

static bool parse_int(vstr *s, int64_t *out)
{
    const char *p = s->data, *end = s->data + s->len;
    while (p < end && isspace((unsigned char)*p)) p++;
    while (end > p && isspace((unsigned char)end[-1])) end--;
    bool neg = false;
    if (p < end && (*p == '+' || *p == '-')) neg = *p++ == '-';
    if (p == end) return false;
    uint64_t v = 0;
    bool last_us = true;
    for (; p < end; p++) {
        if (*p == '_' && !last_us) {
            last_us = true;
            continue;
        }
        if (*p < '0' || *p > '9') return false;
        if (v > (UINT64_C(1) << 63) / 10) return false;
        v = v * 10 + (uint64_t)(*p - '0');
        last_us = false;
    }
    if (last_us) return false;
    if (v > (UINT64_C(1) << 63) - (neg ? 0 : 1)) return false;
    *out = neg ? (int64_t)(0 - v) : (int64_t)v;
    return true;
}

static bool parse_float(vstr *s, double *out)
{
    const char *p = s->data, *end = s->data + s->len;
    while (p < end && isspace((unsigned char)*p)) p++;
    while (end > p && isspace((unsigned char)end[-1])) end--;
    size_t n = (size_t)(end - p);
    if (n == 0 || n > 300) return false;
    char tmp[320];
    memcpy(tmp, p, n);
    tmp[n] = 0;
    const char *t = tmp + ((tmp[0] == '+' || tmp[0] == '-') ? 1 : 0);
    double sign = tmp[0] == '-' ? -1.0 : 1.0;
#ifdef _MSC_VER
#define strcasecmp _stricmp
#endif
    if (strcasecmp(t, "inf") == 0 || strcasecmp(t, "infinity") == 0) {
        *out = sign * INFINITY;
        return true;
    }
    if (strcasecmp(t, "nan") == 0) {
        *out = NAN;
        return true;
    }
    for (const char *c = t; *c; c++)
        if (!strchr("0123456789.eE+-_", *c)) return false;
    char *stop;
    *out = strtod(tmp, &stop);
    return *stop == 0;
}

static double round_half_even(double x)
{
    double r = floor(x + 0.5);
    if (r - x == 0.5 && fmod(r, 2.0) != 0.0) r -= 1.0;
    return r;
}

bool v_call_pure(int fn, value *args, int argc, value *out, verr *e)
{
    switch (fn) {
    case FN_LEN: {
        value a = args[0];
        if (a.t == V_STR)
            *out = v_int(v_as_str(a)->chars);
        else if (a.t == V_LIST || a.t == V_TUPLE || a.t == V_SET)
            *out = v_int(v_as_list(a)->n);
        else if (a.t == V_DICT)
            *out = v_int(v_as_dict(a)->n);
        else
            return verr_set(e, "TypeError: object of type '%s' has no len()", v_type_name(a));
        return true;
    }
    case FN_STR: *out = argc ? v_str_of(args[0]) : v_str("", 0); return true;
    case FN_INT: {
        if (!argc) {
            *out = v_int(0);
            return true;
        }
        value a = args[0];
        if (is_intlike(a)) {
            *out = v_int(as_int(a));
            return true;
        }
        if (a.t == V_FLOAT) {
            if (!isfinite(a.u.f) || fabs(a.u.f) >= 9.2e18)
                return verr_set(e, "OverflowError: cannot convert float to integer");
            *out = v_int((int64_t)trunc(a.u.f));
            return true;
        }
        int64_t iv;
        if (a.t == V_STR && parse_int(v_as_str(a), &iv)) {
            *out = v_int(iv);
            return true;
        }
        if (a.t == V_STR) return verr_set(e, "ValueError: invalid literal for int() with base 10: '%s'", v_as_str(a)->data);
        return verr_set(e, "TypeError: int() argument must be a string or a number, not '%s'", v_type_name(a));
    }
    case FN_FLOAT: {
        if (!argc) {
            *out = v_float(0.0);
            return true;
        }
        value a = args[0];
        double d;
        if (is_num(a)) {
            *out = v_float(as_double(a));
            return true;
        }
        if (a.t == V_STR && parse_float(v_as_str(a), &d)) {
            *out = v_float(d);
            return true;
        }
        if (a.t == V_STR) return verr_set(e, "ValueError: could not convert string to float: '%s'", v_as_str(a)->data);
        return verr_set(e, "TypeError: float() argument must be a string or a number, not '%s'", v_type_name(a));
    }
    case FN_BOOL: *out = v_bool(argc ? v_truthy(args[0]) : false); return true;
    case FN_MIN: case FN_MAX: {
        value *items = args;
        uint32_t n = (uint32_t)argc;
        bool owned = false;
        if (argc == 1) {
            if (!v_iter_items(args[0], &items, &n, e)) return false;
            owned = true;
        }
        if (n == 0) {
            if (owned) free_items(items, n);
            return verr_set(e, "ValueError: %s() arg is an empty sequence", fn == FN_MIN ? "min" : "max");
        }
        uint32_t best = 0;
        for (uint32_t i = 1; i < n; i++) {
            int c;
            if (!order(items[i], items[best], &c, e)) {
                if (owned) free_items(items, n);
                return false;
            }
            if (c != 2 && (fn == FN_MIN ? c < 0 : c > 0)) best = i;
        }
        *out = v_inc(items[best]);
        if (owned) free_items(items, n);
        return true;
    }
    case FN_ABS: {
        value a = args[0];
        if (a.t == V_FLOAT) {
            *out = v_float(fabs(a.u.f));
            return true;
        }
        if (is_intlike(a)) {
            int64_t x = as_int(a);
            if (x == INT64_MIN) return int_result(0, true, out, e);
            *out = v_int(x < 0 ? -x : x);
            return true;
        }
        return verr_set(e, "TypeError: bad operand type for abs(): '%s'", v_type_name(a));
    }
    case FN_ROUND: {
        value a = args[0];
        if (!is_num(a)) return verr_set(e, "TypeError: type %s doesn't define __round__ method", v_type_name(a));
        if (argc == 1 || args[1].t == V_NONE) {
            if (is_intlike(a)) {
                *out = v_int(as_int(a));
                return true;
            }
            if (!isfinite(a.u.f) || fabs(a.u.f) >= 9.2e18)
                return verr_set(e, "OverflowError: cannot convert float to integer");
            *out = v_int((int64_t)round_half_even(a.u.f));
            return true;
        }
        if (!is_intlike(args[1])) return verr_set(e, "TypeError: round() ndigits must be an integer");
        int64_t nd = as_int(args[1]);
        if (is_intlike(a)) {
            *out = v_int(as_int(a));   /* ndigits >= 0 keeps an int unchanged */
            return true;
        }
        if (nd < 0 || nd > 30 || !isfinite(a.u.f)) {
            double p = pow(10.0, (double)-nd);
            *out = v_float(isfinite(a.u.f) && nd < 0 ? round_half_even(a.u.f / p) * p : a.u.f);
            return true;
        }
        char tmp[400];
        snprintf(tmp, sizeof tmp, "%.*f", (int)nd, a.u.f);
        *out = v_float(strtod(tmp, NULL));
        return true;
    }
    case FN_SET: return from_iterable(V_SET, args, argc, out, e);
    case FN_LIST: return from_iterable(V_LIST, args, argc, out, e);
    case FN_TUPLE: return from_iterable(V_TUPLE, args, argc, out, e);
    case FN_MENU_SET_ADD: {
        value c = args[0];
        if (c.t == V_SET) {
            if (!v_set_add(c, v_inc(args[1]), e)) return false;
        } else if (c.t == V_LIST) {
            v_list_push(c, v_inc(args[1]));
        } else {
            return verr_set(e, "TypeError: menu set must be a set or a list, not '%s'", v_type_name(c));
        }
        *out = v_none();
        return true;
    }
    default:
        return verr_set(e, "builtin %d is not a pure function", fn);
    }
}

/* ---- methods ------------------------------------------------------------------- */

enum {
    M_STRIP, M_LOWER, M_UPPER, M_CAPITALIZE, M_TITLE, M_ADD, M_APPEND, M_REMOVE, M_DISCARD, M_POP, M_GET, M_KEYS,
    M_VALUES, M_ITEMS, M_COUNT, M_INDEX, M_STARTSWITH, M_ENDSWITH, M_REPLACE, M_SPLIT, M_JOIN, M_COUNT_METHODS
};

static const char *const method_names[M_COUNT_METHODS] = {
    "strip", "lower", "upper", "capitalize", "title", "add", "append", "remove", "discard", "pop", "get",
    "keys", "values", "items", "count", "index", "startswith", "endswith", "replace", "split", "join"};

static bool need_str(value v, const char *what, verr *e)
{
    if (v.t == V_STR) return true;
    return verr_set(e, "TypeError: %s must be str, not %s", what, v_type_name(v));
}

static value str_strip(vstr *s, value chars)
{
    size_t a = 0, b = s->len;
    if (chars.t == V_STR) {
        vstr *cs = v_as_str(chars);
        for (;;) {
            if (a >= b) break;
            size_t p = a;
            uint32_t c = vn_utf8_next(s->data, s->len, &p);
            char enc[4];
            size_t el = vn_utf8_encode(c, enc);
            bool hit = false;
            for (size_t q = 0; q + el <= cs->len; q++)
                if (memcmp(cs->data + q, enc, el) == 0) hit = true;
            if (!hit) break;
            a = p;
        }
        while (b > a) {
            size_t p = b - 1;
            while (p > a && ((uint8_t)s->data[p] & 0xC0) == 0x80) p--;
            size_t q = p;
            uint32_t c = vn_utf8_next(s->data, s->len, &q);
            char enc[4];
            size_t el = vn_utf8_encode(c, enc);
            bool hit = false;
            for (size_t k = 0; k + el <= cs->len; k++)
                if (memcmp(cs->data + k, enc, el) == 0) hit = true;
            if (!hit) break;
            b = p;
        }
    } else {
        while (a < b) {
            size_t p = a;
            if (!is_space(vn_utf8_next(s->data, s->len, &p))) break;
            a = p;
        }
        while (b > a) {
            size_t p = b - 1;
            while (p > a && ((uint8_t)s->data[p] & 0xC0) == 0x80) p--;
            size_t q = p;
            if (!is_space(vn_utf8_next(s->data, s->len, &q))) break;
            b = p;
        }
    }
    return v_str(s->data + a, b - a);
}

static value str_case(vstr *s, int mode)
{
    value r = v_str(s->data, s->len);
    char *d = v_as_str(r)->data;
    bool word_start = true;
    for (uint32_t i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)d[i];
        bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        switch (mode) {
        case M_LOWER: d[i] = (char)tolower(c); break;
        case M_UPPER: d[i] = (char)toupper(c); break;
        case M_CAPITALIZE: d[i] = (char)(i == 0 ? toupper(c) : tolower(c)); break;
        case M_TITLE:
            if (alpha) d[i] = (char)(word_start ? toupper(c) : tolower(c));
            word_start = !alpha && c < 0x80;
            if (c >= 0x80) word_start = false;
            break;
        }
    }
    return r;
}

static int64_t str_find(vstr *s, vstr *sub, size_t from)
{
    if (sub->len == 0) return (int64_t)from;
    for (size_t i = from; i + sub->len <= s->len; i++)
        if (memcmp(s->data + i, sub->data, sub->len) == 0) return (int64_t)i;
    return -1;
}

static bool str_affix(vstr *s, value arg, bool end, bool *out, verr *e)
{
    value *cands = &arg;
    uint32_t n = 1;
    if (arg.t == V_TUPLE) {
        cands = v_as_list(arg)->items;
        n = v_as_list(arg)->n;
    }
    *out = false;
    for (uint32_t i = 0; i < n; i++) {
        if (!need_str(cands[i], "startswith/endswith argument", e)) return false;
        vstr *p = v_as_str(cands[i]);
        if (p->len <= s->len && memcmp(s->data + (end ? s->len - p->len : 0), p->data, p->len) == 0) *out = true;
    }
    return true;
}

static value str_split(vstr *s, value sep, int64_t maxsplit, verr *e, bool *ok)
{
    value r = v_list_new(V_LIST, 4);
    *ok = true;
    if (sep.t == V_NONE) {
        size_t i = 0;
        while (i < s->len) {
            while (i < s->len && isspace((unsigned char)s->data[i])) i++;
            if (i >= s->len) break;
            if (maxsplit >= 0 && (int64_t)v_as_list(r)->n >= maxsplit) {
                size_t end = s->len;
                while (end > i && isspace((unsigned char)s->data[end - 1])) end--;
                v_list_push(r, v_str(s->data + i, end - i));
                break;
            }
            size_t j = i;
            while (j < s->len && !isspace((unsigned char)s->data[j])) j++;
            v_list_push(r, v_str(s->data + i, j - i));
            i = j;
        }
        return r;
    }
    if (sep.t != V_STR || v_as_str(sep)->len == 0) {
        *ok = sep.t == V_STR ? verr_set(e, "ValueError: empty separator")
                             : verr_set(e, "TypeError: must be str or None, not %s", v_type_name(sep));
        return r;
    }
    vstr *p = v_as_str(sep);
    size_t i = 0;
    for (;;) {
        int64_t j = (maxsplit >= 0 && (int64_t)v_as_list(r)->n >= maxsplit) ? -1 : str_find(s, p, i);
        if (j < 0) {
            v_list_push(r, v_str(s->data + i, s->len - i));
            break;
        }
        v_list_push(r, v_str(s->data + i, (size_t)j - i));
        i = (size_t)j + p->len;
    }
    return r;
}

bool v_call_method(int m, value obj, value *args, int argc, value *out, verr *e)
{
    if (m < 0 || m >= M_COUNT_METHODS) return verr_set(e, "bad method %d", m);
    const char *name = method_names[m];
    *out = v_none();
#define ARGS(lo, hi)                                                                                \
    if (argc < (lo) || argc > (hi))                                                                 \
    return verr_set(e, "TypeError: %s.%s() takes %d to %d arguments (%d given)", v_type_name(obj), name, lo, hi, argc)

    if (obj.t == V_STR) {
        vstr *s = v_as_str(obj);
        switch (m) {
        case M_STRIP:
            ARGS(0, 1);
            if (argc && args[0].t != V_NONE && !need_str(args[0], "strip arg", e)) return false;
            *out = str_strip(s, argc ? args[0] : v_none());
            return true;
        case M_LOWER: case M_UPPER: case M_CAPITALIZE: case M_TITLE:
            ARGS(0, 0);
            *out = str_case(s, m);
            return true;
        case M_COUNT: {
            ARGS(1, 1);
            if (!need_str(args[0], "count arg", e)) return false;
            vstr *sub = v_as_str(args[0]);
            int64_t c = 0;
            if (sub->len == 0) {
                c = s->chars + 1;
            } else {
                for (int64_t p = str_find(s, sub, 0); p >= 0; p = str_find(s, sub, (size_t)p + sub->len)) c++;
            }
            *out = v_int(c);
            return true;
        }
        case M_INDEX: {
            ARGS(1, 1);
            if (!need_str(args[0], "index arg", e)) return false;
            int64_t p = str_find(s, v_as_str(args[0]), 0);
            if (p < 0) return verr_set(e, "ValueError: substring not found");
            *out = v_int(count_chars(s->data, (size_t)p));
            return true;
        }
        case M_STARTSWITH: case M_ENDSWITH: {
            ARGS(1, 1);
            bool r;
            if (!str_affix(s, args[0], m == M_ENDSWITH, &r, e)) return false;
            *out = v_bool(r);
            return true;
        }
        case M_REPLACE: {
            ARGS(2, 3);
            if (!need_str(args[0], "replace arg 1", e) || !need_str(args[1], "replace arg 2", e)) return false;
            if (argc == 3 && !is_intlike(args[2])) return verr_set(e, "TypeError: replace count must be int");
            int64_t limit = argc == 3 ? as_int(args[2]) : -1;
            vstr *a = v_as_str(args[0]), *b = v_as_str(args[1]);
            vn_buf buf = {0};
            size_t i = 0;
            int64_t done = 0;
            if (a->len == 0) {
                /* Python inserts the replacement between every character. */
                size_t pos = 0;
                while (limit < 0 || done < limit) {
                    vn_buf_put(&buf, b->data, b->len);
                    done++;
                    if (pos >= s->len) break;
                    size_t st = pos;
                    vn_utf8_next(s->data, s->len, &pos);
                    vn_buf_put(&buf, s->data + st, pos - st);
                }
                vn_buf_put(&buf, s->data + pos, s->len - pos);
            } else {
                for (;;) {
                    int64_t p = (limit >= 0 && done >= limit) ? -1 : str_find(s, a, i);
                    if (p < 0) break;
                    vn_buf_put(&buf, s->data + i, (size_t)p - i);
                    vn_buf_put(&buf, b->data, b->len);
                    i = (size_t)p + a->len;
                    done++;
                }
                vn_buf_put(&buf, s->data + i, s->len - i);
            }
            *out = v_str((const char *)buf.data, buf.len);
            vn_buf_free(&buf);
            return true;
        }
        case M_SPLIT: {
            ARGS(0, 2);
            if (argc == 2 && !is_intlike(args[1])) return verr_set(e, "TypeError: maxsplit must be int");
            bool ok;
            value r = str_split(s, argc ? args[0] : v_none(), argc == 2 ? as_int(args[1]) : -1, e, &ok);
            if (!ok) {
                v_dec(r);
                return false;
            }
            *out = r;
            return true;
        }
        case M_JOIN: {
            ARGS(1, 1);
            value *items;
            uint32_t n;
            if (!v_iter_items(args[0], &items, &n, e)) return false;
            vn_buf buf = {0};
            for (uint32_t i = 0; i < n; i++) {
                if (items[i].t != V_STR) {
                    verr_set(e, "TypeError: sequence item %u: expected str instance, %s found", i, v_type_name(items[i]));
                    free_items(items, n);
                    vn_buf_free(&buf);
                    return false;
                }
                if (i) vn_buf_put(&buf, s->data, s->len);
                vn_buf_put(&buf, v_as_str(items[i])->data, v_as_str(items[i])->len);
            }
            free_items(items, n);
            *out = v_str((const char *)buf.data, buf.len);
            vn_buf_free(&buf);
            return true;
        }
        }
    } else if (obj.t == V_LIST) {
        vlist *l = v_as_list(obj);
        switch (m) {
        case M_APPEND:
            ARGS(1, 1);
            v_list_push(obj, v_inc(args[0]));
            return true;
        case M_REMOVE: {
            ARGS(1, 1);
            int64_t i = list_find(l, args[0]);
            if (i < 0) return verr_set(e, "ValueError: list.remove(x): x not in list");
            v_dec(l->items[i]);
            memmove(l->items + i, l->items + i + 1, sizeof(value) * (l->n - (uint32_t)i - 1));
            l->n--;
            return true;
        }
        case M_POP: {
            ARGS(0, 1);
            if (l->n == 0) return verr_set(e, "IndexError: pop from empty list");
            int64_t i = (int64_t)l->n - 1;
            if (argc && !norm_index(args[0], l->n, &i, "pop", e)) return false;
            *out = l->items[i];
            memmove(l->items + i, l->items + i + 1, sizeof(value) * (l->n - (uint32_t)i - 1));
            l->n--;
            return true;
        }
        case M_COUNT: {
            ARGS(1, 1);
            int64_t c = 0;
            for (uint32_t i = 0; i < l->n; i++) c += v_equal(l->items[i], args[0]);
            *out = v_int(c);
            return true;
        }
        case M_INDEX: {
            ARGS(1, 1);
            int64_t i = list_find(l, args[0]);
            if (i < 0) return verr_set(e, "ValueError: value is not in list");
            *out = v_int(i);
            return true;
        }
        }
    } else if (obj.t == V_TUPLE) {
        vlist *l = v_as_list(obj);
        if (m == M_COUNT) {
            ARGS(1, 1);
            int64_t c = 0;
            for (uint32_t i = 0; i < l->n; i++) c += v_equal(l->items[i], args[0]);
            *out = v_int(c);
            return true;
        }
        if (m == M_INDEX) {
            ARGS(1, 1);
            int64_t i = list_find(l, args[0]);
            if (i < 0) return verr_set(e, "ValueError: tuple.index(x): x not in tuple");
            *out = v_int(i);
            return true;
        }
    } else if (obj.t == V_SET) {
        vlist *l = v_as_list(obj);
        switch (m) {
        case M_ADD:
            ARGS(1, 1);
            return v_set_add(obj, v_inc(args[0]), e);
        case M_REMOVE: case M_DISCARD: {
            ARGS(1, 1);
            int64_t i = list_find(l, args[0]);
            if (i < 0) {
                if (m == M_DISCARD) return true;
                vn_buf b = {0};
                v_repr(args[0], &b);
                vn_buf_u8(&b, 0);
                verr_set(e, "KeyError: %s", (const char *)b.data);
                vn_buf_free(&b);
                return false;
            }
            v_dec(l->items[i]);
            memmove(l->items + i, l->items + i + 1, sizeof(value) * (l->n - (uint32_t)i - 1));
            l->n--;
            return true;
        }
        case M_POP:
            ARGS(0, 0);
            if (l->n == 0) return verr_set(e, "KeyError: 'pop from an empty set'");
            *out = l->items[0];
            memmove(l->items, l->items + 1, sizeof(value) * (l->n - 1));
            l->n--;
            return true;
        }
    } else if (obj.t == V_DICT) {
        vdict *d = v_as_dict(obj);
        switch (m) {
        case M_GET: {
            ARGS(1, 2);
            int64_t i = v_dict_find(obj, args[0]);
            *out = i >= 0 ? v_inc(d->vals[i]) : argc == 2 ? v_inc(args[1]) : v_none();
            return true;
        }
        case M_POP: {
            ARGS(1, 2);
            int64_t i = v_dict_find(obj, args[0]);
            if (i < 0) {
                if (argc == 2) {
                    *out = v_inc(args[1]);
                    return true;
                }
                return verr_set(e, "KeyError: dict.pop: key not found");
            }
            v_dec(d->keys[i]);
            *out = d->vals[i];
            memmove(d->keys + i, d->keys + i + 1, sizeof(value) * (d->n - (uint32_t)i - 1));
            memmove(d->vals + i, d->vals + i + 1, sizeof(value) * (d->n - (uint32_t)i - 1));
            d->n--;
            return true;
        }
        case M_KEYS: case M_VALUES: case M_ITEMS: {
            ARGS(0, 0);
            value r = v_list_new(V_LIST, d->n);
            for (uint32_t i = 0; i < d->n; i++) {
                if (m == M_KEYS) {
                    v_list_push(r, v_inc(d->keys[i]));
                } else if (m == M_VALUES) {
                    v_list_push(r, v_inc(d->vals[i]));
                } else {
                    value t = v_list_new(V_TUPLE, 2);
                    v_list_push(t, v_inc(d->keys[i]));
                    v_list_push(t, v_inc(d->vals[i]));
                    v_list_push(r, t);
                }
            }
            *out = r;
            return true;
        }
        }
    }
#undef ARGS
    return verr_set(e, "AttributeError: '%s' object has no attribute '%s'", v_type_name(obj), name);
}

/* ---- serialisation -------------------------------------------------------------- */

enum { S_UNSET, S_NONE, S_FALSE, S_TRUE, S_INT, S_FLOAT, S_STR, S_LIST, S_TUPLE, S_SET, S_DICT, S_REF };

struct vser {
    vn_buf *out;
    vobj **keys;
    uint32_t *ids;
    uint32_t cap, count;
};

vser *vser_new(vn_buf *out)
{
    vser *s = vn_calloc(1, sizeof *s);
    s->out = out;
    s->cap = 64;
    s->keys = vn_calloc(s->cap, sizeof *s->keys);
    s->ids = vn_calloc(s->cap, sizeof *s->ids);
    return s;
}

void vser_free(vser *s)
{
    if (!s) return;
    free(s->keys);
    free(s->ids);
    free(s);
}

static uint32_t ptr_hash(const void *p, uint32_t cap)
{
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return (uint32_t)x & (cap - 1);
}

static bool ser_lookup(vser *s, vobj *o, uint32_t *id)
{
    for (uint32_t h = ptr_hash(o, s->cap);; h = (h + 1) & (s->cap - 1)) {
        if (!s->keys[h]) return false;
        if (s->keys[h] == o) {
            *id = s->ids[h];
            return true;
        }
    }
}

static void ser_insert(vser *s, vobj *o)
{
    if ((s->count + 1) * 2 > s->cap) {
        uint32_t oc = s->cap;
        vobj **ok = s->keys;
        uint32_t *oi = s->ids;
        s->cap *= 2;
        s->keys = vn_calloc(s->cap, sizeof *s->keys);
        s->ids = vn_calloc(s->cap, sizeof *s->ids);
        for (uint32_t i = 0; i < oc; i++) {
            if (!ok[i]) continue;
            uint32_t h = ptr_hash(ok[i], s->cap);
            while (s->keys[h]) h = (h + 1) & (s->cap - 1);
            s->keys[h] = ok[i];
            s->ids[h] = oi[i];
        }
        free(ok);
        free(oi);
    }
    uint32_t h = ptr_hash(o, s->cap);
    while (s->keys[h]) h = (h + 1) & (s->cap - 1);
    s->keys[h] = o;
    s->ids[h] = s->count++;
}

void vser_value(vser *s, value v)
{
    vn_buf *b = s->out;
    switch (v.t) {
    case V_UNSET: vn_buf_u8(b, S_UNSET); return;
    case V_NONE: vn_buf_u8(b, S_NONE); return;
    case V_BOOL: vn_buf_u8(b, v.u.b ? S_TRUE : S_FALSE); return;
    case V_INT: vn_buf_u8(b, S_INT); vn_buf_u64(b, (uint64_t)v.u.i); return;
    case V_FLOAT: vn_buf_u8(b, S_FLOAT); vn_buf_f64(b, v.u.f); return;
    default: break;
    }
    uint32_t id;
    if (ser_lookup(s, v.u.o, &id)) {
        vn_buf_u8(b, S_REF);
        vn_buf_u32(b, id);
        return;
    }
    ser_insert(s, v.u.o);
    if (v.t == V_STR) {
        vn_buf_u8(b, S_STR);
        vn_buf_str(b, v_as_str(v)->data, v_as_str(v)->len);
    } else if (v.t == V_DICT) {
        vdict *d = v_as_dict(v);
        vn_buf_u8(b, S_DICT);
        vn_buf_u32(b, d->n);
        for (uint32_t i = 0; i < d->n; i++) {
            vser_value(s, d->keys[i]);
            vser_value(s, d->vals[i]);
        }
    } else {
        vlist *l = v_as_list(v);
        vn_buf_u8(b, v.t == V_LIST ? S_LIST : v.t == V_TUPLE ? S_TUPLE : S_SET);
        vn_buf_u32(b, l->n);
        for (uint32_t i = 0; i < l->n; i++) vser_value(s, l->items[i]);
    }
}

struct vdeser {
    vn_rd *in;
    value *objs;
    uint32_t n, cap;
    int depth;
};

vdeser *vdeser_new(vn_rd *in)
{
    vdeser *d = vn_calloc(1, sizeof *d);
    d->in = in;
    return d;
}

void vdeser_free(vdeser *d)
{
    if (!d) return;
    for (uint32_t i = 0; i < d->n; i++) v_dec(d->objs[i]);
    free(d->objs);
    free(d);
}

static void deser_track(vdeser *d, value v)
{
    if (d->n == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 32;
        d->objs = vn_realloc(d->objs, sizeof(value) * d->cap);
    }
    d->objs[d->n++] = v_inc(v);
}

bool vdeser_value(vdeser *d, value *out)
{
    vn_rd *r = d->in;
    *out = v_none();
    if (++d->depth > 200) {
        d->depth--;
        return false;
    }
    bool ok = true;
    uint8_t tag = vn_rd_u8(r);
    switch (tag) {
    case S_UNSET: *out = v_unset(); break;
    case S_NONE: break;
    case S_FALSE: *out = v_bool(false); break;
    case S_TRUE: *out = v_bool(true); break;
    case S_INT: *out = v_int((int64_t)vn_rd_u64(r)); break;
    case S_FLOAT: *out = v_float(vn_rd_f64(r)); break;
    case S_STR: {
        uint32_t len;
        const char *p = vn_rd_str(r, &len);
        *out = v_str(p, len);
        deser_track(d, *out);
        break;
    }
    case S_LIST: case S_TUPLE: case S_SET: {
        uint32_t n = vn_rd_u32(r);
        if (n > r->len - r->pos) {
            ok = false;
            break;
        }
        value l = v_list_new(tag == S_LIST ? V_LIST : tag == S_TUPLE ? V_TUPLE : V_SET, n);
        deser_track(d, l);
        for (uint32_t i = 0; i < n && ok; i++) {
            value item;
            ok = vdeser_value(d, &item);
            if (ok) v_list_push(l, item);
        }
        *out = l;
        break;
    }
    case S_DICT: {
        uint32_t n = vn_rd_u32(r);
        if (n > r->len - r->pos) {
            ok = false;
            break;
        }
        value m = v_dict_new(n);
        deser_track(d, m);
        vdict *dd = v_as_dict(m);
        for (uint32_t i = 0; i < n && ok; i++) {
            value k, v;
            ok = vdeser_value(d, &k);
            if (!ok) break;
            ok = vdeser_value(d, &v);
            if (!ok) {
                v_dec(k);
                break;
            }
            /* Keys were unique when written; append without re-checking. */
            dd->keys[dd->n] = k;
            dd->vals[dd->n] = v;
            dd->n++;
        }
        *out = m;
        break;
    }
    case S_REF: {
        uint32_t id = vn_rd_u32(r);
        if (id >= d->n) {
            ok = false;
            break;
        }
        *out = v_inc(d->objs[id]);
        break;
    }
    default: ok = false; break;
    }
    d->depth--;
    if (r->bad) ok = false;
    if (!ok) {
        v_dec(*out);
        *out = v_none();
    }
    return ok;
}
