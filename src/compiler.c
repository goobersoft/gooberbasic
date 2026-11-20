/* GooberBASIC compiler: tokens -> bytecode.
 *
 * Two phases:
 *   1. prescan()  - collects every Type (and its Fields), every Function
 *                   signature and every top-level Global/Dim, so code can use
 *                   them before the line where they are written.
 *   2. a single-pass recursive-descent compiler that emits bytecode directly.
 *      Every expression function returns the static type of the value it left
 *      on the stack, which is how typed opcodes and conversions get chosen.
 */
#include "compiler.h"
#include "vm.h"
#include <stdarg.h>
#include <math.h>
#include <ctype.h>

typedef struct { char *lname; VType type; int slot; } Var;

typedef struct {
    char *lname;
    VType type;
    int32_t i;
    int64_t l;
    double f;
    char *s;
    int slen;
} ConstSym;

typedef struct {
    char *lname;
    char *name;
    int fidx;
    int nparams;
    VType *ptypes;
    char **pnames;
    int *pdefault;   /* token index of default expression, or -1 */
    VType ret;
} FuncSym;

typedef struct { int *items; int n, cap; } IntList;
typedef struct { IntList breaks, conts; } Loop;

typedef struct {
    int fidx;
    bool isMain;
    Var *locals;
    int nlocals, cap;
    Loop loops[64];
    int nloops;
} FuncState;

typedef struct { int where; int slot; VType type; ConstSym *c; } VarRef; /* where: 0 local, 1 global, 2 const */

typedef struct {
    int kind;        /* 0 variable, 1 field, 2 array element */
    VarRef var;
    int field;
    int ndims;
    VType type;
} LVal;

typedef struct { VType t; int32_t i; int64_t l; double f; char *s; int slen; } ConstVal;

static struct {
    TokenList *tl;
    int pos;
    Program *prog;
    bool strict;
    int superFrom;     /* token index of the first superstrict (count if none): declarations after it need a type */
    Var *globals; int nglobals, capGlobals;
    ConstSym *consts; int nconsts, capConsts;
    FuncSym *funcs; int nfuncs, capFuncs;
    FuncState *F;
    FuncState mainState;
    int32_t lastLine;
    int depth;
    /* constant folding (see fold()) */
    int starts[4], nstarts;   /* offsets of the most recent instructions */
    int barrier;              /* highest offset a jump may target: never fold across it */
} C;

static VType expr(void);

/* ------------------------------------------------------------------ */
/* errors, tokens                                                      */
/* ------------------------------------------------------------------ */

static Token *peek(void) { return &C.tl->toks[C.pos]; }
static Token *peekn(int n) {
    int p = C.pos + n;
    if (p >= C.tl->count) p = C.tl->count - 1;
    return &C.tl->toks[p];
}
static TokType pt(void) { return peek()->type; }

static void cerror_at(Token *t, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
static void cerror_at(Token *t, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char msg[768];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    gb_diag("%s:%d: error: %s\n", C.prog->files[t->file], t->line, msg);
    gb_fatal();
}
#define cerror(...) cerror_at(peek(), __VA_ARGS__)

static Token *advance(void) {
    Token *t = peek();
    if (t->type != T_EOF) C.pos++;
    C.lastLine = (t->file << 20) | t->line;
    return t;
}

static bool match(TokType t) {
    if (pt() == t) { advance(); return true; }
    return false;
}

static const char *tok_desc(Token *t) {
    static char buf[128];
    if (t->type == T_IDENT) { snprintf(buf, sizeof buf, "'%s'", t->text); return buf; }
    if (t->type == T_STRLIT) return "a string";
    if (t->type == T_INTLIT || t->type == T_FLOATLIT || t->type == T_LONGLIT) return "a number";
    return tok_name(t->type);
}

static Token *expect(TokType t, const char *what) {
    if (pt() != t) cerror("Expected %s but found %s", what, tok_desc(peek()));
    return advance();
}

static bool is_end_tok(TokType t) {
    return t == T_NEWLINE || t == T_COLON || t == T_EOF || t == T_ELSE || t == T_ELSEIF;
}
static bool at_stmt_end(void) { return is_end_tok(pt()); }

/* ------------------------------------------------------------------ */
/* emission                                                            */
/* ------------------------------------------------------------------ */

static void emit_byte(uint8_t b) {
    Program *p = C.prog;
    if (p->count == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 4096;
        p->code = gb_realloc(p->code, (size_t)p->cap);
        p->lines = gb_realloc(p->lines, sizeof(int32_t) * (size_t)p->cap);
    }
    p->code[p->count] = b;
    p->lines[p->count] = C.lastLine;
    p->count++;
}
static void emit_i32(int32_t v) {
    uint8_t b[4];
    memcpy(b, &v, 4);
    for (int i = 0; i < 4; i++) emit_byte(b[i]);
}
static void mark_start(void) {
    if (C.nstarts == 4) { memmove(C.starts, C.starts + 1, 3 * sizeof(int)); C.nstarts = 3; }
    C.starts[C.nstarts++] = C.prog->count;
}
static bool fold(OpCode op);
static void emit_op(OpCode op) { if (fold(op)) return; mark_start(); emit_byte((uint8_t)op); }
static void emit_op1(OpCode op, int32_t a) { mark_start(); emit_byte((uint8_t)op); emit_i32(a); }
static void emit_op2(OpCode op, int32_t a, int32_t b) { mark_start(); emit_byte((uint8_t)op); emit_i32(a); emit_i32(b); }
/* Every jump target in the compiler is taken from here(), so it also
 * raises the folding barrier: code at or after a label is never merged
 * with code before it. */
static int here(void) { C.barrier = C.prog->count; return C.prog->count; }
static int emit_jump(OpCode op) { mark_start(); emit_byte((uint8_t)op); int at = here(); emit_i32(0); return at; }
static void patch_to(int at, int target) { memcpy(C.prog->code + at, &target, 4); }
static void patch_here(int at) { patch_to(at, here()); }

static void il_push(IntList *l, int v) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->items = gb_realloc(l->items, sizeof(int) * (size_t)l->cap); }
    l->items[l->n++] = v;
}
static void il_patch(IntList *l, int target) {
    for (int i = 0; i < l->n; i++) patch_to(l->items[i], target);
    free(l->items);
    l->items = NULL;
    l->n = l->cap = 0;
}

static int add_const(Value v) {
    Program *p = C.prog;
    for (int i = 0; i < p->nconsts; i++) {
        Value c = p->consts[i];
        if (c.kind != v.kind) continue;
        if (v.kind == K_FLOAT && memcmp(&c.as.f, &v.as.f, sizeof(double)) == 0) return i;
        if (v.kind == K_LONG && c.as.i64 == v.as.i64) return i;
        if (v.kind == K_STRING && c.as.s && v.as.s && c.as.s->len == v.as.s->len &&
            memcmp(c.as.s->chars, v.as.s->chars, (size_t)v.as.s->len) == 0) return i;
    }
    if (p->nconsts == p->capConsts) {
        p->capConsts = p->capConsts ? p->capConsts * 2 : 64;
        p->consts = gb_realloc(p->consts, sizeof(Value) * (size_t)p->capConsts);
    }
    p->consts[p->nconsts] = v;
    return p->nconsts++;
}

static ObjString *perm_str(const char *s, int len) {
    if (len <= 0) return NULL;
    ObjString *o = gb_alloc(sizeof(ObjString) + (size_t)len + 1);
    o->obj.okind = O_STRING;
    o->len = len;
    memcpy(o->chars, s, (size_t)len);
    return o;
}

static void emit_float(double f) { emit_op1(OP_PUSH_CONST, add_const(v_float(f))); }
static void emit_long(int64_t l) { emit_op1(OP_PUSH_CONST, add_const(v_long(l))); }
static void emit_string(const char *s, int len) {
    if (len <= 0) { emit_op1(OP_PUSH_DEFAULT, K_STRING); return; }
    emit_op1(OP_PUSH_CONST, add_const(v_str(perm_str(s, len))));
}

/* ------------------------------------------------------------------ */
/* constant folding                                                    */
/* ------------------------------------------------------------------ */
/* A peephole pass run as each operation is emitted: when the instructions
 * just before it are constant pushes, they are removed and the result is
 * pushed instead, so `W / 2 - 10` compiles to a single PUSH_INT. Every
 * rule computes exactly what the VM would (32-bit wrapping ints, masked
 * shift counts, the same number-to-string formatting). Anything that would
 * fail at run time (integer division by zero) is left alone so the error
 * still happens, with its line number. */

typedef struct { bool ok; uint8_t kind; int32_t i; int64_t l; double f; const char *s; int slen; } KVal;

/* The constant pushed by the instruction at `start`, if it is a constant push. */
static KVal const_at(int start) {
    KVal k = {0};
    uint8_t *c = C.prog->code + start;
    int32_t a;
    memcpy(&a, c + 1, 4);
    if (c[0] == OP_PUSH_INT) { k.ok = true; k.kind = K_INT; k.i = a; }
    else if (c[0] == OP_PUSH_CONST) {
        Value v = C.prog->consts[a];
        if (v.kind == K_FLOAT) { k.ok = true; k.kind = K_FLOAT; k.f = v.as.f; }
        else if (v.kind == K_LONG) { k.ok = true; k.kind = K_LONG; k.l = v.as.i64; }
        else if (v.kind == K_STRING) { k.ok = true; k.kind = K_STRING; k.s = str_chars(v.as.s); k.slen = str_len(v.as.s); }
    } else if (c[0] == OP_PUSH_DEFAULT && a == K_STRING) { k.ok = true; k.kind = K_STRING; k.s = ""; k.slen = 0; }
    return k;
}

/* The last `n` instructions, if they are all constant pushes with no label among them. */
static bool last_consts(int n, KVal *out) {
    if (C.nstarts < n) return false;
    int first = C.starts[C.nstarts - n];
    if (first < C.barrier) return false;
    for (int i = 0; i < n; i++) {
        out[i] = const_at(C.starts[C.nstarts - n + i]);
        if (!out[i].ok) return false;
    }
    return true;
}

/* Removes the last `n` instructions. */
static void drop_last(int n) {
    C.prog->count = C.starts[C.nstarts - n];
    C.nstarts -= n;
}

static void push_kval(KVal k) {
    if (k.kind == K_INT) emit_op1(OP_PUSH_INT, k.i);
    else if (k.kind == K_FLOAT) emit_float(k.f);
    else if (k.kind == K_LONG) emit_long(k.l);
    else emit_string(k.s, k.slen);
}

static KVal kint(int32_t i) { KVal k = {0}; k.ok = true; k.kind = K_INT; k.i = i; return k; }
static KVal kfloat(double f) { KVal k = {0}; k.ok = true; k.kind = K_FLOAT; k.f = f; return k; }
static KVal klong(int64_t l) { KVal k = {0}; k.ok = true; k.kind = K_LONG; k.l = l; return k; }

/* Same as the VM's f2l / f2u. */
static int64_t fold_f2l(double f) {
    if (f != f) return 0;
    if (f >= 9223372036854775807.0) return INT64_MAX;
    if (f <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)f;
}
static uint64_t fold_f2u(double f) {
    if (!(f > 0)) return 0;
    if (f >= 18446744073709551616.0) return UINT64_MAX;
    return (uint64_t)f;
}

/* Same as the VM's f2i: saturating, NaN -> 0. */
static int32_t fold_f2i(double f) {
    if (f != f) return 0;
    if (f >= 2147483647.0) return 2147483647;
    if (f <= -2147483648.0) return (int32_t)0x80000000u;
    return (int32_t)f;
}

/* Strings made while folding live in a scratch buffer until emit_string copies them. */
static KVal kstr_buf(const char *s, int len) {
    static char buf[4][512];
    static int which = 0;
    KVal k = {0};
    if (len >= (int)sizeof buf[0]) return k;          /* too long: don't fold */
    char *b = buf[which++ & 3];
    memcpy(b, s, (size_t)len);
    b[len] = 0;
    k.ok = true; k.kind = K_STRING; k.s = b; k.slen = len;
    return k;
}
static KVal kstr_of(KVal v) {
    char t[64];
    if (v.kind == K_INT) snprintf(t, sizeof t, "%d", v.i);
    else if (v.kind == K_LONG) snprintf(t, sizeof t, "%lld", (long long)v.l);
    else if (v.kind == K_ULONG) snprintf(t, sizeof t, "%llu", (unsigned long long)(uint64_t)v.l);
    else gb_format_float(v.f, t, sizeof t);
    return kstr_buf(t, (int)strlen(t));
}

static bool is_bool_op(uint8_t op) {
    return (op >= OP_EQI && op <= OP_NEO) || op == OP_NOT || (op >= OP_EQL && op <= OP_GEU) || op == OP_TRUTH_L ||
           op == OP_TRUTH_I || op == OP_TRUTH_F || op == OP_TRUTH_S || op == OP_TRUTH_O;
}

