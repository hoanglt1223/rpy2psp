/* Script values with Python semantics: None, bool, int, float, str, list, tuple,
 * dict, set. Containers and strings are reference counted (no cycle collection:
 * a list that contains itself leaks, which story scripts do not do). */
#ifndef VN_VALUE_H
#define VN_VALUE_H

#include "base.h"

#ifdef _MSC_VER
#pragma warning(disable : 4200) /* flexible array member (standard C99) */
#endif

typedef enum vtype { V_UNSET, V_NONE, V_BOOL, V_INT, V_FLOAT, V_STR, V_LIST, V_TUPLE, V_SET, V_DICT } vtype;

typedef struct vobj {
    uint32_t refs;
    uint8_t type;
} vobj;

typedef struct value {
    uint8_t t;
    union {
        bool b;
        int64_t i;
        double f;
        vobj *o;
    } u;
} value;

typedef struct vstr {
    vobj h;
    uint32_t len;     /* bytes */
    uint32_t chars;   /* code points */
    char data[];      /* UTF-8, NUL-terminated */
} vstr;

/* list, tuple and set share one layout; sets keep insertion order. */
typedef struct vlist {
    vobj h;
    uint32_t n, cap;
    value *items;
} vlist;

typedef struct vdict {
    vobj h;
    uint32_t n, cap;
    value *keys, *vals;
} vdict;

#define VERR_CAP 200
/* Error sink for value operations: functions return false and fill msg. */
typedef struct verr {
    char msg[VERR_CAP];
} verr;

bool verr_set(verr *e, const char *fmt, ...) VN_PRINTF(2, 3);

static inline value v_unset(void) { value v = {V_UNSET, {0}}; return v; }
static inline value v_none(void) { value v = {V_NONE, {0}}; return v; }
static inline value v_bool(bool b) { value v = {V_BOOL, {0}}; v.u.b = b; return v; }
static inline value v_int(int64_t i) { value v = {V_INT, {0}}; v.u.i = i; return v; }
static inline value v_float(double f) { value v = {V_FLOAT, {0}}; v.u.f = f; return v; }
static inline bool v_is_obj(value v) { return v.t >= V_STR; }

value v_str(const char *s, size_t len);
value v_cstr(const char *s);
value v_list_new(vtype t, uint32_t cap);   /* V_LIST, V_TUPLE or V_SET */
value v_dict_new(uint32_t cap);

static inline value v_inc(value v)
{
    if (v_is_obj(v)) v.u.o->refs++;
    return v;
}
void v_dec(value v);

static inline vstr *v_as_str(value v) { return (vstr *)v.u.o; }
static inline vlist *v_as_list(value v) { return (vlist *)v.u.o; }
static inline vdict *v_as_dict(value v) { return (vdict *)v.u.o; }

/* Takes ownership of item. */
void v_list_push(value list, value item);
/* Set add with membership test; takes ownership of item. */
bool v_set_add(value set, value item, verr *e);
/* Dict store; takes ownership of key and val. */
bool v_dict_set(value dict, value key, value val, verr *e);
/* Borrowed lookup; returns index or -1. */
int64_t v_dict_find(value dict, value key);

const char *v_type_name(value v);
bool v_truthy(value v);
bool v_equal(value a, value b);
bool v_hashable(value v);

enum { BIN_ADD, BIN_SUB, BIN_MUL, BIN_DIV, BIN_FLOORDIV, BIN_MOD };
enum { UN_NEG, UN_POS, UN_NOT };
enum { CMP_EQ, CMP_NE, CMP_LT, CMP_LE, CMP_GT, CMP_GE, CMP_IN, CMP_NOTIN, CMP_IS, CMP_ISNOT };

/* Results are new references; operands are borrowed. */
bool v_binary(int op, value a, value b, value *out, verr *e);
bool v_unary(int op, value a, value *out, verr *e);
bool v_compare(int op, value a, value b, value *out, verr *e);
bool v_subscr(value c, value k, value *out, verr *e);
bool v_slice(value c, value lo, value hi, value *out, verr *e);
bool v_store_subscr(value c, value k, value v, verr *e);   /* v borrowed */

/* Python str() and repr(), appended to buf. */
void v_to_str(value v, vn_buf *buf);
void v_repr(value v, vn_buf *buf);
value v_str_of(value v);
/* format(value, spec) for interpolation fields. */
bool v_format(value v, const char *spec, size_t speclen, vn_buf *buf, verr *e);

/* Iteration over list/tuple/set/dict keys/str characters: materialised items. */
bool v_iter_items(value v, value **items, uint32_t *n, verr *e);   /* caller v_dec's each and frees */

/* Builtins that need no engine state (bytecode.md CALL_FN 0-10, 24) and the
 * methods (CALL_METHOD). args are borrowed. */
bool v_call_pure(int fn, value *args, int argc, value *out, verr *e);
bool v_call_method(int method, value obj, value *args, int argc, value *out, verr *e);

enum {
    FN_LEN, FN_STR, FN_INT, FN_FLOAT, FN_BOOL, FN_MIN, FN_MAX, FN_ABS, FN_ROUND, FN_SET, FN_LIST,
    FN_RANDINT, FN_CHOICE, FN_RANDOM, FN_INPUT, FN_NOTIFY, FN_FULL_RESTART, FN_BLOCK_ROLLBACK,
    FN_SAVE_PERSISTENT, FN_FORCE_AUTOSAVE, FN_HAS_LABEL, FN_SHOWING, FN_SET_VOLUME, FN_MENU_SET_ADD, FN_TUPLE,
    FN_COUNT
};

/* Serialisation that preserves aliasing (two names for one list stay one list). */
typedef struct vser vser;
vser *vser_new(vn_buf *out);
void vser_value(vser *s, value v);
void vser_free(vser *s);

typedef struct vdeser vdeser;
vdeser *vdeser_new(vn_rd *in);
bool vdeser_value(vdeser *d, value *out);
void vdeser_free(vdeser *d);   /* drops its references to the objects it built */

#endif
