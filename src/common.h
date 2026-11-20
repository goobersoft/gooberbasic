/* GooberBASIC - shared definitions */
#ifndef GB_COMMON_H
#define GB_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>

#define GB_VERSION "0.2.0"

/* ---- Value kinds (runtime tags and static base types) ---- */
typedef enum {
    K_VOID = 0,
    K_INT,
    K_FLOAT,
    K_STRING,
    K_OBJ,     /* instance of a user Type (or Null) */
    K_ARRAY,   /* runtime tag only */
    K_NULL,    /* static type of the Null literal */
    K_LIST,    /* a typed list ("int list", "t_enemy list") */
    K_LINK,    /* internal: a TList node, used by EachIn loops */
    K_LONG,    /* 64-bit integer (as.i64); signed arithmetic */
    K_ULONG    /* static type only: 64-bit unsigned arithmetic (runtime tag is K_LONG) */
} Kind;

/* Numeric storage types. The static base (K_INT, K_LONG, K_ULONG, K_FLOAT)
 * says which width arithmetic is done at; `num` says how the value is
 * stored, so a store into a narrower type wraps (or, for float, rounds).
 *   K_INT   : S8 U8 S16 U16 S32     (as.i)
 *   K_LONG  : U32 S64               (as.i64)
 *   K_ULONG : U64                   (as.i64)
 *   K_FLOAT : F32 F64               (as.f, F32 values are kept rounded) */
typedef enum {
    NT_NONE = 0,
    NT_S8, NT_U8, NT_S16, NT_U16, NT_S32, NT_U32, NT_S64, NT_U64, NT_F32, NT_F64
} NumType;

typedef struct Obj Obj;
typedef struct ObjString ObjString;
typedef struct ObjInst ObjInst;
typedef struct ObjArray ObjArray;
typedef struct ObjList ObjList;
typedef struct ObjLink ObjLink;

typedef struct {
    uint8_t kind;
    union {
        int32_t i;
        int64_t i64;
        double f;
        ObjString *s;   /* NULL means the empty string */
        ObjInst *o;     /* NULL means Null */
        ObjArray *a;    /* NULL means Null */
        ObjList *l;     /* NULL means Null */
        ObjLink *lk;
        Obj *obj;
    } as;
} Value;

static inline Value v_int(int32_t i) { Value v; v.kind = K_INT; v.as.i = i; return v; }
static inline Value v_long(int64_t l) { Value v; v.kind = K_LONG; v.as.i64 = l; return v; }
static inline Value v_float(double f) { Value v; v.kind = K_FLOAT; v.as.f = f; return v; }
static inline Value v_str(ObjString *s) { Value v; v.kind = K_STRING; v.as.s = s; return v; }
static inline Value v_obj(ObjInst *o) { Value v; v.kind = K_OBJ; v.as.o = o; return v; }
static inline Value v_arr(ObjArray *a) { Value v; v.kind = K_ARRAY; v.as.a = a; return v; }
static inline Value v_list(ObjList *l) { Value v; v.kind = K_LIST; v.as.l = l; return v; }
static inline Value v_link(ObjLink *lk) { Value v; v.kind = K_LINK; v.as.lk = lk; return v; }
static inline bool kind_is_ref(uint8_t k) {
    return k == K_STRING || k == K_OBJ || k == K_ARRAY || k == K_LIST || k == K_LINK;
}

/* ---- Static (compile-time) types ---- */
typedef struct {
    uint8_t base;   /* Kind: K_INT, K_LONG, K_ULONG, K_FLOAT, K_STRING, K_OBJ, K_LIST, K_NULL, K_VOID */
    uint8_t dims;   /* > 0 means an array of `base` with this many dimensions */
    uint8_t elem;   /* base == K_LIST: the element kind (K_INT, K_FLOAT, K_STRING, K_OBJ, ...) */
    uint8_t num;    /* NumType of a numeric base (for a list: of its elements) */
    uint8_t lit;    /* compiler only: a literal/constant, so it adapts to the other operand */
    int16_t tid;    /* type id when base (or, for a list, elem) is K_OBJ */
} VType;

/* The natural (widest) storage type of an arithmetic base. */
static inline uint8_t natural_num(uint8_t base) {
    switch (base) {
        case K_INT: return NT_S32;
        case K_LONG: return NT_S64;
        case K_ULONG: return NT_U64;
        case K_FLOAT: return NT_F64;
        default: return NT_NONE;
    }
}
static inline VType vt(uint8_t base) {
    VType t; t.base = base; t.dims = 0; t.elem = 0; t.num = natural_num(base); t.lit = 0; t.tid = -1; return t;
}

/* ---- Heap objects ---- */
typedef enum { O_STRING, O_INST, O_ARRAY, O_LIST, O_LINK } ObjKind;

struct Obj {
    uint8_t okind;
    uint8_t marked;
    Obj *next;
};

struct ObjString {
    Obj obj;
    int len;
    char chars[];
};

struct ObjInst {
    Obj obj;
    int tid;
    int nfields;
    Value fields[];
};

#define GB_MAX_DIMS 8
struct ObjArray {
    Obj obj;
    uint8_t elemKind;
    uint8_t ndims;
    int dims[GB_MAX_DIMS];
    int count;
    Value data[];
};

/* A typed list ("int list", "t_enemy list"): a doubly linked list of values.
 * A removed link has list == NULL but keeps `next`, so an EachIn loop that
 * removed its current element can still step to the following one. */