/* Folding for SWAP and the 64-bit opcodes. Returns -1 if `op` is not one of them. */
static int fold_long(OpCode op) {
    KVal k[2];
    switch (op) {
        case OP_SWAP: {
            /* SWAP SWAP does nothing */
            if (C.nstarts >= 1 && C.starts[C.nstarts - 1] >= C.barrier && C.prog->code[C.starts[C.nstarts - 1]] == OP_SWAP) {
                drop_last(1);
                return 1;
            }
            if (!last_consts(2, k)) return 0;
            KVal a = k[0], b = k[1];
            if (a.kind == K_STRING) a = kstr_buf(a.s, a.slen);
            if (b.kind == K_STRING) b = kstr_buf(b.s, b.slen);
            if (!a.ok || !b.ok) return 0;
            drop_last(2);
            push_kval(b);
            push_kval(a);
            return 1;
        }
        case OP_I2L: case OP_L2I: case OP_L2F: case OP_U2F: case OP_F2L: case OP_F2U:
        case OP_L2S: case OP_U2S: case OP_NEGL: case OP_BNOTL: case OP_TRUTH_L: {
            if (!last_consts(1, k)) return 0;
            KVal a = k[0], r;
            uint8_t want = op == OP_I2L ? K_INT : (op == OP_F2L || op == OP_F2U) ? K_FLOAT : K_LONG;
            if (a.kind != want) return 0;
            switch (op) {
                case OP_I2L: r = klong(a.i); break;
                case OP_L2I: r = kint((int32_t)(uint32_t)(uint64_t)a.l); break;
                case OP_L2F: r = kfloat((double)a.l); break;
                case OP_U2F: r = kfloat((double)(uint64_t)a.l); break;
                case OP_F2L: r = klong(fold_f2l(a.f)); break;
                case OP_F2U: r = klong((int64_t)fold_f2u(a.f)); break;
                case OP_L2S: r = kstr_of(a); break;
                case OP_U2S: a.kind = K_ULONG; r = kstr_of(a); break;
                case OP_NEGL: r = klong((int64_t)(0u - (uint64_t)a.l)); break;
                case OP_BNOTL: r = klong(~a.l); break;
                default: r = kint(a.l != 0); break;     /* TRUTH_L */
            }
            if (!r.ok) return 0;
            drop_last(1);
            push_kval(r);
            return 1;
        }
        default: break;
    }
    if (op < OP_ADDL || op > OP_SARL || op == OP_NEGL || op == OP_BNOTL) return -1;
    if (!last_consts(2, k)) return 0;
    if (k[0].kind != K_LONG || k[1].kind != K_LONG) return 0;
    int64_t a = k[0].l, b = k[1].l;
    uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
    KVal r;
    switch (op) {
        case OP_ADDL: r = klong((int64_t)(ua + ub)); break;
        case OP_SUBL: r = klong((int64_t)(ua - ub)); break;
        case OP_MULL: r = klong((int64_t)(ua * ub)); break;
        case OP_DIVL: if (b == 0) return 0; r = klong(b == -1 ? (int64_t)(0u - ua) : a / b); break;
        case OP_MODL: if (b == 0) return 0; r = klong(b == -1 ? 0 : a % b); break;
        case OP_DIVU: if (b == 0) return 0; r = klong((int64_t)(ua / ub)); break;
        case OP_MODU: if (b == 0) return 0; r = klong((int64_t)(ua % ub)); break;
        case OP_EQL: r = kint(a == b); break;
        case OP_NEL: r = kint(a != b); break;
        case OP_LTL: r = kint(a < b); break;
        case OP_GTL: r = kint(a > b); break;
        case OP_LEL: r = kint(a <= b); break;
        case OP_GEL: r = kint(a >= b); break;
        case OP_LTU: r = kint(ua < ub); break;
        case OP_GTU: r = kint(ua > ub); break;
        case OP_LEU: r = kint(ua <= ub); break;
        case OP_GEU: r = kint(ua >= ub); break;
        case OP_BANDL: r = klong(a & b); break;
        case OP_BORL: r = klong(a | b); break;
        case OP_BXORL: r = klong(a ^ b); break;
        case OP_SHLL: r = klong((int64_t)(ua << (b & 63))); break;
        case OP_SHRL: r = klong((int64_t)(ua >> (b & 63))); break;
        default: r = klong(a >> (b & 63)); break;       /* SARL */
    }
    drop_last(2);
    push_kval(r);
    return 1;
}

static bool fold(OpCode op) {
    KVal k[2];
    int fl = fold_long(op);
    if (fl >= 0) return fl == 1;
    /* TRUTH_I after something that already gives 0 or 1 does nothing */
    if (op == OP_TRUTH_I && C.nstarts >= 1 && C.starts[C.nstarts - 1] >= C.barrier &&
        is_bool_op(C.prog->code[C.starts[C.nstarts - 1]]))
        return true;

    switch (op) {
        /* ---- one operand ---- */
        case OP_I2F: case OP_F2I: case OP_I2S: case OP_F2S: case OP_NEGI: case OP_NEGF:
        case OP_NOT: case OP_BNOT: case OP_TRUTH_I: case OP_TRUTH_F: case OP_TRUTH_S: {
            if (!last_consts(1, k)) return false;
            KVal a = k[0], r;
            uint8_t want = (op == OP_F2I || op == OP_F2S || op == OP_NEGF || op == OP_TRUTH_F) ? K_FLOAT
                         : op == OP_TRUTH_S ? K_STRING : K_INT;
            if (a.kind != want) return false;
            switch (op) {
                case OP_I2F: r = kfloat((double)a.i); break;
                case OP_F2I: r = kint(fold_f2i(a.f)); break;
                case OP_I2S: case OP_F2S: r = kstr_of(a); break;
                case OP_NEGI: r = kint((int32_t)(0u - (uint32_t)a.i)); break;
                case OP_NEGF: r = kfloat(-a.f); break;
                case OP_NOT: r = kint(a.i == 0); break;
                case OP_BNOT: r = kint(~a.i); break;
                case OP_TRUTH_I: r = kint(a.i != 0); break;
                case OP_TRUTH_F: r = kint(a.f != 0.0); break;
                default: r = kint(a.slen != 0); break;      /* TRUTH_S */
            }
            if (!r.ok) return false;
            drop_last(1);
            push_kval(r);
            return true;
        }
        /* ---- convert the value under the top ---- */
        case OP_I2F_UNDER: case OP_I2S_UNDER: case OP_F2S_UNDER: {
            if (!last_consts(2, k)) return false;
            uint8_t want = op == OP_F2S_UNDER ? K_FLOAT : K_INT;
            if (k[0].kind != want) return false;
            KVal r = op == OP_I2F_UNDER ? kfloat((double)k[0].i) : kstr_of(k[0]);
            if (!r.ok) return false;
            KVal top = k[1];
            if (top.kind == K_STRING) top = kstr_buf(top.s, top.slen);  /* keep a copy before truncating */
            if (!top.ok) return false;
            drop_last(2);
            push_kval(r);
            push_kval(top);
            return true;
        }
        default: break;
    }

    /* ---- two operands ---- */
    bool intOp = (op >= OP_ADDI && op <= OP_MODI) || (op >= OP_EQI && op <= OP_GEI) ||
                 op == OP_BAND || op == OP_BOR || op == OP_BXOR || op == OP_SHL || op == OP_SHR || op == OP_SAR;
    bool fltOp = (op >= OP_ADDF && op <= OP_MODF) || op == OP_POWF || (op >= OP_EQF && op <= OP_GEF);
    if (!intOp && !fltOp && op != OP_CONCAT) return false;
    if (!last_consts(2, k)) return false;
    KVal a = k[0], b = k[1], r = {0};
    if (op == OP_CONCAT) {
        if (a.kind != K_STRING || b.kind != K_STRING) return false;
        char t[1024];
        if (a.slen + b.slen >= (int)sizeof t) return false;
        memcpy(t, a.s, (size_t)a.slen);
        memcpy(t + a.slen, b.s, (size_t)b.slen);
        r = kstr_buf(t, a.slen + b.slen);
    } else if (intOp) {
        if (a.kind != K_INT || b.kind != K_INT) return false;
        uint32_t ua = (uint32_t)a.i, ub = (uint32_t)b.i;
        switch (op) {
            case OP_ADDI: r = kint((int32_t)(ua + ub)); break;
            case OP_SUBI: r = kint((int32_t)(ua - ub)); break;
            case OP_MULI: r = kint((int32_t)(ua * ub)); break;
            case OP_DIVI: if (b.i == 0) return false;
                          r = kint(b.i == -1 ? (int32_t)(0u - ua) : a.i / b.i); break;
            case OP_MODI: if (b.i == 0) return false;
                          r = kint(b.i == -1 ? 0 : a.i % b.i); break;
            case OP_EQI: r = kint(a.i == b.i); break;
            case OP_NEI: r = kint(a.i != b.i); break;
            case OP_LTI: r = kint(a.i < b.i); break;
            case OP_GTI: r = kint(a.i > b.i); break;
            case OP_LEI: r = kint(a.i <= b.i); break;
            case OP_GEI: r = kint(a.i >= b.i); break;
            case OP_BAND: r = kint(a.i & b.i); break;
            case OP_BOR: r = kint(a.i | b.i); break;
            case OP_BXOR: r = kint(a.i ^ b.i); break;
            case OP_SHL: r = kint((int32_t)(ua << (b.i & 31))); break;
            case OP_SHR: r = kint((int32_t)(ua >> (b.i & 31))); break;
            default: r = kint(a.i >> (b.i & 31)); break;   /* SAR */
        }
    } else {
        if (a.kind != K_FLOAT || b.kind != K_FLOAT) return false;
        switch (op) {
            case OP_ADDF: r = kfloat(a.f + b.f); break;
            case OP_SUBF: r = kfloat(a.f - b.f); break;
            case OP_MULF: r = kfloat(a.f * b.f); break;
            case OP_DIVF: r = kfloat(a.f / b.f); break;
            case OP_MODF: r = kfloat(fmod(a.f, b.f)); break;
            case OP_POWF: r = kfloat(pow(a.f, b.f)); break;
            case OP_EQF: r = kint(a.f == b.f); break;
            case OP_NEF: r = kint(a.f != b.f); break;
            case OP_LTF: r = kint(a.f < b.f); break;
            case OP_GTF: r = kint(a.f > b.f); break;
            case OP_LEF: r = kint(a.f <= b.f); break;
            default: r = kint(a.f >= b.f); break;           /* GEF */
        }
    }
    if (!r.ok) return false;
    drop_last(2);
    push_kval(r);
    return true;
}

/* ------------------------------------------------------------------ */
/* types                                                               */
/* ------------------------------------------------------------------ */

static bool is_numk(uint8_t b) { return b == K_INT || b == K_LONG || b == K_ULONG || b == K_FLOAT; }
static bool is_intk(uint8_t b) { return b == K_INT || b == K_LONG || b == K_ULONG; }
static bool is_num(VType t) { return t.dims == 0 && is_numk(t.base); }
static bool is_integer(VType t) { return t.dims == 0 && is_intk(t.base); }

/* Arithmetic widths in promotion order: int < long < ulong < float. */
static int num_rank(uint8_t b) { return b == K_INT ? 0 : b == K_LONG ? 1 : b == K_ULONG ? 2 : 3; }

/* The built-in numeric type names. */
static const struct { const char *name; uint8_t num; } NUM_TYPES[] = {
    {"int", NT_S32}, {"byte", NT_U8}, {"short", NT_U16}, {"long", NT_U64},
    {"float", NT_F32}, {"double", NT_F64},
    {"uint8", NT_U8}, {"sint8", NT_S8}, {"uint16", NT_U16}, {"sint16", NT_S16},
    {"uint32", NT_U32}, {"sint32", NT_S32}, {"uint64", NT_U64}, {"sint64", NT_S64},
    {NULL, 0}
};

static int num_type_find(const char *lname) {
    for (int i = 0; NUM_TYPES[i].name; i++) if (!strcmp(NUM_TYPES[i].name, lname)) return NUM_TYPES[i].num;
    return NT_NONE;
}

static uint8_t num_base(uint8_t nt) {
    switch (nt) {
        case NT_U32: case NT_S64: return K_LONG;
        case NT_U64: return K_ULONG;
        case NT_F32: case NT_F64: return K_FLOAT;
        default: return K_INT;
    }
}

static VType vt_num(uint8_t nt) { VType t = vt(num_base(nt)); t.num = nt; return t; }

static const char *num_name(uint8_t nt) {
    static const char *names[] = {"?", "sint8", "byte", "sint16", "short", "int", "uint32", "sint64", "long", "float", "double"};
    return nt <= NT_F64 ? names[nt] : "?";
}

/* Integer storage types: is every value of `a` also a value of `b`? */
static bool int_fits(uint8_t a, uint8_t b) {
    static const int bits[] = {0, 8, 8, 16, 16, 32, 32, 64, 64};
    static const bool uns[] = {0, 0, 1, 0, 1, 0, 1, 0, 1};
    if (uns[a] == uns[b]) return bits[a] <= bits[b];
    return uns[a] && bits[a] < bits[b];
}
static bool is_str(VType t) { return t.dims == 0 && t.base == K_STRING; }
static bool is_ref(VType t) { return t.dims > 0 || t.base == K_OBJ || t.base == K_NULL || t.base == K_LIST; }
static bool is_obj(VType t) { return t.dims == 0 && t.base == K_OBJ; }

static uint8_t runtime_kind(VType t) {
    if (t.dims) return K_ARRAY;
    if (t.base == K_NULL) return K_OBJ;
    if (t.base == K_ULONG) return K_LONG;
    return t.base;
}

static const char *type_str(VType t) {
    static char bufs[8][160];
    static int which = 0;
    char *b = bufs[which++ & 7];
    const char *base;
    char lbuf[80];
    switch (t.base) {
        case K_INT: case K_LONG: case K_ULONG: case K_FLOAT: base = num_name(t.num); break;
        case K_STRING: base = "string"; break;
        case K_OBJ: base = t.tid >= 0 ? C.prog->types[t.tid].name : "object"; break;
        case K_NULL: base = "null"; break;
        case K_LIST: {
            VType e = vt(t.elem);
            e.tid = t.tid;
            e.num = t.num;
            snprintf(lbuf, sizeof lbuf, "%s list", type_str(e));
            base = lbuf;
            break;
        }
        case K_VOID: base = "nothing"; break;
        default: base = "?"; break;
    }
    if (t.dims) {
        char d[24] = "[";
        for (int i = 1; i < t.dims; i++) strcat(d, ",");
        strcat(d, "]");
        snprintf(b, 160, "%s%s", base, d);
    } else snprintf(b, 160, "%s", base);
    return b;
}

static bool vt_eq(VType a, VType b) {
    if (a.base != b.base || a.dims != b.dims || a.num != b.num) return false;
    if (a.base == K_OBJ) return a.tid == b.tid;
    if (a.base == K_LIST) return a.elem == b.elem && (a.elem != K_OBJ || a.tid == b.tid);
    return true;
}

static int find_type(const char *lname) {
    for (int i = 0; i < C.prog->ntypes; i++)
        if (strcmp(C.prog->types[i].lname, lname) == 0) return i;
    return -1;
}

static bool is_type_name(const char *l) {
    if (num_type_find(l) || !strcmp(l, "string")) return true;
    return find_type(l) >= 0;
}

/* List commands are compiler intrinsics (they check the element type).
 * Order matters: the first four take a value. */
static const char *const LIST_FNS[] = {
    "list_add_first", "list_add_last", "list_remove", "list_contains",
    "count_list", "list_is_empty", "clear_list", NULL
};

static const char *renamed_builtin(const char *text);
static void unknown_name(Token *t, const char *what) __attribute__((noreturn));

static VType resolve_type_name(Token *t) {
    const char *l = t->lower;
    int nt = num_type_find(l);
    if (nt) return vt_num((uint8_t)nt);
    if (!strcmp(l, "string")) return vt(K_STRING);
    int tid = find_type(l);
    if (tid < 0) unknown_name(t, "type");
    VType r = vt(K_OBJ);
    r.tid = (int16_t)tid;
    return r;
}

