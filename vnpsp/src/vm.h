/* Bytecode interpreter. Pure instructions run inside vm_run; display, audio and
 * interaction instructions stop it with VM_HOST so the engine can checkpoint
 * first and then carry them out (see engine.c). */
#ifndef VN_VM_H
#define VN_VM_H

#include "value.h"
#include "vnb.h"

typedef enum vm_status {
    VM_HOST,     /* pc is at a host instruction, not yet executed */
    VM_DONE,     /* a routine (init/defaults) returned */
    VM_END,      /* the story returned with an empty call stack */
    VM_ERROR     /* vm->err holds the message */
} vm_status;

typedef struct vm_saved {
    uint16_t sym;
    value old;
} vm_saved;

typedef struct vm_frame {
    uint32_t ret;
    uint32_t nsaved, cap;
    vm_saved *saved;
} vm_frame;

/* Python's random module (MT19937) so seeded runs match the reference interpreter. */
typedef struct vm_rng {
    uint32_t mt[624];
    uint32_t index;
} vm_rng;

typedef struct vm {
    const vn_script *s;
    uint32_t pc;
    bool routine;            /* RETURN on an empty stack ends a routine, not the story */
    value *stack;
    uint32_t sp, stack_cap;
    vm_frame *frames;
    uint32_t nframes, frames_cap;
    value *vars;
    vm_rng rng;
    bool uses_random;
    value *const_strs;       /* lazily built PUSH_STR values */
    uint32_t last_jump;      /* target of the latest jump or call, VN_NONE32 after it is read */
    verr err;
} vm;

void vm_init(vm *v, const vn_script *s);
void vm_free(vm *v);
/* Drops stack, frames and store variables (persistent ones stay). */
void vm_reset(vm *v);

void vm_rng_seed(vm_rng *r, uint32_t seed);
uint32_t vm_rng_u32(vm_rng *r);

/* Runs a routine (init or defaults) to its RETURN. Host instructions there are errors. */
bool vm_run_routine(vm *v, uint32_t addr);
void vm_start(vm *v, uint32_t addr);
vm_status vm_run(vm *v);

/* Stack access for the engine while it carries out a host instruction. */
value vm_pop(vm *v);              /* new reference owned by the caller */
void vm_push(vm *v, value val);    /* takes ownership */
value vm_peek(vm *v, uint32_t depth);
bool vm_error(vm *v, const char *fmt, ...) VN_PRINTF(2, 3);

/* Decoded operands of the instruction at v->pc. */
typedef struct vm_ops {
    uint8_t op;
    uint32_t len;
    uint32_t u[4];           /* b, h, v, s, a operands in order */
    float f;
    double d;                /* PUSH_FLOAT */
    uint8_t count;           /* S / M list length */
    const uint8_t *list;     /* S: u32 strings; M: (u32 string, u32 address) pairs */
} vm_ops;
bool vm_decode(const vm *v, vm_ops *ops);

/* Script-level helpers. */
bool vm_jump_label(vm *v, value name, bool call, uint32_t ret);
bool vm_load(vm *v, uint16_t sym, value *out);   /* new reference */
const char *vm_sym_name(const vm *v, uint32_t sym);
int32_t vm_find_sym(const vm *v, const char *name, size_t len);

#endif
