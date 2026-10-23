#include "vm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Instructions without a host interaction before the VM gives up (an endless loop). */
#define VM_STEP_LIMIT 20000000u

static const struct { uint8_t lo, hi; } fn_arity[FN_COUNT] = {
    {1, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {1, 8}, {1, 8}, {1, 1}, {1, 2}, {0, 1}, {0, 1},
    {2, 2}, {1, 1}, {0, 0}, {3, 3}, {1, 1}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {1, 1}, {1, 1}, {3, 3}, {2, 2}, {0, 1},
};

bool vm_error(vm *v, const char *fmt, ...)
{
    char msg[VERR_CAP];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(v->err.msg, sizeof v->err.msg, "%s", msg);
    return false;
}

/* ---- Python random (MT19937, CPython's seeding and derivations) ---------------------- */

void vm_rng_seed(vm_rng *r, uint32_t seed)
{
    /* CPython seeds an int through init_by_array with its 32-bit words (one word for 0). */
    uint32_t *mt = r->mt;
    mt[0] = 19650218u;
    for (int i = 1; i < 624; i++) mt[i] = 1812433253u * (mt[i - 1] ^ (mt[i - 1] >> 30)) + (uint32_t)i;
    uint32_t key[1] = {seed};
    int i = 1, j = 0;
    for (int k = 624; k; k--) {
        mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1664525u)) + key[j] + (uint32_t)j;
        i++;
        j++;
        if (i >= 624) {
            mt[0] = mt[623];
            i = 1;
        }
        if (j >= 1) j = 0;
    }
    for (int k = 623; k; k--) {
        mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1566083941u)) - (uint32_t)i;
        i++;
        if (i >= 624) {
            mt[0] = mt[623];
            i = 1;
        }
    }
    mt[0] = 0x80000000u;
    r->index = 624;
}