static bool is_old_tlist(const char *lower) { return !strcmp(lower, "t_list") || !strcmp(lower, "tlist"); }

static void old_tlist_error(Token *t) __attribute__((noreturn));
static void old_tlist_error(Token *t) {
    cerror_at(t, "'%s' was replaced by typed lists. Put the element type before 'list', "
                 "e.g. local enemies:t_enemy list = new t_enemy list", t->text);
}

static bool at_list_word(void) { return pt() == T_IDENT && !strcmp(peek()->lower, "list"); }

/* A type name (already consumed) optionally followed by `list`. */
static VType parse_type(Token *tn) {
    VType t = resolve_type_name(tn);
    if (!at_list_word()) return t;
    advance();
    if (at_list_word()) cerror_at(peek(), "Lists of lists are not supported");
    VType l = vt(K_LIST);
    l.elem = t.base;
    l.num = t.num;
    l.tid = t.tid;
    return l;
}

static VType list_elem(VType l) {
    VType e = vt(l.elem);
    e.tid = l.tid;
    e.num = l.num;
    return e;
}

static VType suffix_type(char s) {
    if (s == '#') return vt_num(NT_F32);
    if (s == '$') return vt(K_STRING);
    return vt(K_INT);
}

/* Parses an optional ":type" after a name (the name's suffix also counts). */
static VType parse_annot(Token *name, bool *explicitOut) {
    VType t = vt(K_INT);
    bool ex = false;
    if (pt() == T_COLON && peekn(1)->type == T_IDENT && is_old_tlist(peekn(1)->lower)) old_tlist_error(peekn(1));
    if (name->suffix) { t = suffix_type(name->suffix); ex = true; }
    if (pt() == T_COLON && peekn(1)->type == T_IDENT && is_type_name(peekn(1)->lower)) {
        advance();
        t = parse_type(advance());
        ex = true;
    }
    if (explicitOut) *explicitOut = ex;
    return t;
}

/* superstrict works by position, not by a flag set as statements run: the
 * prescan reads function headers, fields and globals before any statement,
 * so "is this name after the superstrict line?" is the one test both passes share. */
static bool superstrict_at(const Token *t) { return t - C.tl->toks >= C.superFrom; }

/* parse_annot, but under superstrict a missing type is an error instead of a quiet int.
 * `example` is how the declaration would look with a type, with %s for the name. */
static VType parse_typed(Token *name, const char *example) {
    bool ex;
    VType t = parse_annot(name, &ex);
    if (!ex && superstrict_at(name)) {
        char eg[160];
        snprintf(eg, sizeof eg, example, name->text);
        cerror_at(name, "'%s' needs a type, e.g. %s (superstrict)", name->text, eg);
    }
    return t;
}

/* what a function leaves on the stack when it ends without a value: a function
 * with no return type under superstrict returns nothing, but every call still pops one */
static uint8_t ret_kind(VType ret) { return ret.base == K_VOID && !ret.dims ? K_INT : runtime_kind(ret); }

/* At '[': if the brackets are empty ("[]" or "[,]") consume them and return
 * the number of dimensions. Returns 0 if not at '[', -1 if the brackets
 * contain expressions (left unconsumed). */
static int parse_empty_dims(void) {
    if (pt() != T_LBRACK) return 0;
    int p = 1;
    while (peekn(p)->type == T_COMMA) p++;
    if (peekn(p)->type != T_RBRACK) return -1;
    advance();
    int dims = 1;
    while (match(T_COMMA)) dims++;
    expect(T_RBRACK, "']'");
    if (dims > GB_MAX_DIMS) cerror("Too many array dimensions (max %d)", GB_MAX_DIMS);
    return dims;
}

/* NARROW, folded when the value is a constant. */
static void emit_narrow(uint8_t nt) {
    KVal k[1];
    if (last_consts(1, k)) {
        KVal r = k[0];
        bool ok = true;
        switch (nt) {
            case NT_S8: r.i = (int8_t)r.i; ok = r.kind == K_INT; break;
            case NT_U8: r.i = (uint8_t)r.i; ok = r.kind == K_INT; break;
            case NT_S16: r.i = (int16_t)r.i; ok = r.kind == K_INT; break;
            case NT_U16: r.i = (uint16_t)r.i; ok = r.kind == K_INT; break;
            case NT_U32: r.l = (int64_t)(uint32_t)r.l; ok = r.kind == K_LONG; break;
            case NT_F32: r.f = (double)(float)r.f; ok = r.kind == K_FLOAT; break;
            default: ok = false; break;
        }
        if (ok) { drop_last(1); push_kval(r); return; }
    }
    emit_op1(OP_NARROW, nt);
}

/* Converts the number on top of the stack from one arithmetic width to another. */
static void emit_widths(uint8_t from, uint8_t to) {
    if (from == to) return;
    switch (to) {
        case K_INT: emit_op(from == K_FLOAT ? OP_F2I : OP_L2I); return;
        case K_LONG: case K_ULONG:
            if (from == K_INT) emit_op(OP_I2L);
            else if (from == K_FLOAT) emit_op(to == K_LONG ? OP_F2L : OP_F2U);
            return;   /* long <-> ulong: same bits */
        case K_FLOAT: emit_op(from == K_INT ? OP_I2F : from == K_LONG ? OP_L2F : OP_U2F); return;
    }
}

/* Number -> number, wrapping (or rounding) into the target's storage type. */
static void emit_num_convert(VType from, VType to) {
    emit_widths(from.base, to.base);
    if (to.num == natural_num(to.base)) return;
    if (to.num == NT_F32) {
        if (from.base == K_FLOAT && from.num == NT_F32) return;
        /* 8- and 16-bit integers are exact in a 32-bit float */
        if (from.base == K_INT && from.num != NT_S32) return;
    } else if (is_intk(from.base) && int_fits(from.num, to.num)) return;
    emit_narrow(to.num);
}

/* Number on top of the stack -> string. */
static void emit_num_to_str(VType from) {
    switch (from.base) {
        case K_INT: emit_op(OP_I2S); break;
        case K_LONG: emit_op(OP_L2S); break;
        case K_ULONG: emit_op(OP_U2S); break;
        default: emit_op(OP_F2S); break;
    }
}

/* The same conversions for the value under the top of the stack. */
static void num_convert_under(VType from, VType to) {
    if (from.base == K_INT && to.base == K_FLOAT && (to.num == NT_F64 || from.num != NT_S32)) { emit_op(OP_I2F_UNDER); return; }
    emit_op(OP_SWAP);
    emit_num_convert(from, to);
    emit_op(OP_SWAP);    /* folds away with the first one if nothing was needed */
}
static void num_to_str_under(VType from) {
    if (from.base == K_INT) { emit_op(OP_I2S_UNDER); return; }
    if (from.base == K_FLOAT) { emit_op(OP_F2S_UNDER); return; }
    emit_op(OP_SWAP);
    emit_num_to_str(from);
    emit_op(OP_SWAP);
}

/* Emits whatever conversion turns a value of type `from` (on top of the stack) into `to`. */
static void coerce(VType from, VType to, const char *ctx) {
    if (to.base == K_VOID) cerror("%s: nothing to assign to", ctx);
    if (from.base == K_VOID) cerror("%s: expression does not return a value", ctx);
    if (to.dims || from.dims) {
        if (from.base == K_NULL && !from.dims) return;
        if (vt_eq(from, to)) return;
        cerror("%s: cannot use %s as %s", ctx, type_str(from), type_str(to));
    }
    if (is_numk(to.base)) {
        if (is_numk(from.base)) { emit_num_convert(from, to); return; }
        if (from.base == K_STRING) {
            switch (to.base) {
                case K_INT: emit_op(OP_S2I); break;
                case K_LONG: emit_op(OP_S2L); break;
                case K_ULONG: emit_op(OP_S2U); break;
                default: emit_op(OP_S2F); break;
            }
            emit_num_convert(vt(to.base), to);
            return;
        }
    }
    switch (to.base) {
        case K_STRING:
            if (from.base == K_STRING) return;
            if (is_numk(from.base)) { emit_num_to_str(from); return; }
            break;
        case K_OBJ:
            if (from.base == K_NULL) return;
            if (from.base == K_OBJ && from.tid == to.tid) return;
            break;
        case K_LIST:
            if (from.base == K_NULL || vt_eq(from, to)) return;
            break;
    }
    cerror("%s: cannot use %s as %s", ctx, type_str(from), type_str(to));
}

/* Turn the value on top of the stack into an Int usable as a condition. */
static void to_cond(VType t) {
    if (t.base == K_VOID) cerror("Expression does not return a value");
    if (is_ref(t)) emit_op(OP_TRUTH_O);
    else if (t.base == K_FLOAT) emit_op(OP_TRUTH_F);
    else if (t.base == K_LONG || t.base == K_ULONG) emit_op(OP_TRUTH_L);
    else if (t.base == K_STRING) emit_op(OP_TRUTH_S);
}

static void to_bool(VType t) {
    if (t.dims == 0 && t.base == K_INT) emit_op(OP_TRUTH_I);
    else to_cond(t);
}

/* ------------------------------------------------------------------ */
/* symbols                                                             */
/* ------------------------------------------------------------------ */

static int declare_local_slot(const char *lname, VType t, Token *at) {
    FuncState *F = C.F;
    if (lname) {
        for (int i = 0; i < F->nlocals; i++)
            if (F->locals[i].lname && strcmp(F->locals[i].lname, lname) == 0) {
                if (!vt_eq(F->locals[i].type, t))
                    cerror_at(at, "'%s' was already declared as %s", at->text, type_str(F->locals[i].type));
                return F->locals[i].slot;
            }
    }
    if (F->nlocals == F->cap) {
        F->cap = F->cap ? F->cap * 2 : 16;
        F->locals = gb_realloc(F->locals, sizeof(Var) * (size_t)F->cap);
    }
    Var *v = &F->locals[F->nlocals];
    v->lname = lname ? gb_strdup(lname) : NULL;
    v->type = t;
    v->type.lit = 0;
    v->slot = F->nlocals;
    return F->nlocals++;
}

static int hidden_local(VType t) { return declare_local_slot(NULL, t, NULL); }

static Var *find_global(const char *lname) {
    for (int i = 0; i < C.nglobals; i++)
        if (strcmp(C.globals[i].lname, lname) == 0) return &C.globals[i];
    return NULL;
}

static ConstSym *find_const(const char *lname) {
    for (int i = 0; i < C.nconsts; i++)
        if (strcmp(C.consts[i].lname, lname) == 0) return &C.consts[i];
    return NULL;
}

static FuncSym *find_func(const char *lname) {
    for (int i = 0; i < C.nfuncs; i++)
        if (strcmp(C.funcs[i].lname, lname) == 0) return &C.funcs[i];
    return NULL;
}

static bool lookup_var(const char *lname, VarRef *out) {
    FuncState *F = C.F;
    for (int i = 0; i < F->nlocals; i++)
        if (F->locals[i].lname && strcmp(F->locals[i].lname, lname) == 0) {
            out->where = 0; out->slot = F->locals[i].slot; out->type = F->locals[i].type; out->c = NULL;
            return true;
        }
    Var *g = find_global(lname);
    if (g) { out->where = 1; out->slot = g->slot; out->type = g->type; out->c = NULL; return true; }
    ConstSym *c = find_const(lname);
    if (c) { out->where = 2; out->slot = -1; out->type = c->type; out->c = c; return true; }
    return false;
}

static void check_suffix(Token *name, VType t) {
    if (!name->suffix || t.dims) return;
    VType s = suffix_type(name->suffix);
    if (s.base != t.base)
        cerror_at(name, "'%s' is %s, but the suffix '%c' says %s", name->text, type_str(t), name->suffix, type_str(s));
}

/* Built-in names are snake_case. If `text` is another spelling of one
 * (DrawRect, drawrect, TList), returns the real name so errors can say so. */
static const char *renamed_builtin(const char *text) {
    char key[128];
    int n = 0;
    for (const char *p = text; *p && n < 127; p++)
        if (*p != '_') key[n++] = (char)tolower((unsigned char)*p);
    key[n] = 0;
    for (int i = 0; LIST_FNS[i]; i++) {
        int k = 0;
        const char *p = LIST_FNS[i];
        for (; *p; p++) {
            if (*p == '_') continue;
            if (*p != key[k]) break;
            k++;
        }
        if (!*p && !key[k]) return LIST_FNS[i];
    }
    for (int i = 0; i < native_count(); i++) {
        const char *nm = native_get(i)->name;
        if (nm[0] == '_') continue;
        int k = 0;
        const char *p = nm;
        for (; *p; p++) {
            if (*p == '_') continue;
            if (*p != key[k]) break;
            k++;
        }
        if (!*p && !key[k]) return nm;
    }
    return NULL;
}

static bool looks_camel(const char *text) {
    if (strchr(text, '_')) return false;
    for (const char *p = text; *p; p++) if (isupper((unsigned char)*p)) return true;
    return false;
}

/* Errors for an unknown name, adding "did you mean" for old built-in spellings. */
static void unknown_name(Token *t, const char *what) {
    const char *nm = renamed_builtin(t->text);
    if (nm) cerror_at(t, "Unknown %s '%s'. Built-in names are snake_case: did you mean %s?", what, t->text, nm);
    cerror_at(t, "Unknown %s '%s'", what, t->text);
}

static VarRef auto_var(Token *name) {
    if (C.strict) {
        if (renamed_builtin(name->text)) unknown_name(name, "name");
        cerror_at(name, "Identifier '%s' has not been declared (strict mode)", name->text);
    }
    /* Without strict an unknown name becomes a variable, so "x = MouseX"
     * would silently read 0. Catch old CamelCase built-in names. */
    if (looks_camel(name->text) && renamed_builtin(name->text)) unknown_name(name, "name");
    VarRef r;
    r.where = 0;
    r.type = name->suffix ? suffix_type(name->suffix) : vt(K_INT);
    r.slot = declare_local_slot(name->lower, r.type, name);
    r.c = NULL;
    return r;
}