struct ObjLink {
    Obj obj;
    ObjList *list;
    ObjLink *prev, *next;
    Value value;
};

struct ObjList {
    Obj obj;
    ObjLink *head, *tail;
    int count;
};

/* ---- Program metadata shared by compiler and VM ---- */
typedef struct {
    char *name;
    char *lname;
    int nfields;
    char **fieldNames;   /* lower-case */
    VType *fieldTypes;
} TypeDef;

typedef struct {
    char *name;
    int nparams;
    VType *params;
    VType ret;
    int nlocals;
    VType *localTypes;
    int entry;           /* code offset */
} FuncDef;

typedef struct VM VM;
typedef Value (*NativeFn)(VM *vm, int argc, Value *args);

typedef struct {
    const char *name;
    char ret;            /* 'v','i','f','s' */
    const char *params;  /* e.g. "iiii|i" : i,f,s ; '|' starts optional args */
    NativeFn fn;
} NativeDef;

typedef struct {
    const char *name;
    int32_t value;
} ConstDef;

typedef struct {
    uint8_t *code;
    int32_t *lines;      /* per code byte: (file << 20) | line */
    int count, cap;

    Value *consts;
    int nconsts, capConsts;

    FuncDef *funcs;
    int nfuncs, capFuncs;

    TypeDef *types;
    int ntypes, capTypes;

    int nglobals;
    VType *globalTypes;

    char **files;
    int nfiles;
} Program;

/* ---- Opcodes ---- */
typedef enum {
    OP_NOP,
    OP_PUSH_INT,      /* i32 */
    OP_PUSH_CONST,    /* idx */
    OP_PUSH_DEFAULT,  /* kind */
    OP_POP,
    OP_DUPN,          /* n */
    OP_LOAD_LOCAL, OP_STORE_LOCAL,
    OP_LOAD_GLOBAL, OP_STORE_GLOBAL,

    OP_ADDI, OP_SUBI, OP_MULI, OP_DIVI, OP_MODI, OP_NEGI,
    OP_ADDF, OP_SUBF, OP_MULF, OP_DIVF, OP_MODF, OP_NEGF, OP_POWF,
    OP_CONCAT,

    OP_EQI, OP_NEI, OP_LTI, OP_GTI, OP_LEI, OP_GEI,
    OP_EQF, OP_NEF, OP_LTF, OP_GTF, OP_LEF, OP_GEF,
    OP_EQS, OP_NES, OP_LTS, OP_GTS, OP_LES, OP_GES,
    OP_EQO, OP_NEO,

    OP_NOT, OP_BAND, OP_BOR, OP_BXOR, OP_BNOT, OP_SHL, OP_SHR, OP_SAR,

    OP_I2F, OP_F2I, OP_I2S, OP_F2S, OP_S2I, OP_S2F,
    OP_I2F_UNDER, OP_I2S_UNDER, OP_F2S_UNDER,  /* convert value below top */
    OP_SWAP,          /* swap the top two values */
    OP_NARROW,        /* NumType: wrap an int/long to a narrower type, or round a float to 32 bits */

    /* 64-bit integers (signed; add/sub/mul/neg/bitwise/eq are the same for unsigned) */
    OP_ADDL, OP_SUBL, OP_MULL, OP_DIVL, OP_MODL, OP_NEGL,
    OP_DIVU, OP_MODU,
    OP_EQL, OP_NEL, OP_LTL, OP_GTL, OP_LEL, OP_GEL,
    OP_LTU, OP_GTU, OP_LEU, OP_GEU,
    OP_BANDL, OP_BORL, OP_BXORL, OP_BNOTL, OP_SHLL, OP_SHRL, OP_SARL,
    OP_I2L, OP_L2I, OP_L2F, OP_U2F, OP_F2L, OP_F2U, OP_L2S, OP_U2S, OP_S2L, OP_S2U,
    OP_TRUTH_L,
    OP_TRUTH_I, OP_TRUTH_F, OP_TRUTH_S, OP_TRUTH_O,

    OP_JMP, OP_JF, OP_JT,   /* absolute target */

    OP_CALL,          /* func, argc */
    OP_NATIVE,        /* native, argc */
    OP_RET,

    OP_NEW,           /* tid */
    OP_GETF, OP_SETF, /* field index */
    OP_ITER_FIRST,    /* list -> its first link (or Null) */
    OP_ITER_NEXT,     /* link -> the next link still in the list (or Null) */
    OP_LINK_VALUE,    /* link -> its value */

    OP_NEWARR,        /* elemKind, ndims */
    OP_AGET, OP_ASET, /* ndims */
    OP_ALEN,          /* pops array, dim(int) */

    OP_END
} OpCode;

/* ---- Helpers ---- */
void *gb_alloc(size_t size);
void *gb_realloc(void *p, size_t size);
char *gb_strdup(const char *s);
char *gb_strndup(const char *s, size_t n);
char *gb_lower(const char *s);

/* ---- Embedding hooks (used by GooberEngine; NULL by default) ----
 * Diagnostics (compile and runtime errors) go through gb_diag(). With no hook
 * they are written to stderr, exactly as before. If gb_error_jmp is set,
 * lexer/compiler errors longjmp there instead of calling exit(1), so a host
 * can report the error and carry on. */
extern void (*gb_diag_hook)(const char *text);
extern jmp_buf *gb_error_jmp;
void gb_diag(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void gb_vdiag(const char *fmt, va_list ap);
void gb_fatal(void) __attribute__((noreturn));
void gb_format_float(double f, char *buf, size_t n);

#endif