uint32_t vm_rng_u32(vm_rng *r)
{
    uint32_t *mt = r->mt;
    if (r->index >= 624) {
        for (int k = 0; k < 624; k++) {
            uint32_t y = (mt[k] & 0x80000000u) | (mt[(k + 1) % 624] & 0x7fffffffu);
            mt[k] = mt[(k + 397) % 624] ^ (y >> 1) ^ ((y & 1) ? 0x9908b0dfu : 0u);
        }
        r->index = 0;
    }
    uint32_t y = mt[r->index++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    y ^= y >> 18;
    return y;
}

static uint64_t rng_bits(vm_rng *r, int k)
{
    uint64_t out = 0;
    for (int shift = 0; k > 0; shift += 32, k -= 32) {
        uint32_t w = vm_rng_u32(r);
        if (k < 32) w >>= 32 - k;
        out |= (uint64_t)w << shift;
    }
    return out;
}

static uint64_t rng_below(vm_rng *r, uint64_t n)
{
    int k = 0;
    for (uint64_t t = n; t; t >>= 1) k++;
    uint64_t x = rng_bits(r, k);
    while (x >= n) x = rng_bits(r, k);
    return x;
}

/* ---- setup ------------------------------------------------------------------- */

void vm_init(vm *v, const vn_script *s)
{
    memset(v, 0, sizeof *v);
    v->s = s;
    v->vars = vn_calloc(s->nsyms ? s->nsyms : 1, sizeof *v->vars);
    v->const_strs = vn_calloc(s->nstrings ? s->nstrings : 1, sizeof *v->const_strs);
    v->stack_cap = 64;
    v->stack = vn_alloc(sizeof(value) * v->stack_cap);
    v->last_jump = VN_NONE32;
    vm_rng_seed(&v->rng, 0);
    for (uint32_t pc = 0; pc < s->code_size;) {
        uint32_t len = vn_insn_length(s, pc);
        if (!len) break;
        if (s->code[pc] == OP_CALL_FN && s->code[pc + 1] >= FN_RANDINT && s->code[pc + 1] <= FN_RANDOM)
            v->uses_random = true;
        pc += len;
    }
}

static void drop_frames(vm *v)
{
    for (uint32_t i = 0; i < v->nframes; i++) {
        for (uint32_t k = 0; k < v->frames[i].nsaved; k++) v_dec(v->frames[i].saved[k].old);
        free(v->frames[i].saved);
    }
    v->nframes = 0;
}

void vm_reset(vm *v)
{
    while (v->sp) v_dec(v->stack[--v->sp]);
    drop_frames(v);
    for (uint32_t i = 0; i < v->s->nsyms; i++) {
        if (v->s->sym_kind[i] == 1) continue;
        v_dec(v->vars[i]);
        v->vars[i] = v_unset();
    }
    v->last_jump = VN_NONE32;
}

void vm_free(vm *v)
{
    vm_reset(v);
    for (uint32_t i = 0; i < v->s->nsyms; i++) v_dec(v->vars[i]);
    for (uint32_t i = 0; i < v->s->nstrings; i++) v_dec(v->const_strs[i]);
    free(v->vars);
    free(v->const_strs);
    free(v->stack);
    free(v->frames);
    memset(v, 0, sizeof *v);
}

/* ---- stack ------------------------------------------------------------------- */

void vm_push(vm *v, value val)
{
    if (v->sp == v->stack_cap) {
        v->stack_cap *= 2;
        v->stack = vn_realloc(v->stack, sizeof(value) * v->stack_cap);
    }
    v->stack[v->sp++] = val;
}

value vm_pop(vm *v)
{
    /* Callers check depth first; an empty pop yields None rather than crashing. */
    return v->sp ? v->stack[--v->sp] : v_none();
}

value vm_peek(vm *v, uint32_t depth)
{
    return depth < v->sp ? v->stack[v->sp - 1 - depth] : v_none();
}

const char *vm_sym_name(const vm *v, uint32_t sym)
{
    return sym < v->s->nsyms ? vn_str(v->s, v->s->sym_name[sym]) : "?";
}

int32_t vm_find_sym(const vm *v, const char *name, size_t len)
{
    for (uint32_t i = 0; i < v->s->nsyms; i++) {
        uint32_t id = v->s->sym_name[i];
        if (v->s->string_len[id] == len && memcmp(v->s->strings[id], name, len) == 0) return (int32_t)i;
    }
    return -1;
}

bool vm_load(vm *v, uint16_t sym, value *out)
{
    if (sym >= v->s->nsyms) return vm_error(v, "bad symbol %u", sym);
    value x = v->vars[sym];
    if (x.t == V_UNSET) {
        if (v->s->sym_kind[sym] == 1) {
            *out = v_none();
            return true;
        }
        return vm_error(v, "NameError: variable %s is not set", vm_sym_name(v, sym));
    }
    *out = v_inc(x);
    return true;
}

static void store(vm *v, uint16_t sym, value val)
{
    v_dec(v->vars[sym]);
    v->vars[sym] = val;
}

/* ---- decoding ---------------------------------------------------------------- */

bool vm_decode(const vm *v, vm_ops *o)
{
    const vn_script *s = v->s;
    memset(o, 0, sizeof *o);
    o->len = vn_insn_length(s, v->pc);
    if (!o->len) return false;
    const uint8_t *p = s->code + v->pc;
    o->op = p[0];
    size_t pos = 1;
    int k = 0;
    for (const char *f = vn_op_format(o->op); *f; f++) {
        switch (*f) {
        case 'b': o->u[k++] = p[pos]; pos += 1; break;
        case 'h': case 'v': o->u[k++] = vn_le16(p + pos); pos += 2; break;
        case 'i': case 's': case 'a': o->u[k++] = vn_le32(p + pos); pos += 4; break;
        case 'f': o->f = vn_lef32(p + pos); pos += 4; break;
        case 'd': {
            uint64_t bits = (uint64_t)vn_le32(p + pos) | ((uint64_t)vn_le32(p + pos + 4) << 32);
            memcpy(&o->d, &bits, 8);
            pos += 8;
            break;
        }
        case 'S': case 'M':
            o->count = p[pos];
            o->list = p + pos + 1;
            pos += 1 + (size_t)o->count * (*f == 'S' ? 4 : 8);
            break;
        }
    }
    return true;
}

/* ---- execution --------------------------------------------------------------- */

static bool push_frame(vm *v, uint32_t ret)
{
    if (v->nframes >= 4096) return vm_error(v, "RecursionError: call stack deeper than 4096");
    if (v->nframes == v->frames_cap) {
        v->frames_cap = v->frames_cap ? v->frames_cap * 2 : 16;
        v->frames = vn_realloc(v->frames, sizeof(vm_frame) * v->frames_cap);
    }
    vm_frame *f = &v->frames[v->nframes++];
    memset(f, 0, sizeof *f);
    f->ret = ret;
    return true;
}

static void frame_remember(vm_frame *f, uint16_t sym, value old)
{
    for (uint32_t i = 0; i < f->nsaved; i++)
        if (f->saved[i].sym == sym) return;
    if (f->nsaved == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 4;
        f->saved = vn_realloc(f->saved, sizeof(vm_saved) * f->cap);
    }
    f->saved[f->nsaved].sym = sym;
    f->saved[f->nsaved].old = v_inc(old);
    f->nsaved++;
}

bool vm_jump_label(vm *v, value name, bool call, uint32_t ret)
{
    if (name.t != V_STR) return vm_error(v, "TypeError: label name must be a string, not %s", v_type_name(name));
    uint32_t addr = vn_script_label(v->s, v_as_str(name)->data, v_as_str(name)->len);
    if (addr == VN_NONE32) return vm_error(v, "no label '%s'", v_as_str(name)->data);
    if (call && !push_frame(v, ret)) return false;
    v->pc = addr;
    v->last_jump = addr;
    return true;
}

static bool is_host_fn(uint8_t fn)
{
    switch (fn) {
    case FN_INPUT: case FN_NOTIFY: case FN_FULL_RESTART: case FN_BLOCK_ROLLBACK: case FN_SAVE_PERSISTENT:
    case FN_FORCE_AUTOSAVE: case FN_SHOWING: case FN_SET_VOLUME:
        return true;
    default:
        return false;
    }
}

static bool call_random(vm *v, uint8_t fn, value *args, value *out)
{
    if (fn == FN_RANDOM) {
        uint32_t a = vm_rng_u32(&v->rng) >> 5, b = vm_rng_u32(&v->rng) >> 6;
        *out = v_float((a * 67108864.0 + b) * (1.0 / 9007199254740992.0));
        return true;
    }
    if (fn == FN_RANDINT) {
        if ((args[0].t != V_INT && args[0].t != V_BOOL) || (args[1].t != V_INT && args[1].t != V_BOOL))
            return vm_error(v, "TypeError: randint() arguments must be integers");
        int64_t a = args[0].t == V_BOOL ? args[0].u.b : args[0].u.i;
        int64_t b = args[1].t == V_BOOL ? args[1].u.b : args[1].u.i;
        if (b < a) return vm_error(v, "ValueError: empty range in randint(%lld, %lld)", (long long)a, (long long)b);
        *out = v_int(a + (int64_t)rng_below(&v->rng, (uint64_t)(b - a) + 1));
        return true;
    }
    value seq = args[0];
    uint32_t n;
    if (seq.t == V_LIST || seq.t == V_TUPLE)
        n = v_as_list(seq)->n;
    else if (seq.t == V_STR)
        n = v_as_str(seq)->chars;
    else
        return vm_error(v, "TypeError: choice() needs a sequence, not %s", v_type_name(seq));
    if (n == 0) return vm_error(v, "IndexError: Cannot choose from an empty sequence");
    if (!v_subscr(seq, v_int((int64_t)rng_below(&v->rng, n)), out, &v->err)) return false;
    return true;
}

void vm_start(vm *v, uint32_t addr)
{
    v->pc = addr;
    v->routine = false;
}

bool vm_run_routine(vm *v, uint32_t addr)
{
    uint32_t saved_pc = v->pc;
    bool saved_routine = v->routine;
    uint32_t base_frames = v->nframes;
    v->pc = addr;
    v->routine = true;
    vm_status st = vm_run(v);
    bool ok = st == VM_DONE;
    if (st == VM_HOST) vm_error(v, "instruction %u at %u is not allowed in an init routine", v->s->code[v->pc], v->pc);
    while (v->nframes > base_frames) {
        vm_frame *f = &v->frames[--v->nframes];
        for (uint32_t k = 0; k < f->nsaved; k++) v_dec(f->saved[k].old);
        free(f->saved);
    }
    v->pc = saved_pc;
    v->routine = saved_routine;
    return ok;
}

#define NEED(n)                                                                  \
    do {                                                                         \
        if (v->sp < (uint32_t)(n)) {                                             \
            vm_error(v, "stack underflow at %u", pc);                            \
            return VM_ERROR;                                                     \
        }                                                                        \
    } while (0)

#define JUMP_TO(target)                                                          \
    do {                                                                         \
        uint32_t t_ = (target);                                                  \
        if (t_ >= s->code_size) {                                                \
            vm_error(v, "jump outside code at %u", pc);                          \
            return VM_ERROR;                                                     \
        }                                                                        \
        next = t_;                                                               \
    } while (0)

vm_status vm_run(vm *v)
{
    const vn_script *s = v->s;
    for (uint32_t steps = 0;; steps++) {
        if (steps >= VM_STEP_LIMIT) {
            vm_error(v, "the script ran %u instructions without an interaction (endless loop?)", VM_STEP_LIMIT);
            return VM_ERROR;
        }
        uint32_t pc = v->pc;
        vm_ops o;
        if (!vm_decode(v, &o)) {
            vm_error(v, "bad instruction at %u", pc);
            return VM_ERROR;
        }
        uint32_t next = pc + o.len;
        value a, b, c, r;
        switch (o.op) {
        case OP_NOP: break;
        case OP_PUSH_NONE: vm_push(v, v_none()); break;
        case OP_PUSH_TRUE: vm_push(v, v_bool(true)); break;
        case OP_PUSH_FALSE: vm_push(v, v_bool(false)); break;
        case OP_PUSH_INT: vm_push(v, v_int((int32_t)o.u[0])); break;
        case OP_PUSH_FLOAT: vm_push(v, v_float(o.d)); break;
        case OP_PUSH_STR:
            if (o.u[0] >= s->nstrings) {
                vm_error(v, "bad string %u at %u", o.u[0], pc);
                return VM_ERROR;
            }
            if (v->const_strs[o.u[0]].t != V_STR)
                v->const_strs[o.u[0]] = v_str(s->strings[o.u[0]], s->string_len[o.u[0]]);
            vm_push(v, v_inc(v->const_strs[o.u[0]]));
            break;
        case OP_LOAD:
            if (!vm_load(v, (uint16_t)o.u[0], &r)) return VM_ERROR;
            vm_push(v, r);
            break;
        case OP_STORE: case OP_STORE_DEFAULT:
            NEED(1);
            if (o.u[0] >= s->nsyms) {
                vm_error(v, "bad symbol at %u", pc);
                return VM_ERROR;
            }
            a = vm_pop(v);
            if (o.op == OP_STORE_DEFAULT) {
                value cur = v->vars[o.u[0]];
                bool unset = cur.t == V_UNSET || (s->sym_kind[o.u[0]] == 1 && cur.t == V_NONE);
                if (!unset) {
                    v_dec(a);
                    break;
                }
            }
            store(v, (uint16_t)o.u[0], a);
            break;
        case OP_POP: NEED(1); v_dec(vm_pop(v)); break;
        case OP_DUP: NEED(1); vm_push(v, v_inc(vm_peek(v, 0))); break;
        case OP_BUILD_LIST: case OP_BUILD_TUPLE: case OP_BUILD_SET: {
            uint32_t n = o.u[0];
            NEED(n);
            vtype t = o.op == OP_BUILD_LIST ? V_LIST : o.op == OP_BUILD_TUPLE ? V_TUPLE : V_SET;
            r = v_list_new(t, n);
            uint32_t base = v->sp - n;
            bool ok = true;
            for (uint32_t i = 0; i < n; i++) {
                if (t == V_SET) {
                    if (ok && !v_set_add(r, v->stack[base + i], &v->err)) ok = false;
                    else if (!ok) v_dec(v->stack[base + i]);
                } else {
                    v_list_push(r, v->stack[base + i]);
                }
            }
            v->sp = base;
            if (!ok) {
                v_dec(r);
                return VM_ERROR;
            }
            vm_push(v, r);
            break;
        }
        case OP_BUILD_DICT: {
            uint32_t n = o.u[0];
            NEED(2 * n);
            r = v_dict_new(n);
            uint32_t base = v->sp - 2 * n;
            bool ok = true;
            for (uint32_t i = 0; i < n; i++) {
                value k = v->stack[base + 2 * i], val = v->stack[base + 2 * i + 1];
                if (ok && !v_dict_set(r, k, val, &v->err)) {
                    ok = false;
                } else if (!ok) {
                    v_dec(k);
                    v_dec(val);
                }
            }
            v->sp = base;
            if (!ok) {
                v_dec(r);
                return VM_ERROR;
            }
            vm_push(v, r);
            break;
        }
        case OP_SUBSCR: {
            NEED(2);
            b = vm_pop(v);
            a = vm_pop(v);
            bool ok = v_subscr(a, b, &r, &v->err);
            v_dec(a);
            v_dec(b);
            if (!ok) return VM_ERROR;
            vm_push(v, r);
            break;
        }
        case OP_SLICE: {
            NEED(3);
            c = vm_pop(v);
            b = vm_pop(v);
            a = vm_pop(v);
            bool ok = v_slice(a, b, c, &r, &v->err);
            v_dec(a);
            v_dec(b);
            v_dec(c);
            if (!ok) return VM_ERROR;
            vm_push(v, r);
            break;
        }
        case OP_STORE_SUBSCR: {
            NEED(3);
            c = vm_pop(v);
            b = vm_pop(v);
            a = vm_pop(v);
            bool ok = v_store_subscr(a, b, c, &v->err);
            v_dec(a);
            v_dec(b);
            v_dec(c);
            if (!ok) return VM_ERROR;
            break;
        }
        case OP_BINARY: case OP_UNARY: case OP_COMPARE: {
            bool ok;
            if (o.op == OP_UNARY) {
                NEED(1);
                a = vm_pop(v);
                ok = v_unary((int)o.u[0], a, &r, &v->err);
                v_dec(a);
            } else {
                NEED(2);
                b = vm_pop(v);
                a = vm_pop(v);
                ok = o.op == OP_BINARY ? v_binary((int)o.u[0], a, b, &r, &v->err)
                                       : v_compare((int)o.u[0], a, b, &r, &v->err);
                v_dec(a);
                v_dec(b);
            }
            if (!ok) return VM_ERROR;
            vm_push(v, r);
            break;
        }
        case OP_JUMP: JUMP_TO(o.u[0]); break;
        case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE: {
            NEED(1);
            a = vm_pop(v);
            bool t = v_truthy(a);
            v_dec(a);
            if (t == (o.op == OP_JUMP_IF_TRUE)) JUMP_TO(o.u[0]);
            break;
        }
        case OP_JUMP_IF_FALSE_OR_POP: case OP_JUMP_IF_TRUE_OR_POP: {
            NEED(1);
            bool t = v_truthy(vm_peek(v, 0));
            if (t == (o.op == OP_JUMP_IF_TRUE_OR_POP))
                JUMP_TO(o.u[0]);
            else
                v_dec(vm_pop(v));
            break;
        }
        case OP_CALL_FN: {
            uint8_t fn = (uint8_t)o.u[0], argc = (uint8_t)o.u[1];
            if (fn >= FN_COUNT || argc < fn_arity[fn].lo || argc > fn_arity[fn].hi) {
                vm_error(v, "bad builtin call %u/%u at %u", fn, argc, pc);
                return VM_ERROR;
            }
            NEED(argc);
            if (is_host_fn(fn)) return VM_HOST;
            value *args = v->stack + v->sp - argc;
            bool ok;
            if (fn >= FN_RANDINT && fn <= FN_RANDOM) {
                ok = call_random(v, fn, args, &r);
            } else if (fn == FN_HAS_LABEL) {
                ok = true;
                r = v_bool(args[0].t == V_STR &&
                           vn_script_label(s, v_as_str(args[0])->data, v_as_str(args[0])->len) != VN_NONE32);
            } else {
                ok = v_call_pure(fn, args, argc, &r, &v->err);
            }
            for (uint32_t i = 0; i < argc; i++) v_dec(vm_pop(v));
            if (!ok) return VM_ERROR;
            vm_push(v, r);
            break;
        }
        case OP_CALL_METHOD: {
            uint8_t argc = (uint8_t)o.u[1];
            NEED(argc + 1u);
            value *args = v->stack + v->sp - argc;
            value obj = v->stack[v->sp - argc - 1];
            bool ok = v_call_method((int)o.u[0], obj, args, argc, &r, &v->err);
            for (uint32_t i = 0; i <= argc; i++) v_dec(vm_pop(v));
            if (!ok) return VM_ERROR;
            vm_push(v, r);
            break;
        }
        case OP_CALL:
            if (o.u[0] >= s->code_size) {
                vm_error(v, "call outside code at %u", pc);
                return VM_ERROR;
            }
            if (!push_frame(v, next)) return VM_ERROR;
            next = o.u[0];
            v->last_jump = next;
            break;
        case OP_CALL_DYNAMIC: case OP_JUMP_DYNAMIC: {
            NEED(1);
            a = vm_pop(v);
            bool ok = vm_jump_label(v, a, o.op == OP_CALL_DYNAMIC, next);
            v_dec(a);
            if (!ok) return VM_ERROR;
            next = v->pc;
            break;
        }
        case OP_RETURN: {
            if (v->nframes == 0) {
                v->pc = pc;
                return v->routine ? VM_DONE : VM_END;
            }
            vm_frame *f = &v->frames[--v->nframes];
            next = f->ret;
            for (uint32_t i = f->nsaved; i-- > 0;) store(v, f->saved[i].sym, f->saved[i].old);
            free(f->saved);
            break;
        }
        case OP_BIND:
            NEED(1);
            if (o.u[0] >= s->nsyms) {
                vm_error(v, "bad symbol at %u", pc);
                return VM_ERROR;
            }
            if (v->nframes) frame_remember(&v->frames[v->nframes - 1], (uint16_t)o.u[0], v->vars[o.u[0]]);
            store(v, (uint16_t)o.u[0], vm_pop(v));
            break;
        default:
            if (o.op >= OP_SAY && o.op < OP_COUNT) return VM_HOST;
            vm_error(v, "unknown opcode %u at %u", o.op, pc);
            return VM_ERROR;
        }
        if (o.op == OP_JUMP || ((o.op == OP_JUMP_IF_FALSE || o.op == OP_JUMP_IF_TRUE) && next != pc + o.len))
            v->last_jump = next;
        v->pc = next;
    }
}