static void add_const_sym(Token *name, ConstVal v) {
    if (find_const(name->lower)) cerror_at(name, "Constant '%s' is already defined", name->text);
    if (C.nconsts == C.capConsts) {
        C.capConsts = C.capConsts ? C.capConsts * 2 : 64;
        C.consts = gb_realloc(C.consts, sizeof(ConstSym) * (size_t)C.capConsts);
    }
    ConstSym *c = &C.consts[C.nconsts++];
    c->lname = gb_strdup(name->lower);
    c->type = v.t;
    c->i = v.i;
    c->l = v.l;
    c->f = v.f;
    c->s = v.s;
    c->slen = v.slen;
}

static void add_builtin_const(const char *name, VType t, int32_t i, double f) {
    if (C.nconsts == C.capConsts) {
        C.capConsts = C.capConsts ? C.capConsts * 2 : 64;
        C.consts = gb_realloc(C.consts, sizeof(ConstSym) * (size_t)C.capConsts);
    }
    ConstSym *c = &C.consts[C.nconsts++];
    c->lname = gb_lower(name);
    c->type = t;
    c->type.lit = 1;     /* untyped: adapts to the other operand, like a literal */
    c->i = i;
    c->l = 0;
    c->f = f;
    c->s = NULL;
    c->slen = 0;
}

/* ------------------------------------------------------------------ */
/* variable access                                                     */
/* ------------------------------------------------------------------ */

static void emit_load_var(VarRef *r) {
    if (r->where == 0) emit_op1(OP_LOAD_LOCAL, r->slot);
    else if (r->where == 1) emit_op1(OP_LOAD_GLOBAL, r->slot);
    else {
        ConstSym *c = r->c;
        if (c->type.base == K_INT) emit_op1(OP_PUSH_INT, c->i);
        else if (c->type.base == K_LONG || c->type.base == K_ULONG) emit_long(c->l);
        else if (c->type.base == K_FLOAT) emit_float(c->f);
        else emit_string(c->s, c->slen);
    }
}

static void emit_store_var(VarRef *r) {
    if (r->where == 0) emit_op1(OP_STORE_LOCAL, r->slot);
    else if (r->where == 1) emit_op1(OP_STORE_GLOBAL, r->slot);
    else cerror("Cannot assign to a constant");
}

static void emit_lval_load(LVal *lv) {
    if (lv->kind == 0) emit_load_var(&lv->var);
    else if (lv->kind == 1) emit_op1(OP_GETF, lv->field);
    else emit_op1(OP_AGET, lv->ndims);
}

static void emit_lval_store(LVal *lv) {
    if (lv->kind == 0) emit_store_var(&lv->var);
    else if (lv->kind == 1) emit_op1(OP_SETF, lv->field);
    else emit_op1(OP_ASET, lv->ndims);
}

static void emit_lval_dup(LVal *lv) {
    if (lv->kind == 1) emit_op1(OP_DUPN, 1);
    else if (lv->kind == 2) emit_op1(OP_DUPN, lv->ndims + 1);
}

/* Parse one accessor ('.field' or '[i, j]') applied to a value of type `t`
 * already on the stack. Index expressions are emitted; the final get/set is
 * left to the caller via `out`. */
static void parse_accessor(VType t, LVal *out) {
    if (match(T_DOT)) {
        Token *f = expect(T_IDENT, "a field name after '.'");
        if (t.base == K_NULL) cerror_at(f, "Cannot access a field of null");
        if (!is_obj(t)) cerror_at(f, "Cannot access field '%s' of %s", f->text, type_str(t));
        TypeDef *td = &C.prog->types[t.tid];
        for (int i = 0; i < td->nfields; i++)
            if (strcmp(td->fieldNames[i], f->lower) == 0) {
                out->kind = 1;
                out->field = i;
                out->type = td->fieldTypes[i];
                check_suffix(f, out->type);
                return;
            }
        cerror_at(f, "type %s has no field '%s'", td->name, f->text);
    }
    Token *lb = expect(T_LBRACK, "'['");
    if (!t.dims) cerror_at(lb, "%s is not an array", type_str(t));
    int n = 0;
    do {
        VType it = expr();
        coerce(it, vt(K_INT), "Array index");
        n++;
    } while (match(T_COMMA));
    expect(T_RBRACK, "']'");
    if (n != t.dims) cerror_at(lb, "Array has %d dimension(s) but %d index(es) were given", t.dims, n);
    out->kind = 2;
    out->ndims = n;
    out->type = t;
    out->type.dims = 0;
}

/* ------------------------------------------------------------------ */
/* constant expressions (Const values, For ... Step)                   */
/* ------------------------------------------------------------------ */

static ConstVal cx_add(void);

static ConstVal cnum_i(int32_t i) { ConstVal v = {0}; v.t = vt(K_INT); v.i = i; return v; }
static ConstVal cnum_f(double f) { ConstVal v = {0}; v.t = vt(K_FLOAT); v.f = f; return v; }
static ConstVal cnum_l(uint8_t base, int64_t l) { ConstVal v = {0}; v.t = vt(base); v.l = l; return v; }
static bool cx_wide(ConstVal v) { return v.t.base == K_LONG || v.t.base == K_ULONG; }
static int64_t cx_l(ConstVal v) { return cx_wide(v) ? v.l : (int64_t)v.i; }
static double cx_f(ConstVal v) {
    if (v.t.base == K_FLOAT) return v.f;
    if (v.t.base == K_LONG) return (double)v.l;
    if (v.t.base == K_ULONG) return (double)(uint64_t)v.l;
    return (double)v.i;
}

/* Integer constant arithmetic where either side is 64-bit. */
static ConstVal cx_long_op(TokType op, ConstVal a, ConstVal b) {
    uint8_t base = (a.t.base == K_ULONG || b.t.base == K_ULONG) ? K_ULONG : K_LONG;
    int64_t x = cx_l(a), y = cx_l(b);
    uint64_t ux = (uint64_t)x, uy = (uint64_t)y;
    if ((op == T_SLASH || op == T_MOD) && y == 0) cerror("Division by zero in constant");
    switch (op) {
        case T_PLUS: return cnum_l(base, (int64_t)(ux + uy));
        case T_MINUS: return cnum_l(base, (int64_t)(ux - uy));
        case T_STAR: return cnum_l(base, (int64_t)(ux * uy));
        case T_SLASH:
            if (base == K_ULONG) return cnum_l(base, (int64_t)(ux / uy));
            return cnum_l(base, y == -1 ? (int64_t)(0u - ux) : x / y);
        case T_MOD:
            if (base == K_ULONG) return cnum_l(base, (int64_t)(ux % uy));
            return cnum_l(base, y == -1 ? 0 : x % y);
        case T_SHL: return cnum_l(base, (int64_t)(ux << (y & 63)));
        case T_SHR: return cnum_l(base, (int64_t)(ux >> (y & 63)));
        case T_PIPE: return cnum_l(base, x | y);
        case T_AMP: return cnum_l(base, x & y);
        default: return cnum_l(base, x ^ y);   /* T_TILDE */
    }
}

static ConstVal cx_primary(void) {
    Token *t = peek();
    switch (t->type) {
        case T_INTLIT: advance(); return cnum_i(t->ival);
        case T_LONGLIT: advance(); return cnum_l(t->ival ? K_ULONG : K_LONG, t->lval);
        case T_FLOATLIT: advance(); return cnum_f(t->fval);
        case T_STRLIT: {
            advance();
            ConstVal v = {0};
            v.t = vt(K_STRING);
            v.s = gb_strndup(t->text, (size_t)t->ival);
            v.slen = t->ival;
            return v;
        }
        case T_LPAREN: {
            advance();
            ConstVal v = cx_add();
            expect(T_RPAREN, "')'");
            return v;
        }
        case T_IDENT: {
            ConstSym *c = find_const(t->lower);
            if (!c) cerror("'%s' is not a constant", t->text);
            advance();
            ConstVal v = {0};
            v.t = c->type; v.i = c->i; v.l = c->l; v.f = c->f; v.s = c->s; v.slen = c->slen;
            return v;
        }
        default:
            cerror("Expected a constant value but found %s", tok_desc(t));
    }
}

static bool at_min_literal(int64_t *l, bool *isLong);

static ConstVal cx_unary(void) {
    int64_t minl;
    bool minLong;
    if (at_min_literal(&minl, &minLong)) {
        advance();
        advance();
        return minLong ? cnum_l(K_LONG, minl) : cnum_i((int32_t)minl);
    }
    if (match(T_MINUS)) {
        ConstVal v = cx_unary();
        if (v.t.base == K_INT) v.i = (int32_t)(0u - (uint32_t)v.i);
        else if (cx_wide(v)) {
            v.l = (int64_t)(0u - (uint64_t)v.l);
            if (v.t.base == K_ULONG && v.l < 0) v.t = vt(K_LONG);   /* -9223372036854775808 */
        }
        else if (v.t.base == K_FLOAT) v.f = -v.f;
        else cerror("Cannot negate a string");
        return v;
    }
    if (match(T_PLUS)) return cx_unary();
    if (match(T_TILDE)) {
        ConstVal v = cx_unary();
        if (cx_wide(v)) { v.l = ~v.l; return v; }
        if (v.t.base != K_INT) cerror("'~' needs an int");
        v.i = ~v.i;
        return v;
    }
    return cx_primary();
}

static ConstVal cx_mul(void) {
    ConstVal a = cx_unary();
    for (;;) {
        TokType op = pt();
        if (op != T_STAR && op != T_SLASH && op != T_MOD && op != T_SHL && op != T_SHR && op != T_CARET) return a;
        advance();
        ConstVal b = cx_unary();
        if (a.t.base == K_STRING || b.t.base == K_STRING) cerror("Invalid string operation in constant");
        if (op == T_CARET) { a = cnum_f(pow(cx_f(a), cx_f(b))); continue; }
        if (cx_wide(a) || cx_wide(b)) {
            if (a.t.base == K_FLOAT || b.t.base == K_FLOAT) {
                double x = cx_f(a), y = cx_f(b);
                if (op == T_SHL || op == T_SHR) cerror("Shift needs int values");
                a = cnum_f(op == T_STAR ? x * y : op == T_SLASH ? x / y : fmod(x, y));
            } else a = cx_long_op(op, a, b);
            continue;
        }
        if (op == T_SHL || op == T_SHR) {
            if (a.t.base != K_INT || b.t.base != K_INT) cerror("Shift needs int values");
            a.i = op == T_SHL ? (int32_t)((uint32_t)a.i << (b.i & 31)) : (int32_t)((uint32_t)a.i >> (b.i & 31));
            continue;
        }
        if (a.t.base == K_INT && b.t.base == K_INT) {
            if ((op == T_SLASH || op == T_MOD) && b.i == 0) cerror("Division by zero in constant");
            if (op == T_STAR) a.i = (int32_t)((uint32_t)a.i * (uint32_t)b.i);
            else if (op == T_SLASH) a.i = a.i / b.i;
            else a.i = a.i % b.i;
        } else {
            double x = cx_f(a), y = cx_f(b);
            a = cnum_f(op == T_STAR ? x * y : op == T_SLASH ? x / y : fmod(x, y));
        }
    }
}

static ConstVal cx_add(void) {
    ConstVal a = cx_mul();
    for (;;) {
        TokType op = pt();
        if (op != T_PLUS && op != T_MINUS && op != T_PIPE && op != T_AMP && op != T_TILDE) return a;
        advance();
        ConstVal b = cx_mul();
        if (op == T_PLUS && (a.t.base == K_STRING || b.t.base == K_STRING)) {
            if (a.t.base != K_STRING || b.t.base != K_STRING) cerror("Constant string + number is not supported");
            char *s = gb_alloc((size_t)(a.slen + b.slen + 1));
            memcpy(s, a.s, (size_t)a.slen);
            memcpy(s + a.slen, b.s, (size_t)b.slen);
            a.s = s;
            a.slen += b.slen;
            continue;
        }
        if (a.t.base == K_STRING || b.t.base == K_STRING) cerror("Invalid string operation in constant");
        if (cx_wide(a) || cx_wide(b)) {
            if (a.t.base == K_FLOAT || b.t.base == K_FLOAT) {
                if (op == T_PIPE || op == T_AMP || op == T_TILDE) cerror("Bitwise operators need int values");
                a = cnum_f(op == T_PLUS ? cx_f(a) + cx_f(b) : cx_f(a) - cx_f(b));
            } else a = cx_long_op(op, a, b);
            continue;
        }
        if (op == T_PIPE || op == T_AMP || op == T_TILDE) {
            if (a.t.base != K_INT || b.t.base != K_INT) cerror("Bitwise operators need int values");
            a.i = op == T_PIPE ? (a.i | b.i) : op == T_AMP ? (a.i & b.i) : (a.i ^ b.i);
            continue;
        }
        if (a.t.base == K_INT && b.t.base == K_INT)
            a.i = (int32_t)(op == T_PLUS ? (uint32_t)a.i + (uint32_t)b.i : (uint32_t)a.i - (uint32_t)b.i);
        else
            a = cnum_f(op == T_PLUS ? cx_f(a) + cx_f(b) : cx_f(a) - cx_f(b));
    }
}

