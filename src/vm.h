#ifndef GB_VM_H
#define GB_VM_H

#include "common.h"
#include <setjmp.h>

#define GB_STACK_SIZE (1 << 18)
#define GB_MAX_FRAMES 4096

typedef struct {
    int func;
    uint8_t *ip;
    Value *base;
    ObjClosure *closure;   /* the function value being run, or NULL for a direct call */
} Frame;

struct VM {
    Program *prog;
    Value *stack;
    Value *sp;
    Frame frames[GB_MAX_FRAMES];
    int nframes;
    Value *globals;
    ObjUpvalue *openUpvalues;

    /* garbage collector */
    Obj *objects;
    size_t bytesAllocated;
    size_t nextGC;
    Obj **gray;
    int ngray, capGray;
    bool gcStress;      /* GOOBER_GC_STRESS=1: collect on every allocation */

    int dataPos;        /* the next defdata item read_int/read_float/read_string returns */

    uint8_t *ip;        /* synced before native calls, for error lines */
    int exitCode;
    jmp_buf exitJmp;
};

extern VM *gb_vm;

/* objects.c */
ObjString *str_new(VM *vm, const char *s, int len);
ObjString *str_from_c(VM *vm, const char *s);
ObjInst *inst_new(VM *vm, int tid);
ObjArray *array_new(VM *vm, uint8_t elemKind, int ndims, const int *dims);
ObjList *list_new(VM *vm);
void list_add(VM *vm, ObjList *l, Value v, bool atFront);
void list_insert_before(VM *vm, ObjList *l, ObjLink *at, Value v);
void list_unlink(ObjLink *k);
void list_clear(ObjList *l);
ObjClosure *closure_new(VM *vm, int fidx, int nup);
ObjUpvalue *upvalue_new(VM *vm, Value *loc);
void gc_collect(VM *vm);
void gc_free_all(VM *vm);
Value default_value(uint8_t kind);

static inline const char *str_chars(ObjString *s) { return s ? s->chars : ""; }
static inline int str_len(ObjString *s) { return s ? s->len : 0; }

/* vm.c */
void vm_init(VM *vm, Program *prog);
int vm_run(VM *vm);
void vm_free(VM *vm);
void vm_error(VM *vm, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
void vm_exit(VM *vm, int code) __attribute__((noreturn));
void disassemble(Program *prog);
Value vm_convert(VM *vm, Value v, uint8_t to);

/* builtins.c / gfx.c */
extern NativeDef gb_core_natives[];
extern NativeDef gb_gfx_natives[];
extern ConstDef gb_gfx_consts[];
const NativeDef *native_get(int idx);
int native_count(void);
int native_find(const char *lname);
void gfx_pump_events(void);
void gfx_shutdown(void);

/* net.c: networking, shared by goober and GooberEngine */
extern NativeDef gb_net_natives[];
extern ConstDef gb_net_consts[];
void net_poll(void);       /* read and write the sockets (flip and delay call it) */
void net_shutdown(void);   /* leave the game, close everything */

#endif