static ConstVal const_coerce(ConstVal v, VType to, Token *at) {
    if (to.dims || to.base == K_OBJ || to.base == K_LIST) cerror_at(at, "Constants must be int, float or string");
    if (to.base == K_STRING || v.t.base == K_STRING) {
        if (to.base == v.t.base) return v;
        cerror_at(at, "Cannot use %s as %s", type_str(v.t), type_str(to));
    }
    ConstVal r = {0};
    r.t = to;
    if (to.base == K_FLOAT) {
        r.f = cx_f(v);
        if (to.num == NT_F32) r.f = (double)(float)r.f;
        return r;
    }
    /* integer target: get the bits (saturating from float, like the VM), then wrap */
    int64_t x;
    if (v.t.base == K_FLOAT) {
        if (to.base == K_INT) x = fold_f2i(v.f);
        else if (to.base == K_ULONG) x = (int64_t)fold_f2u(v.f);
        else x = fold_f2l(v.f);
    } else x = cx_l(v);
    switch (to.num) {
        case NT_S8: r.i = (int8_t)x; break;
        case NT_U8: r.i = (uint8_t)x; break;
        case NT_S16: r.i = (int16_t)x; break;
        case NT_U16: r.i = (uint16_t)x; break;
        case NT_S32: r.i = (int32_t)(uint32_t)(uint64_t)x; break;
        case NT_U32: r.l = (int64_t)(uint32_t)x; break;
        default: r.l = x; break;
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* expressions                                                         */
/* ------------------------------------------------------------------ */

/* The type two numbers meet at for an operator: the wider arithmetic width
 * (int < long < ulong < float; `toFloat` forces float, for ^). For floats the
 * result is a 32-bit float unless a 64-bit one is involved; a literal or an
 * untyped constant takes the other side's type (f:float * 0.5 stays a float).
 * Emits the conversions for both operands (left is below right). */
static VType unify_num(VType lt, VType rt, bool toFloat) {
    uint8_t base = num_rank(lt.base) >= num_rank(rt.base) ? lt.base : rt.base;
    if (toFloat) base = K_FLOAT;
    VType target = vt(base);
    if (base == K_FLOAT) {
        bool f64 = (lt.base == K_FLOAT && lt.num == NT_F64 && !lt.lit) || (rt.base == K_FLOAT && rt.num == NT_F64 && !rt.lit);
        bool f32 = lt.num == NT_F32 || rt.num == NT_F32;
        target.num = (f32 && !f64) ? NT_F32 : NT_F64;
    }
    num_convert_under(lt, target);
    emit_num_convert(rt, target);
    target.lit = lt.lit && rt.lit;
    return target;
}

/* numeric/string binary operator; left operand is below the right one */
static VType arith(TokType op, VType lt, VType rt) {
    if (op == T_PLUS && (is_str(lt) || is_str(rt))) {
        if (!is_str(lt)) {
            if (is_num(lt)) num_to_str_under(lt);
            else cerror("Cannot add %s to a string", type_str(lt));
        }
        if (!is_str(rt)) {
            if (is_num(rt)) emit_num_to_str(rt);
            else cerror("Cannot add %s to a string", type_str(rt));
        }
        emit_op(OP_CONCAT);
        return vt(K_STRING);
    }
    if (op == T_AMP || op == T_PIPE || op == T_TILDE || op == T_SHL || op == T_SHR || op == T_SAR) {
        if (!is_integer(lt) || !is_integer(rt))
            cerror("Bitwise operators need integer values (got %s and %s)", type_str(lt), type_str(rt));
        VType r = unify_num(lt, rt, false);
        bool w = r.base != K_INT;
        switch (op) {
            case T_AMP: emit_op(w ? OP_BANDL : OP_BAND); break;
            case T_PIPE: emit_op(w ? OP_BORL : OP_BOR); break;
            case T_TILDE: emit_op(w ? OP_BXORL : OP_BXOR); break;
            case T_SHL: emit_op(w ? OP_SHLL : OP_SHL); break;
            case T_SHR: emit_op(w ? OP_SHRL : OP_SHR); break;
            default: emit_op(r.base == K_ULONG ? OP_SHRL : w ? OP_SARL : OP_SAR); break;   /* unsigned: no sign bit */
        }
        return r;
    }
    if (!is_num(lt) || !is_num(rt))
        cerror("Operator needs numbers (got %s and %s)", type_str(lt), type_str(rt));
    VType r = unify_num(lt, rt, op == T_CARET);
    if (r.base == K_FLOAT) {
        switch (op) {
            case T_PLUS: emit_op(OP_ADDF); break;
            case T_MINUS: emit_op(OP_SUBF); break;
            case T_STAR: emit_op(OP_MULF); break;
            case T_SLASH: emit_op(OP_DIVF); break;
            case T_MOD: emit_op(OP_MODF); break;
            default: emit_op(OP_POWF); break;
        }
        /* 32-bit float maths: the exact double result rounded to 32 bits is
         * what single precision would give (fmod is always exact) */
        if (r.num == NT_F32 && op != T_MOD) emit_narrow(NT_F32);
        return r;
    }
    if (r.base == K_INT) {
        switch (op) {
            case T_PLUS: emit_op(OP_ADDI); break;
            case T_MINUS: emit_op(OP_SUBI); break;
            case T_STAR: emit_op(OP_MULI); break;
            case T_SLASH: emit_op(OP_DIVI); break;
            default: emit_op(OP_MODI); break;
        }
        return r;
    }
    bool u = r.base == K_ULONG;
    switch (op) {
        case T_PLUS: emit_op(OP_ADDL); break;
        case T_MINUS: emit_op(OP_SUBL); break;
        case T_STAR: emit_op(OP_MULL); break;
        case T_SLASH: emit_op(u ? OP_DIVU : OP_DIVL); break;
        default: emit_op(u ? OP_MODU : OP_MODL); break;
    }
    return r;
}

static int cmp_index(TokType op) {
    switch (op) {
        case T_EQ: return 0;
        case T_NE: return 1;
        case T_LT: return 2;
        case T_GT: return 3;
        case T_LE: return 4;
        default: return 5;
    }
}

static VType compare(TokType op, VType lt, VType rt) {
    int ci = cmp_index(op);
    if (is_num(lt) && is_num(rt)) {
        VType r = unify_num(lt, rt, false);
        switch (r.base) {
            case K_INT: emit_op((OpCode)(OP_EQI + ci)); break;
            case K_LONG: emit_op((OpCode)(OP_EQL + ci)); break;
            case K_ULONG: emit_op(ci < 2 ? (OpCode)(OP_EQL + ci) : (OpCode)(OP_LTU + ci - 2)); break;
            default: emit_op((OpCode)(OP_EQF + ci)); break;
        }
        return vt(K_INT);
    }
    if (is_str(lt) || is_str(rt)) {
        if (!is_str(lt)) {
            if (is_num(lt)) num_to_str_under(lt);
            else cerror("Cannot compare %s with a string", type_str(lt));
        }
        if (!is_str(rt)) {
            if (is_num(rt)) emit_num_to_str(rt);
            else cerror("Cannot compare %s with a string", type_str(rt));
        }
        emit_op((OpCode)(OP_EQS + ci));
        return vt(K_INT);
    }
    if (is_ref(lt) && is_ref(rt)) {
        if (ci > 1) cerror("Objects can only be compared with = or <>");
        bool ok = lt.base == K_NULL || rt.base == K_NULL || vt_eq(lt, rt);
        if (!ok) cerror("Cannot compare %s with %s", type_str(lt), type_str(rt));
        emit_op(ci == 0 ? OP_EQO : OP_NEO);
        return vt(K_INT);
    }
    cerror("Cannot compare %s with %s", type_str(lt), type_str(rt));
}

static VType call_user(FuncSym *f, bool parens) {
    if (parens) expect(T_LPAREN, "'('");
    int n = 0;
    bool empty = parens ? pt() == T_RPAREN : at_stmt_end();
    if (!empty) {
        do {
            if (n >= f->nparams) cerror("Too many arguments for %s (it takes %d)", f->name, f->nparams);
            char ctx[160];
            snprintf(ctx, sizeof ctx, "Argument %d of %s", n + 1, f->name);
            VType at = expr();
            coerce(at, f->ptypes[n], ctx);
            n++;
        } while (match(T_COMMA));
    }
    if (parens) expect(T_RPAREN, "')'");
    for (; n < f->nparams; n++) {
        if (f->pdefault[n] < 0) cerror("Missing argument '%s' for %s", f->pnames[n], f->name);
        int save = C.pos;
        int32_t saveLine = C.lastLine;
        C.pos = f->pdefault[n];
        VType at = expr();
        coerce(at, f->ptypes[n], "default argument");
        C.pos = save;
        C.lastLine = saveLine;
    }
    emit_op2(OP_CALL, f->fidx, f->nparams);
    return f->ret;
}

static VType kind_char_type(char c) {
    if (c == 'f') return vt(K_FLOAT);
    if (c == 's') return vt(K_STRING);
    if (c == 'v') return vt(K_VOID);
    return vt(K_INT);
}

static VType call_native(int idx, bool parens) {
    const NativeDef *nd = native_get(idx);
    if (parens) expect(T_LPAREN, "'('");
    const char *p = nd->params;
    int required = 0, maxArgs = 0;
    bool opt = false;
    for (const char *q = p; *q; q++) {
        if (*q == '|') { opt = true; continue; }
        maxArgs++;
        if (!opt) required++;
    }
    int n = 0;
    bool empty = parens ? pt() == T_RPAREN : at_stmt_end();
    if (!empty) {
        const char *q = p;
        do {
            while (*q == '|') q++;
            if (!*q) cerror("Too many arguments for %s (it takes at most %d)", nd->name, maxArgs);
            char ctx[160];
            snprintf(ctx, sizeof ctx, "Argument %d of %s", n + 1, nd->name);
            VType at = expr();
            coerce(at, kind_char_type(*q), ctx);
            q++;
            n++;
        } while (match(T_COMMA));
    }
    if (parens) expect(T_RPAREN, "')'");
    if (n < required) cerror("%s needs at least %d argument(s), got %d", nd->name, required, n);
    emit_op2(OP_NATIVE, idx, n);
    return kind_char_type(nd->ret);
}

static int native_or_die(const char *name) {
    int i = native_find(name);
    if (i < 0) { gb_diag("internal error: missing native %s\n", name); gb_fatal(); }
    return i;
}

static VType expr_postfix(void);

static int list_fn_index(const char *lower) {
    for (int i = 0; LIST_FNS[i]; i++) if (!strcmp(LIST_FNS[i], lower)) return i;
    return -1;
}

/* list_add_first / list_add_last / list_remove / list_contains (list, value),
 * count_list / list_is_empty / clear_list (list). The value must have the
 * list's element type; an int may go into a float list. */
static VType list_call(Token *id, int which, bool parens) {
    const char *fn = LIST_FNS[which];
    if (parens) expect(T_LPAREN, "'('");
    Token *at = peek();
    VType lt = expr();
    if (lt.dims || lt.base != K_LIST) cerror_at(at, "%s needs a list as its first argument, got %s", fn, type_str(lt));
    int argc = 1;
    if (which <= 3) {
        expect(T_COMMA, "','");
        Token *vtok = peek();
        VType v = expr();
        VType e = list_elem(lt);
        if (v.base == K_NULL && !v.dims)
            cerror_at(vtok, "%s: a list cannot hold null", fn);
        /* any number goes into a number list (wrapped/rounded to its element
         * type), except that a float cannot silently go into an integer list */
        if (is_num(e) && is_num(v) && !(is_intk(e.base) && v.base == K_FLOAT)) coerce(v, e, fn);
        else if (!vt_eq(v, e))
            cerror_at(vtok, "%s: cannot put %s %s into %s %s", fn,
                      v.base == K_INT || v.base == K_OBJ ? "an" : "a", type_str(v),
                      lt.elem == K_INT || lt.elem == K_OBJ ? "an" : "a", type_str(lt));
        argc = 2;
    }
    if (parens) expect(T_RPAREN, "')'");
    char nn[40];
    snprintf(nn, sizeof nn, "__%s", fn);
    emit_op2(OP_NATIVE, native_or_die(nn), argc);
    return (which == 3 || which == 4 || which == 5) ? vt(K_INT) : vt(K_VOID);
}

/* Built-in functions that need special typing. Returns true if handled. */
static bool intrinsic(Token *id, VType *out) {
    const char *n = id->lower;
    bool hasParen = pt() == T_LPAREN;

    if (!strcmp(n, "first") || !strcmp(n, "last") || !strcmp(n, "after") || !strcmp(n, "before"))
        cerror_at(id, "%s was removed: types no longer keep a list of every instance. "
                      "Keep objects in a list and loop with for x:t_foo = eachin list", id->text);
    if (!strcmp(n, "create_list"))
        cerror_at(id, "create_list was replaced by typed lists, e.g. new int list or new t_enemy list");
    int li = list_fn_index(n);
    if (li >= 0) {
        VType r = list_call(id, li, hasParen);
        if (r.base == K_VOID) cerror_at(id, "%s is a command and does not return a value", id->text);
        *out = r;
        return true;
    }

    if (!hasParen) return false;
    bool isLen = !strcmp(n, "len");
    int convNum = num_type_find(n);   /* int(x), byte(x), double(x), uint32(x), ... */
    bool isStr = !strcmp(n, "str") || !strcmp(n, "string");
    bool isAbs = !strcmp(n, "abs"), isSgn = !strcmp(n, "sgn");
    bool isMin = !strcmp(n, "min"), isMax = !strcmp(n, "max");
    if (!(isLen || convNum || isStr || isAbs || isSgn || isMin || isMax)) return false;

    advance(); /* ( */
    VType a = expr();
    if (isLen) {
        if (a.dims) {
            if (match(T_COMMA)) { VType d = expr(); coerce(d, vt(K_INT), "len dimension"); }
            else emit_op1(OP_PUSH_INT, 1);
            emit_op(OP_ALEN);
        } else if (is_str(a) || is_num(a)) {
            coerce(a, vt(K_STRING), "len");
            emit_op2(OP_NATIVE, native_or_die("__len"), 1);
        } else cerror_at(id, "len needs a string or an array, got %s", type_str(a));
        expect(T_RPAREN, "')'");
        *out = vt(K_INT);
        return true;
    }
    if (convNum || isStr) {
        VType to = convNum ? vt_num((uint8_t)convNum) : vt(K_STRING);
        coerce(a, to, id->text);
        expect(T_RPAREN, "')'");
        *out = to;
        return true;
    }
    if (isAbs || isSgn) {
        if (!is_num(a)) cerror_at(id, "%s needs a number", id->text);
        const char *nn;
        switch (a.base) {
            case K_FLOAT: nn = isAbs ? "__absf" : "__sgnf"; break;
            case K_LONG: nn = isAbs ? "__absl" : "__sgnl"; break;
            case K_ULONG: nn = isAbs ? NULL : "__sgnu"; break;   /* abs of an unsigned is itself */
            default: nn = isAbs ? "__absi" : "__sgni"; break;
        }
        if (nn) emit_op2(OP_NATIVE, native_or_die(nn), 1);
        expect(T_RPAREN, "')'");
        /* floats keep their type (abs/sgn are exact); abs(-128) of a sint8 needs an int */
        *out = a.base == K_FLOAT ? a : vt(a.base);
        out->lit = 0;
        return true;
    }
    /* Min / Max */
    if (!is_num(a)) cerror_at(id, "%s needs numbers", id->text);
    expect(T_COMMA, "','");
    int afterA = here();
    (void)afterA;
    VType b = expr();
    if (!is_num(b)) cerror_at(id, "%s needs numbers", id->text);
    VType r = unify_num(a, b, false);
    const char *nn;
    switch (r.base) {
        case K_FLOAT: nn = isMin ? "__minf" : "__maxf"; break;
        case K_LONG: nn = isMin ? "__minl" : "__maxl"; break;
        case K_ULONG: nn = isMin ? "__minu" : "__maxu"; break;
        default: nn = isMin ? "__mini" : "__maxi"; break;
    }
    emit_op2(OP_NATIVE, native_or_die(nn), 2);
    expect(T_RPAREN, "')'");
    r.lit = 0;
    *out = r;
    return true;
}

static VType primary_ident(void) {
    Token *id = advance();
    VarRef ref;
    bool isVar = lookup_var(id->lower, &ref);
    if (isVar && pt() != T_LPAREN) {
        check_suffix(id, ref.type);
        emit_load_var(&ref);
        return ref.type;
    }
    FuncSym *f = find_func(id->lower);
    if (f) {
        if (f->ret.base == K_VOID && !f->ret.dims)
            cerror_at(id, "%s has no return type, so it does not return a value (superstrict)", id->text);
        return call_user(f, pt() == T_LPAREN);
    }
    VType it;
    if (intrinsic(id, &it)) return it;
    int n = native_find(id->lower);
    if (n >= 0) {
        VType r = call_native(n, pt() == T_LPAREN);
        if (r.base == K_VOID) cerror_at(id, "%s is a command and does not return a value", id->text);
        return r;
    }
    if (isVar) { emit_load_var(&ref); return ref.type; }
    if (pt() == T_LPAREN) unknown_name(id, "function");
    VarRef r = auto_var(id);
    emit_load_var(&r);
    return r.type;
}

static VType expr_primary(void) {
    Token *t = peek();
    switch (t->type) {
        case T_INTLIT: { advance(); emit_op1(OP_PUSH_INT, t->ival); VType r = vt(K_INT); r.lit = 1; return r; }
        case T_LONGLIT: { advance(); emit_long(t->lval); VType r = vt(t->ival ? K_ULONG : K_LONG); r.lit = 1; return r; }
        case T_FLOATLIT: { advance(); emit_float(t->fval); VType r = vt(K_FLOAT); r.lit = 1; return r; }
        case T_STRLIT: advance(); emit_string(t->text, t->ival); return vt(K_STRING);
        case T_LPAREN: {
            advance();
            VType r = expr();
            expect(T_RPAREN, "')'");
            return r;
        }
        case T_NULL: advance(); emit_op1(OP_PUSH_DEFAULT, K_OBJ); return vt(K_NULL);
        case T_NEW: {
            advance();
            Token *tn = expect(T_IDENT, "a type name after new");
            if (is_old_tlist(tn->lower)) old_tlist_error(tn);
            if (at_list_word()) {
                VType lt = parse_type(tn);
                if (pt() == T_LPAREN && peekn(1)->type == T_RPAREN) { advance(); advance(); }
                emit_op2(OP_NATIVE, native_or_die("__list_new"), 0);
                return lt;
            }
            if (is_type_name(tn->lower) && find_type(tn->lower) < 0)
                cerror_at(tn, "new makes objects and lists: new t_foo or new %s list", tn->lower);
            int tid = find_type(tn->lower);
            if (tid < 0) unknown_name(tn, "type");
            if (pt() == T_LPAREN && peekn(1)->type == T_RPAREN) { advance(); advance(); }
            emit_op1(OP_NEW, tid);
            VType r = vt(K_OBJ);
            r.tid = (int16_t)tid;
            return r;
        }
        case T_IDENT: return primary_ident();
        default:
            cerror("Expected an expression but found %s", tok_desc(t));
    }
}

static VType expr_postfix(void) {
    VType t = expr_primary();
    while (pt() == T_DOT || pt() == T_LBRACK) {
        LVal lv;
        parse_accessor(t, &lv);
        emit_lval_load(&lv);
        t = lv.type;
    }
    return t;
}

static VType expr_unary(void);

static VType expr_pow(void) {
    VType a = expr_postfix();
    if (match(T_CARET)) {
        VType b = expr_unary(); /* right associative */
        return arith(T_CARET, a, b);
    }
    return a;
}

/* `-2147483648` is the smallest int, even though 2147483648 alone is too big
 * for one (same for -9223372036854775808 and a sint64). True if the minus at
 * the current token is followed by such a literal; `*l` gets the value. */
static bool at_min_literal(int64_t *l, bool *isLong) {
    Token *n = peekn(1);
    if (pt() != T_MINUS || n->type != T_LONGLIT) return false;
    TokType after = peekn(2)->type;
    if (after == T_CARET || after == T_DOT || after == T_LBRACK) return false;
    if (n->lval == 2147483648ll) { *l = INT32_MIN; *isLong = false; return true; }
    if (n->ival && n->lval == INT64_MIN) { *l = INT64_MIN; *isLong = true; return true; }
    return false;
}

static VType expr_unary(void) {
    int64_t minl;
    bool minLong;
    if (at_min_literal(&minl, &minLong)) {
        advance();
        advance();
        if (minLong) emit_long(minl); else emit_op1(OP_PUSH_INT, (int32_t)minl);
        VType r = vt(minLong ? K_LONG : K_INT);
        r.lit = 1;
        return r;
    }
    if (match(T_MINUS)) {
        VType t = expr_unary();
        if (!is_num(t)) cerror("Cannot negate %s", type_str(t));
        if (t.base == K_FLOAT) { emit_op(OP_NEGF); return t; }   /* exact, so a float stays a float */
        emit_op(t.base == K_INT ? OP_NEGI : OP_NEGL);
        VType r = vt(t.base);      /* -x of a byte is an int, not a byte */
        r.lit = t.lit;
        return r;
    }
    if (match(T_PLUS)) {
        VType t = expr_unary();
        if (!is_num(t)) cerror("Unary + needs a number");
        return t;
    }
    if (match(T_TILDE)) {
        VType t = expr_unary();
        if (!is_integer(t)) cerror("'~' needs an integer");
        emit_op(t.base == K_INT ? OP_BNOT : OP_BNOTL);
        VType r = vt(t.base);
        r.lit = t.lit;
        return r;
    }
    return expr_pow();
}

static VType expr_mul(void) {
    VType a = expr_unary();
    for (;;) {
        TokType op = pt();
        if (op != T_STAR && op != T_SLASH && op != T_MOD && op != T_SHL && op != T_SHR && op != T_SAR) return a;
        advance();
        VType b = expr_unary();
        a = arith(op, a, b);
    }
}

static VType expr_add(void) {
    VType a = expr_mul();
    for (;;) {
        TokType op = pt();
        if (op != T_PLUS && op != T_MINUS) return a;
        advance();
        VType b = expr_mul();
        a = arith(op, a, b);
    }
}

static VType expr_bit(void) {
    VType a = expr_add();
    for (;;) {
        TokType op = pt();
        if (op != T_AMP && op != T_PIPE && op != T_TILDE) return a;
        advance();
        VType b = expr_add();
        a = arith(op, a, b);
    }
}

static VType expr_cmp(void) {
    VType a = expr_bit();
    for (;;) {
        TokType op = pt();
        if (op != T_EQ && op != T_NE && op != T_LT && op != T_GT && op != T_LE && op != T_GE) return a;
        advance();
        VType b = expr_bit();
        a = compare(op, a, b);
    }
}

static VType expr_not(void) {
    if (match(T_NOT)) {
        VType t = expr_not();
        to_cond(t);
        emit_op(OP_NOT);
        return vt(K_INT);
    }
    return expr_cmp();
}

static VType expr_and(void) {
    VType a = expr_not();
    while (pt() == T_AND) {
        advance();
        to_bool(a);
        emit_op1(OP_DUPN, 1);
        int j = emit_jump(OP_JF);
        emit_op(OP_POP);
        VType b = expr_not();
        to_bool(b);
        patch_here(j);
        a = vt(K_INT);
    }
    return a;
}

static VType expr(void) {
    VType a = expr_and();
    while (pt() == T_OR) {
        advance();
        to_bool(a);
        emit_op1(OP_DUPN, 1);
        int j = emit_jump(OP_JT);
        emit_op(OP_POP);
        VType b = expr_and();
        to_bool(b);
        patch_here(j);
        a = vt(K_INT);
    }
    return a;
}

static void cond_expr(void) {
    VType t = expr();
    to_cond(t);
}

/* ------------------------------------------------------------------ */
/* statements                                                          */
/* ------------------------------------------------------------------ */

static void statement(void);

static void end_of_statement(void) {
    if (pt() == T_NEWLINE || pt() == T_COLON) { advance(); return; }
    if (pt() == T_EOF) return;
    cerror("Expected end of statement but found %s", tok_desc(peek()));
}

static bool in_stops(TokType t, const TokType *stops, int n) {
    for (int i = 0; i < n; i++) if (stops[i] == t) return true;
    return false;
}

static void block(const TokType *stops, int nstops, Token *opener) {
    C.depth++;
    for (;;) {
        TokType t = pt();
        if (in_stops(t, stops, nstops)) break;
        if (t == T_EOF)
            cerror_at(opener, "%s on line %d is never closed (expected %s)", tok_name(opener->type), opener->line,
                      tok_name(stops[0]));
        if (t == T_NEWLINE || t == T_COLON) { advance(); continue; }
        statement();
        if (!in_stops(pt(), stops, nstops)) end_of_statement();
    }
    C.depth--;
}

/* statements on the rest of a single-line If, up to Else / ElseIf / end of line */
static void line_statements(void) {
    for (;;) {
        TokType t = pt();
        if (t == T_NEWLINE || t == T_EOF || t == T_ELSE || t == T_ELSEIF) return;
        if (t == T_COLON) { advance(); continue; }
        statement();
        t = pt();
        if (t == T_COLON) { advance(); continue; }
        if (t == T_NEWLINE || t == T_EOF || t == T_ELSE || t == T_ELSEIF) return;
        cerror("Expected end of statement but found %s", tok_desc(peek()));
    }
}

static Loop *push_loop(void) {
    if (C.F->nloops >= 64) cerror("Loops nested too deeply");
    Loop *l = &C.F->loops[C.F->nloops++];
    memset(l, 0, sizeof *l);
    return l;
}

static void pop_loop(int contTarget, int breakTarget) {
    Loop *l = &C.F->loops[--C.F->nloops];
    il_patch(&l->conts, contTarget);
    il_patch(&l->breaks, breakTarget);
}

static void declare_target(Token *name, VType t, bool global, VarRef *out) {
    if (global) {
        Var *g = find_global(name->lower);
        if (!g) cerror_at(name, "internal: global '%s' was not prescanned", name->text);
        if (!vt_eq(g->type, t)) cerror_at(name, "global '%s' was declared as %s", name->text, type_str(g->type));
        out->where = 1;
        out->slot = g->slot;
        out->type = g->type;
    } else {
        out->where = 0;
        out->slot = declare_local_slot(name->lower, t, name);
        out->type = t;
    }
    out->c = NULL;
}

/* Local / Global / Dim declarations */
static void decl_statement(bool global, bool isDim) {
    do {
        Token *name = expect(T_IDENT, "a variable name");
        VType t = parse_typed(name, isDim ? "dim %s:int[10]" : global ? "global %s:int" : "local %s:int");
        VarRef ref;
        if (pt() == T_LBRACK) {
            int d = parse_empty_dims();
            if (d > 0) {
                t.dims = (uint8_t)d;
                declare_target(name, t, global, &ref);
                if (match(T_EQ)) {
                    VType et = expr();
                    coerce(et, t, "Assignment");
                } else emit_op1(OP_PUSH_DEFAULT, K_ARRAY);
                emit_store_var(&ref);
                continue;
            }
            advance(); /* [ */
            int n = 0;
            do {
                VType st = expr();
                coerce(st, vt(K_INT), "Array size");
                n++;
            } while (match(T_COMMA));
            expect(T_RBRACK, "']'");
            if (n > GB_MAX_DIMS) cerror_at(name, "Too many array dimensions (max %d)", GB_MAX_DIMS);
            VType arrT = t;
            arrT.dims = (uint8_t)n;
            declare_target(name, arrT, global, &ref);
            emit_op2(OP_NEWARR, runtime_kind(t), n);
            emit_store_var(&ref);
            continue;
        }
        if (isDim) cerror_at(name, "dim needs a size, e.g. dim %s[10]", name->text);
        declare_target(name, t, global, &ref);
        if (match(T_EQ)) {
            VType et = expr();
            coerce(et, t, "Assignment");
        } else emit_op1(OP_PUSH_DEFAULT, runtime_kind(t));
        emit_store_var(&ref);
    } while (match(T_COMMA));
}

static void const_statement(void) {
    do {
        Token *name = expect(T_IDENT, "a constant name");
        bool ex;
        VType t = parse_annot(name, &ex);
        expect(T_EQ, "'=' (constants need a value)");
        ConstVal v = cx_add();
        if (ex) v = const_coerce(v, t, name);
        else v.t.lit = 1;   /* untyped constant: behaves like the literal it stands for */
        if (find_global(name->lower)) cerror_at(name, "'%s' is already a global", name->text);
        add_const_sym(name, v);
    } while (match(T_COMMA));
}

static void assignment(Token *id, VarRef ref) {
    if (ref.where == 2) cerror_at(id, "Cannot assign to constant '%s'", id->text);
    check_suffix(id, ref.type);
    LVal lv;
    lv.kind = 0;
    lv.var = ref;
    lv.type = ref.type;
    while (pt() == T_DOT || pt() == T_LBRACK) {
        emit_lval_load(&lv);
        VType cur = lv.type;
        parse_accessor(cur, &lv);
    }
    TokType op = pt();
    if (op == T_EQ) {
        advance();
        VType et = expr();
        coerce(et, lv.type, "Assignment");
        emit_lval_store(&lv);
        return;
    }
    if (op == T_ADD_ASSIGN || op == T_SUB_ASSIGN || op == T_MUL_ASSIGN || op == T_DIV_ASSIGN) {
        advance();
        emit_lval_dup(&lv);
        emit_lval_load(&lv);
        VType et = expr();
        TokType bop = op == T_ADD_ASSIGN ? T_PLUS : op == T_SUB_ASSIGN ? T_MINUS : op == T_MUL_ASSIGN ? T_STAR : T_SLASH;
        VType rt = arith(bop, lv.type, et);
        coerce(rt, lv.type, "Assignment");
        emit_lval_store(&lv);
        return;
    }
    cerror("Expected '=' after '%s'", id->text);
}

static int matching_paren(int pos) {
    int depth = 0;
    for (int i = pos; i < C.tl->count; i++) {
        TokType t = C.tl->toks[i].type;
        if (t == T_LPAREN || t == T_LBRACK) depth++;
        else if (t == T_RPAREN || t == T_RBRACK) { if (--depth == 0) return i; }
        else if (t == T_NEWLINE || t == T_EOF) return -1;
    }
    return -1;
}

static void call_statement(Token *id) {
    bool parens = false;
    if (pt() == T_LPAREN) {
        int m = matching_paren(C.pos);
        if (m >= 0 && is_end_tok(C.tl->toks[m + 1].type)) parens = true;
    }
    FuncSym *f = find_func(id->lower);
    if (f) { call_user(f, parens); emit_op(OP_POP); return; }
    int li = list_fn_index(id->lower);
    if (li >= 0) { list_call(id, li, parens); emit_op(OP_POP); return; }
    int n = native_find(id->lower);
    if (n >= 0) { call_native(n, parens); emit_op(OP_POP); return; }
    unknown_name(id, "command or function");
}

static void ident_statement(void) {
    Token *id = peek();
    TokType nx = peekn(1)->type;
    bool assignish = nx == T_EQ || nx == T_DOT || nx == T_LBRACK || nx == T_ADD_ASSIGN || nx == T_SUB_ASSIGN ||
                     nx == T_MUL_ASSIGN || nx == T_DIV_ASSIGN;
    VarRef ref;
    bool isVar = lookup_var(id->lower, &ref);
    advance();
    /* `name:Type = value` declares a local on the spot (like Blitz's p.Player = New Player) */
    if (nx == T_COLON && peekn(1)->type == T_IDENT && is_type_name(peekn(1)->lower)) {
        VType t = parse_annot(id, NULL);
        if (pt() == T_LBRACK) {
            int d = parse_empty_dims();
            if (d < 0) cerror_at(id, "Use local %s:%s[size] to create an array", id->text, peekn(-1)->text);
            t.dims = (uint8_t)d;
        }
        if (isVar) {
            if (!vt_eq(ref.type, t)) cerror_at(id, "'%s' was already declared as %s", id->text, type_str(ref.type));
        } else {
            if (C.strict) cerror_at(id, "Use local to declare '%s' (strict mode)", id->text);
            ref.where = 0;
            ref.slot = declare_local_slot(id->lower, t, id);
            ref.type = t;
            ref.c = NULL;
        }
        if (pt() != T_EQ) cerror("Expected '=' after the declaration of '%s'", id->text);
        assignment(id, ref);
        return;
    }
    if (!assignish) {
        if (isVar && !find_func(id->lower) && native_find(id->lower) < 0 && list_fn_index(id->lower) < 0)
            cerror_at(id, "Expected '=' after variable '%s'", id->text);
        call_statement(id);
        return;
    }
    if (!isVar) {
        if (nx != T_EQ) unknown_name(id, "variable");
        ref = auto_var(id);
    }
    assignment(id, ref);
}

static void if_statement(Token *ifTok) {
    cond_expr();
    match(T_THEN);
    if (pt() != T_NEWLINE && pt() != T_EOF) {
        /* single-line If */
        int jf = emit_jump(OP_JF);
        line_statements();
        if (match(T_ELSE)) {
            int je = emit_jump(OP_JMP);
            patch_here(jf);
            line_statements();
            patch_here(je);
        } else if (pt() == T_ELSEIF) {
            Token *t = advance();
            int je = emit_jump(OP_JMP);
            patch_here(jf);
            if_statement(t);
            patch_here(je);
        } else patch_here(jf);
        /* allow a trailing EndIf on the same line */
        match(T_ENDIF);
        return;
    }
    static const TokType stops[] = {T_ENDIF, T_ELSEIF, T_ELSE};
    IntList ends = {0};
    int jf = emit_jump(OP_JF);
    block(stops, 3, ifTok);
    for (;;) {
        if (pt() == T_ELSEIF) {
            advance();
            il_push(&ends, emit_jump(OP_JMP));
            patch_here(jf);
            cond_expr();
            match(T_THEN);
            jf = emit_jump(OP_JF);
            block(stops, 3, ifTok);
            continue;
        }
        if (pt() == T_ELSE) {
            advance();
            il_push(&ends, emit_jump(OP_JMP));
            patch_here(jf);
            jf = -1;
            static const TokType endOnly[] = {T_ENDIF};
            block(endOnly, 1, ifTok);
        }
        break;
    }
    expect(T_ENDIF, "endif");
    if (jf >= 0) patch_here(jf);
    il_patch(&ends, here());
}

static void while_statement(Token *w) {
    int top = here();
    cond_expr();
    int jf = emit_jump(OP_JF);
    push_loop();
    static const TokType stops[] = {T_WEND};
    block(stops, 1, w);
    expect(T_WEND, "wend");
    emit_op1(OP_JMP, top);
    patch_here(jf);
    pop_loop(top, here());
}

static void repeat_statement(Token *r) {
    int top = here();
    push_loop();
    static const TokType stops[] = {T_UNTIL, T_FOREVER};
    block(stops, 2, r);
    int cont = here();
    if (match(T_UNTIL)) {
        cond_expr();
        emit_op1(OP_JF, top);
    } else {
        expect(T_FOREVER, "until or forever");
        emit_op1(OP_JMP, top);
    }
    pop_loop(cont, here());
}

static VarRef for_var(Token *name, bool isLocal, bool explicitT, VType t) {
    VarRef r;
    if (isLocal) {
        r.where = 0;
        r.slot = declare_local_slot(name->lower, t, name);
        r.type = t;
        r.c = NULL;
        return r;
    }
    if (lookup_var(name->lower, &r)) {
        if (r.where == 2) cerror_at(name, "Cannot use constant '%s' as a loop variable", name->text);
        if (explicitT && !vt_eq(r.type, t))
            cerror_at(name, "'%s' was already declared as %s", name->text, type_str(r.type));
        return r;
    }
    if (C.strict) cerror_at(name, "Loop variable '%s' has not been declared (use for local ...)", name->text);
    r.where = 0;
    r.slot = declare_local_slot(name->lower, t, name);
    r.type = t;
    r.c = NULL;
    return r;
}

static void for_statement(Token *forTok) {
    bool isLocal = match(T_LOCAL);
    Token *name = expect(T_IDENT, "a loop variable");
    bool ex;
    VType annot = parse_annot(name, &ex);
    expect(T_EQ, "'='");
    /* superstrict: a new counting variable needs a type; eachin takes the list's, so it may leave it out */
    if (isLocal && !ex && pt() != T_EACHIN && superstrict_at(name))
        cerror_at(name, "'%s' needs a type, e.g. for local %s:int = ... (superstrict)", name->text, name->text);
    static const TokType stops[] = {T_NEXT};

    if (pt() == T_EACH)
        cerror("for ... = each was removed: types no longer keep a list of every instance. "
               "Keep objects in a list and loop with for x:t_foo = eachin list");
    if (match(T_EACHIN)) {
        Token *at = peek();
        VType lt = expr();
        if (lt.dims || lt.base != K_LIST) cerror_at(at, "eachin needs a list, got %s", type_str(lt));
        VType e = list_elem(lt);
        /* without a type, a new loop variable takes the element type */
        VarRef v = for_var(name, isLocal, ex, ex ? annot : e);
        if (!vt_eq(v.type, e))
            cerror_at(name, "The loop variable is %s but the list holds %s", type_str(v.type), type_str(e));
        int lk = hidden_local(vt(K_LINK));
        emit_op(OP_ITER_FIRST);
        emit_op1(OP_STORE_LOCAL, lk);
        int top = here();
        emit_op1(OP_LOAD_LOCAL, lk);
        emit_op(OP_TRUTH_O);
        int jf = emit_jump(OP_JF);
        emit_op1(OP_LOAD_LOCAL, lk);
        emit_op(OP_LINK_VALUE);
        emit_store_var(&v);
        push_loop();
        block(stops, 1, forTok);
        int cont = here();
        emit_op1(OP_LOAD_LOCAL, lk);
        emit_op(OP_ITER_NEXT);
        emit_op1(OP_STORE_LOCAL, lk);
        emit_op1(OP_JMP, top);
        patch_here(jf);
        pop_loop(cont, here());
        expect(T_NEXT, "next");
        if (pt() == T_IDENT) advance();
        return;
    }

    VarRef v = for_var(name, isLocal, ex, annot);
    if (!is_num(v.type)) cerror_at(name, "for loop variable must be int or float");
    VType st = expr();
    coerce(st, v.type, "for start");
    emit_store_var(&v);
    expect(T_TO, "to");
    VType et = expr();
    coerce(et, v.type, "for end");
    int endSlot = hidden_local(v.type);
    emit_op1(OP_STORE_LOCAL, endSlot);
    ConstVal step = cnum_i(1);
    if (match(T_STEP)) {
        Token *stTok = peek();
        step = cx_add();
        if (step.t.base == K_STRING) cerror_at(stTok, "step must be a number");
        if (cx_f(step) == 0) cerror_at(stTok, "step cannot be 0");
        if (is_intk(v.type.base) && step.t.base == K_FLOAT) cerror_at(stTok, "an integer loop needs an integer step");
    }
    bool up = cx_f(step) > 0;
    emit_load_var(&v);
    emit_op1(OP_LOAD_LOCAL, endSlot);
    compare(up ? T_LE : T_GE, v.type, v.type);
    int jf = emit_jump(OP_JF);
    int body = here();
    push_loop();
    block(stops, 1, forTok);
    int cont = here();
    /* The next value is tested against the end before it is stored, at full
     * width: `for b:byte = 250 to 0 step -50` stops after 0 instead of
     * wrapping to 206 and looping forever. */
    emit_load_var(&v);
    VType stepT = step.t;
    stepT.lit = 1;
    if (step.t.base == K_FLOAT) emit_float(step.f);
    else if (cx_wide(step)) emit_long(step.l);
    else emit_op1(OP_PUSH_INT, step.i);
    VType next = arith(T_PLUS, v.type, stepT);
    emit_op1(OP_DUPN, 1);
    emit_op1(OP_LOAD_LOCAL, endSlot);
    compare(up ? T_LE : T_GE, next, v.type);
    int jdone = emit_jump(OP_JF);
    coerce(next, v.type, "for step");
    emit_store_var(&v);
    emit_op1(OP_JMP, body);
    patch_here(jdone);
    coerce(next, v.type, "for step");   /* the loop variable ends one step past the end, as before */
    emit_store_var(&v);
    patch_here(jf);
    pop_loop(cont, here());
    expect(T_NEXT, "next");
    if (pt() == T_IDENT) advance();
}

static void select_statement(Token *selTok) {
    VType st = expr();
    if (st.base == K_VOID) cerror("select needs a value");
    int slot = hidden_local(st);
    emit_op1(OP_STORE_LOCAL, slot);
    while (pt() == T_NEWLINE || pt() == T_COLON) advance();
    IntList ends = {0};
    int nextJ = -1;
    bool hadDefault = false;
    static const TokType stops[] = {T_CASE, T_DEFAULT, T_ENDSELECT};
    for (;;) {
        if (pt() == T_CASE) {
            if (hadDefault) cerror("case after default");
            advance();
            if (nextJ >= 0) patch_here(nextJ);
            IntList body = {0};
            do {
                emit_op1(OP_LOAD_LOCAL, slot);
                VType vt2 = expr();
                compare(T_EQ, st, vt2);
                il_push(&body, emit_jump(OP_JT));
            } while (match(T_COMMA));
            nextJ = emit_jump(OP_JMP);
            il_patch(&body, here());
            block(stops, 3, selTok);
            il_push(&ends, emit_jump(OP_JMP));
        } else if (pt() == T_DEFAULT) {
            if (hadDefault) cerror("Only one default is allowed");
            hadDefault = true;
            advance();
            if (nextJ >= 0) { patch_here(nextJ); nextJ = -1; }
            block(stops, 3, selTok);
        } else if (pt() == T_ENDSELECT) {
            advance();
            break;
        } else if (pt() == T_EOF) {
            cerror_at(selTok, "select on line %d is never closed (expected endselect)", selTok->line);
        } else {
            cerror("Expected case, default or endselect but found %s", tok_desc(peek()));
        }
    }
    if (nextJ >= 0) patch_here(nextJ);
    il_patch(&ends, here());
}

static void skip_to_matching_paren(void) {
    int depth = 0;
    for (;;) {
        TokType t = pt();
        if (t == T_EOF) cerror("Unexpected end of file in function header");
        advance();
        if (t == T_LPAREN) depth++;
        else if (t == T_RPAREN && --depth == 0) return;
    }
}

static void function_statement(Token *fnTok) {
    if (!C.F->isMain || C.depth > 0) cerror_at(fnTok, "Functions must be declared at the top level, not inside other code blocks");
    Token *nm = expect(T_IDENT, "a function name");
    FuncSym *f = find_func(nm->lower);
    while (pt() != T_LPAREN) advance();
    skip_to_matching_paren();

    int over = emit_jump(OP_JMP);
    FuncState fs;
    memset(&fs, 0, sizeof fs);
    fs.fidx = f->fidx;
    fs.isMain = false;
    FuncState *prev = C.F;
    C.F = &fs;
    for (int i = 0; i < f->nparams; i++) {
        for (int j = 0; j < i; j++)
            if (!strcmp(f->pnames[i], f->pnames[j])) cerror_at(nm, "Parameter '%s' appears twice", f->pnames[i]);
        declare_local_slot(f->pnames[i], f->ptypes[i], nm);
    }
    C.prog->funcs[f->fidx].entry = here();
    static const TokType stops[] = {T_ENDFUNCTION};
    block(stops, 1, fnTok);
    expect(T_ENDFUNCTION, "endfunction");
    emit_op1(OP_PUSH_DEFAULT, ret_kind(f->ret));
    emit_op(OP_RET);

    FuncDef *fd = &C.prog->funcs[f->fidx];
    fd->nlocals = fs.nlocals;
    fd->localTypes = gb_alloc(sizeof(VType) * (size_t)(fs.nlocals ? fs.nlocals : 1));
    for (int i = 0; i < fs.nlocals; i++) fd->localTypes[i] = fs.locals[i].type;
    C.F = prev;
    patch_here(over);
}

static void return_statement(void) {
    if (C.F->isMain) {
        if (!at_stmt_end()) { VType t = expr(); (void)t; emit_op(OP_POP); }
        emit_op(OP_END);
        return;
    }
    FuncDef *fd = &C.prog->funcs[C.F->fidx];
    if (at_stmt_end()) emit_op1(OP_PUSH_DEFAULT, ret_kind(fd->ret));
    else if (fd->ret.base == K_VOID && !fd->ret.dims)
        cerror("%s has no return type, so it returns nothing: give it one to return a value, "
               "e.g. function %s:int() (superstrict)", fd->name, fd->name);
    else {
        VType t = expr();
        coerce(t, fd->ret, "return value");
    }
    emit_op(OP_RET);
}

static void statement(void) {
    Token *t = peek();
    switch (t->type) {
        case T_STRICT: advance(); C.strict = true; return;
        case T_SUPERSTRICT: advance(); C.strict = true; return;   /* the type rules key off superFrom */
        case T_LOCAL: advance(); decl_statement(false, false); return;
        case T_GLOBAL:
            advance();
            if (!C.F->isMain) cerror_at(t, "global can only be declared outside functions");
            decl_statement(true, false);
            return;
        case T_DIM: advance(); decl_statement(C.F->isMain, true); return;
        case T_CONST: advance(); const_statement(); return;
        case T_IF: advance(); if_statement(t); return;
        case T_WHILE: advance(); while_statement(t); return;
        case T_REPEAT: advance(); repeat_statement(t); return;
        case T_FOR: advance(); for_statement(t); return;
        case T_SELECT: advance(); select_statement(t); return;
        case T_EXIT:
        case T_CONTINUE: {
            advance();
            if (C.F->nloops == 0) cerror_at(t, "%s used outside of a loop", t->text);
            Loop *l = &C.F->loops[C.F->nloops - 1];
            il_push(t->type == T_EXIT ? &l->breaks : &l->conts, emit_jump(OP_JMP));
            return;
        }
        case T_RETURN: advance(); return_statement(); return;
        case T_END: advance(); emit_op(OP_END); return;
        case T_DELETE:
            cerror_at(t, "delete was removed: objects are freed automatically once nothing refers to them. "
                         "Set the variable to null, or list_remove it from its list");
        case T_FUNCTION: advance(); function_statement(t); return;
        case T_TYPE: {
            if (!C.F->isMain || C.depth > 0) cerror_at(t, "Types must be declared at the top level");
            while (pt() != T_ENDTYPE) {
                if (pt() == T_EOF) cerror_at(t, "type on line %d is never closed (expected endtype)", t->line);
                advance();
            }
            advance();
            return;
        }
        case T_IDENT: ident_statement(); return;
        case T_ENDFUNCTION: cerror("endfunction without function");
        case T_ENDIF: cerror("endif without if");
        case T_WEND: cerror("wend without while");
        case T_NEXT: cerror("next without for");
        case T_UNTIL: cerror("until without repeat");
        case T_ENDSELECT: cerror("endselect without select");
        case T_ENDTYPE: cerror("endtype without type");
        case T_ELSE: cerror("else without if");
        case T_ELSEIF: cerror("elseif without if");
        case T_FIELD: cerror("field must be inside a type");
        default: cerror("Unexpected %s", tok_desc(t));
    }
}

/* ------------------------------------------------------------------ */
/* prescan                                                             */
/* ------------------------------------------------------------------ */

static void add_global(Token *name, VType t) {
    if (find_global(name->lower)) cerror_at(name, "global '%s' is declared twice", name->text);
    if (C.nglobals == C.capGlobals) {
        C.capGlobals = C.capGlobals ? C.capGlobals * 2 : 32;
        C.globals = gb_realloc(C.globals, sizeof(Var) * (size_t)C.capGlobals);
    }
    Var *g = &C.globals[C.nglobals];
    g->lname = gb_strdup(name->lower);
    g->type = t;
    g->slot = C.nglobals++;
}

static void skip_initializer(void) {
    int depth = 0;
    for (;;) {
        TokType t = pt();
        if (t == T_EOF || t == T_NEWLINE) return;
        if (depth == 0 && (t == T_COMMA || t == T_COLON)) return;
        if (t == T_LPAREN || t == T_LBRACK) depth++;
        if (t == T_RPAREN || t == T_RBRACK) depth--;
        advance();
    }
}

static int count_dims_skip(void) {
    advance(); /* [ */
    int depth = 1, commas = 0;
    while (depth > 0) {
        TokType t = pt();
        if (t == T_EOF || t == T_NEWLINE) cerror("Missing ']'");
        advance();
        if (t == T_LBRACK || t == T_LPAREN) depth++;
        else if (t == T_RBRACK || t == T_RPAREN) depth--;
        else if (t == T_COMMA && depth == 1) commas++;
    }
    return commas + 1;
}

static void prescan_globals_decl(void) {
    do {
        Token *name = expect(T_IDENT, "a variable name");
        VType t = parse_annot(name, NULL);
        if (pt() == T_LBRACK) t.dims = (uint8_t)count_dims_skip();
        add_global(name, t);
        if (match(T_EQ)) skip_initializer();
    } while (match(T_COMMA));
}

static void prescan_function(void) {
    Token *name = expect(T_IDENT, "a function name");
    if (find_func(name->lower)) cerror_at(name, "function '%s' is defined twice", name->text);
    bool retEx;
    VType ret = parse_annot(name, &retEx);
    if (!retEx && superstrict_at(name)) ret = vt(K_VOID);   /* superstrict: no return type means it returns nothing */
    int d = parse_empty_dims();
    if (d < 0) cerror("A function's array return type is written without a size, e.g. function f:int[]()");
    if (d > 0 && ret.base == K_VOID)
        cerror_at(name, "'%s' returns an array, so it needs a type, e.g. function %s:int[]() (superstrict)", name->text, name->text);
    ret.dims = (uint8_t)d;
    expect(T_LPAREN, "'(' after the function name");
    int cap = 8, n = 0;
    VType *ptypes = gb_alloc(sizeof(VType) * (size_t)cap);
    char **pnames = gb_alloc(sizeof(char *) * (size_t)cap);
    int *pdef = gb_alloc(sizeof(int) * (size_t)cap);
    if (pt() != T_RPAREN) {
        do {
            Token *pn = expect(T_IDENT, "a parameter name");
            VType ptp = parse_typed(pn, "function f(%s:int)");
            int pd = parse_empty_dims();
            if (pd < 0) cerror("Array parameters are written without a size, e.g. a:int[]");
            ptp.dims = (uint8_t)pd;
            if (n == cap) {
                cap *= 2;
                ptypes = gb_realloc(ptypes, sizeof(VType) * (size_t)cap);
                pnames = gb_realloc(pnames, sizeof(char *) * (size_t)cap);
                pdef = gb_realloc(pdef, sizeof(int) * (size_t)cap);
            }
            ptypes[n] = ptp;
            pnames[n] = gb_strdup(pn->lower);
            pdef[n] = -1;
            if (match(T_EQ)) {
                pdef[n] = C.pos;
                int depth = 0;
                while (!(depth == 0 && (pt() == T_COMMA || pt() == T_RPAREN))) {
                    if (pt() == T_NEWLINE || pt() == T_EOF) cerror("Missing ')' in function header");
                    if (pt() == T_LPAREN) depth++;
                    if (pt() == T_RPAREN) depth--;
                    advance();
                }
            } else if (n > 0 && pdef[n - 1] >= 0) {
                cerror_at(pn, "Parameter '%s' needs a default value because the one before it has one", pn->text);
            }
            n++;
        } while (match(T_COMMA));
    }
    expect(T_RPAREN, "')'");

    Program *p = C.prog;
    if (p->nfuncs == p->capFuncs) {
        p->capFuncs = p->capFuncs ? p->capFuncs * 2 : 32;
        p->funcs = gb_realloc(p->funcs, sizeof(FuncDef) * (size_t)p->capFuncs);
    }
    FuncDef *fd = &p->funcs[p->nfuncs];
    memset(fd, 0, sizeof *fd);
    fd->name = gb_strdup(name->text);
    fd->nparams = n;
    fd->params = ptypes;
    fd->ret = ret;
    fd->entry = -1;

    if (C.nfuncs == C.capFuncs) {
        C.capFuncs = C.capFuncs ? C.capFuncs * 2 : 32;
        C.funcs = gb_realloc(C.funcs, sizeof(FuncSym) * (size_t)C.capFuncs);
    }
    FuncSym *fs = &C.funcs[C.nfuncs++];
    fs->lname = gb_strdup(name->lower);
    fs->name = gb_strdup(name->text);
    fs->fidx = p->nfuncs++;
    fs->nparams = n;
    fs->ptypes = ptypes;
    fs->pnames = pnames;
    fs->pdefault = pdef;
    fs->ret = ret;
}

static void prescan(void) {
    TokenList *tl = C.tl;
    Program *p = C.prog;
    /* pass 1: type names */
    for (int i = 0; i < tl->count; i++) {
        if (tl->toks[i].type != T_TYPE) continue;
        Token *nm = &tl->toks[i + 1];
        if (nm->type != T_IDENT) cerror_at(nm, "Expected a type name after type");
        if (is_type_name(nm->lower)) cerror_at(nm, "type '%s' is already defined", nm->text);
        if (p->ntypes == p->capTypes) {
            p->capTypes = p->capTypes ? p->capTypes * 2 : 16;
            p->types = gb_realloc(p->types, sizeof(TypeDef) * (size_t)p->capTypes);
        }
        TypeDef *td = &p->types[p->ntypes++];
        memset(td, 0, sizeof *td);
        td->name = gb_strdup(nm->text);
        td->lname = gb_strdup(nm->lower);
    }
    /* pass 2: fields */
    for (int i = 0; i < tl->count; i++) {
        if (tl->toks[i].type != T_TYPE) continue;
        TypeDef *td = &p->types[find_type(tl->toks[i + 1].lower)];
        C.pos = i + 2;
        int cap = 0;
        while (pt() != T_ENDTYPE) {
            if (pt() == T_EOF) cerror_at(&tl->toks[i], "type '%s' is never closed (expected endtype)", td->name);
            if (pt() == T_NEWLINE || pt() == T_COLON) { advance(); continue; }
            if (pt() == T_FUNCTION || (pt() == T_IDENT && !strcmp(peek()->lower, "method")))
                cerror("GooberBASIC types hold data only; write a function that takes the object instead");
            expect(T_FIELD, "field");
            do {
                Token *fn = expect(T_IDENT, "a field name");
                VType ft = parse_typed(fn, "field %s:int");
                int d = parse_empty_dims();
                if (d < 0) cerror("Array fields are declared without a size, e.g. field items:int[]");
                ft.dims = (uint8_t)d;
                for (int k = 0; k < td->nfields; k++)
                    if (!strcmp(td->fieldNames[k], fn->lower)) cerror_at(fn, "field '%s' appears twice", fn->text);
                if (pt() == T_EQ) cerror("Fields cannot have initial values; set them after new");
                if (td->nfields == cap) {
                    cap = cap ? cap * 2 : 8;
                    td->fieldNames = gb_realloc(td->fieldNames, sizeof(char *) * (size_t)cap);
                    td->fieldTypes = gb_realloc(td->fieldTypes, sizeof(VType) * (size_t)cap);
                }
                td->fieldNames[td->nfields] = gb_strdup(fn->lower);
                td->fieldTypes[td->nfields] = ft;
                td->nfields++;
            } while (match(T_COMMA));
        }
    }
    /* pass 3: function signatures */
    for (int i = 0; i < tl->count; i++) {
        if (tl->toks[i].type != T_FUNCTION) continue;
        C.pos = i + 1;
        prescan_function();
    }
    /* pass 4: top-level globals */
    bool inFunc = false;
    for (int i = 0; i < tl->count; i++) {
        TokType t = tl->toks[i].type;
        if (t == T_FUNCTION) inFunc = true;
        else if (t == T_ENDFUNCTION) inFunc = false;
        else if (!inFunc && (t == T_GLOBAL || t == T_DIM)) {
            C.pos = i + 1;
            prescan_globals_decl();
        }
    }
    C.pos = 0;
}

/* ------------------------------------------------------------------ */

void compile_program(TokenList *tl, Program *prog) {
    memset(&C, 0, sizeof C);
    C.tl = tl;
    C.prog = prog;
    C.superFrom = tl->count;
    for (int i = 0; i < tl->count; i++)
        if (tl->toks[i].type == T_SUPERSTRICT) { C.superFrom = i; break; }

    add_builtin_const("true", vt(K_INT), 1, 0);
    add_builtin_const("false", vt(K_INT), 0, 0);
    add_builtin_const("pi", vt(K_FLOAT), 0, 3.14159265358979323846);
    for (int i = 0; gb_gfx_consts[i].name; i++)
        add_builtin_const(gb_gfx_consts[i].name, vt(K_INT), gb_gfx_consts[i].value, 0);

    /* function 0 is the main program */
    prog->capFuncs = 32;
    prog->funcs = gb_alloc(sizeof(FuncDef) * (size_t)prog->capFuncs);
    prog->funcs[0].name = gb_strdup("<main>");
    prog->funcs[0].ret = vt(K_INT);
    prog->funcs[0].entry = 0;
    prog->nfuncs = 1;

    C.mainState.fidx = 0;
    C.mainState.isMain = true;
    C.F = &C.mainState;

    prescan();

    while (pt() != T_EOF) {
        if (pt() == T_NEWLINE || pt() == T_COLON) { advance(); continue; }
        statement();
        end_of_statement();
    }
    emit_op(OP_END);

    FuncDef *mainFd = &prog->funcs[0];
    mainFd->nlocals = C.mainState.nlocals;
    mainFd->localTypes = gb_alloc(sizeof(VType) * (size_t)(C.mainState.nlocals ? C.mainState.nlocals : 1));
    for (int i = 0; i < C.mainState.nlocals; i++) mainFd->localTypes[i] = C.mainState.locals[i].type;

    prog->nglobals = C.nglobals;
    prog->globalTypes = gb_alloc(sizeof(VType) * (size_t)(C.nglobals ? C.nglobals : 1));
    for (int i = 0; i < C.nglobals; i++) prog->globalTypes[i] = C.globals[i].type;
}

/* For editors (GooberEngine's syntax colouring): 1 = a built-in type name,
 * 2 = a function the compiler handles itself (list commands, len, str, abs...),
 * 0 = neither. Keep the list in step with intrinsic() above. */
int compiler_builtin_word(const char *lower) {
    if (num_type_find(lower) || !strcmp(lower, "string")) return 1;
    if (list_fn_index(lower) >= 0) return 2;
    static const char *const fns[] = {"len", "str", "abs", "sgn", "min", "max", NULL};
    for (int i = 0; fns[i]; i++)
        if (!strcmp(lower, fns[i])) return 2;
    return 0;
}
