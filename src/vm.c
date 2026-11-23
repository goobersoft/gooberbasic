/* GooberBASIC virtual machine.
 *
 * A stack machine. Because the compiler has already checked every type,
 * each opcode knows exactly what kind of values it gets (ADDI adds two Ints,
 * ADDF two Floats, ...), so the hot loop never inspects tags except where
 * a Null object has to be reported.
 */
#include "vm.h"
#include <stdarg.h>
#include <math.h>
#include <errno.h>

static int32_t read_i32(const uint8_t *p) {
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void line_of(VM *vm, uint8_t *ip, const char **file, int *line) {
    Program *p = vm->prog;
    long off = ip ? (long)(ip - p->code) - 1 : 0;
    if (off < 0) off = 0;
    if (off >= p->count) off = p->count - 1;
    int32_t packed = p->count > 0 ? p->lines[off] : 0;
    int fi = packed >> 20;
    *file = (fi >= 0 && fi < p->nfiles) ? p->files[fi] : "?";
    *line = packed & 0xFFFFF;
}

void vm_error(VM *vm, const char *fmt, ...) {
    const char *file;
    int line;
    line_of(vm, vm->ip, &file, &line);
    fflush(stdout);
    char msg[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    gb_diag("%s:%d: runtime error: %s\n", file, line, msg);
    /* short call trace */
    for (int i = vm->nframes - 1, shown = 0; i > 0 && shown < 8; i--, shown++) {
        const char *cf;
        int cl;
        line_of(vm, vm->frames[i - 1].ip, &cf, &cl);
        gb_diag("  called from %s at %s:%d\n", vm->prog->funcs[vm->frames[i].func].name, cf, cl);
    }
    vm_exit(vm, 1);
}

void vm_exit(VM *vm, int code) {
    vm->exitCode = code;
    longjmp(vm->exitJmp, 1);
}

void vm_init(VM *vm, Program *prog) {
    memset(vm, 0, sizeof *vm);
    vm->prog = prog;
    vm->stack = gb_alloc(sizeof(Value) * GB_STACK_SIZE);
    vm->sp = vm->stack;
    vm->globals = gb_alloc(sizeof(Value) * (size_t)(prog->nglobals ? prog->nglobals : 1));
    for (int i = 0; i < prog->nglobals; i++) {
        VType t = prog->globalTypes[i];
        vm->globals[i] = default_value(t.dims ? K_ARRAY : t.base);
    }
    vm->nextGC = 8u << 20;
    vm->gcStress = getenv("GOOBER_GC_STRESS") != NULL;
    gb_vm = vm;
}

void vm_free(VM *vm) {
    gc_free_all(vm);
    free(vm->stack);
    free(vm->globals);
}

/* ---- string helpers used by the dispatch loop ---- */

static ObjString *concat(VM *vm, ObjString *a, ObjString *b) {
    int la = str_len(a), lb = str_len(b);
    if (la == 0) return b;
    if (lb == 0) return a;
    char *buf = malloc((size_t)(la + lb));
    memcpy(buf, a->chars, (size_t)la);
    memcpy(buf + la, b->chars, (size_t)lb);
    ObjString *r = str_new(vm, buf, la + lb);
    free(buf);
    return r;
}

static int str_cmp(ObjString *a, ObjString *b) {
    int la = str_len(a), lb = str_len(b);
    int n = la < lb ? la : lb;
    int c = n ? memcmp(str_chars(a), str_chars(b), (size_t)n) : 0;
    if (c) return c;
    return la - lb;
}

static ObjString *int_to_str(VM *vm, int32_t i) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%d", i);
    return str_new(vm, buf, n);
}

static ObjString *float_to_str(VM *vm, double f) {
    char buf[64];
    gb_format_float(f, buf, sizeof buf);
    return str_from_c(vm, buf);
}

/* Blitz-style lenient conversion: leading number is used, junk ignored */
static int32_t str_to_int(ObjString *s) {
    const char *p = str_chars(s);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '$') return (int32_t)strtoul(p + 1, NULL, 16);
    if (*p == '%') return (int32_t)strtoul(p + 1, NULL, 2);
    double d = strtod(p, NULL);
    if (d > 2147483647.0) return 2147483647;
    if (d < -2147483648.0) return (int32_t)0x80000000u;
    return (int32_t)d;
}

static double str_to_float(ObjString *s) { return strtod(str_chars(s), NULL); }

static ObjString *long_to_str(VM *vm, int64_t l) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)l);
    return str_new(vm, buf, n);
}

static ObjString *ulong_to_str(VM *vm, uint64_t u) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%llu", (unsigned long long)u);
    return str_new(vm, buf, n);
}

/* Lenient like str_to_int, but 64 bits wide (saturating). */
static int64_t str_to_long(ObjString *s) {
    const char *p = str_chars(s);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '$') return (int64_t)strtoull(p + 1, NULL, 16);
    if (*p == '%') return (int64_t)strtoull(p + 1, NULL, 2);
    char *end;
    errno = 0;
    long long v = strtoll(p, &end, 10);
    if (*end == '.' || *end == 'e' || *end == 'E') {   /* "1.5e3": go through double */
        double d = strtod(p, NULL);
        if (d != d) return 0;
        if (d >= 9223372036854775807.0) return INT64_MAX;
        if (d <= -9223372036854775808.0) return INT64_MIN;
        return (int64_t)d;
    }
    return (int64_t)v;
}

static uint64_t str_to_ulong(ObjString *s) {
    const char *p = str_chars(s);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '$') return strtoull(p + 1, NULL, 16);
    if (*p == '%') return strtoull(p + 1, NULL, 2);
    if (*p == '-') return 0;
    char *end;
    errno = 0;
    unsigned long long v = strtoull(p, &end, 10);
    if (*end == '.' || *end == 'e' || *end == 'E') {
        double d = strtod(p, NULL);
        if (!(d > 0)) return 0;
        if (d >= 18446744073709551616.0) return UINT64_MAX;
        return (uint64_t)d;
    }
    return (uint64_t)v;
}

/* Saturating float -> 64-bit conversions (NaN -> 0), like f2i. */
static int64_t f2l(double f) {
    if (f != f) return 0;
    if (f >= 9223372036854775807.0) return INT64_MAX;
    if (f <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)f;
}

static uint64_t f2u(double f) {
    if (!(f > 0)) return 0;
    if (f >= 18446744073709551616.0) return UINT64_MAX;
    return (uint64_t)f;
}

/* Wrap (or round) the value in `v` to the storage type `nt`. */
static void narrow(Value *v, int nt) {
    switch (nt) {
        case NT_S8: v->as.i = (int8_t)v->as.i; break;
        case NT_U8: v->as.i = (uint8_t)v->as.i; break;
        case NT_S16: v->as.i = (int16_t)v->as.i; break;
        case NT_U16: v->as.i = (uint16_t)v->as.i; break;
        case NT_U32: v->as.i64 = (int64_t)(uint32_t)v->as.i64; break;
        case NT_F32: v->as.f = (double)(float)v->as.f; break;
        default: break;
    }
}

static int32_t f2i(double f) {
    if (f != f) return 0;
    if (f >= 2147483647.0) return 2147483647;
    if (f <= -2147483648.0) return (int32_t)0x80000000u;
    return (int32_t)f;
}

/* Convert an int, long, float or string value to `to` (K_INT, K_FLOAT or
 * K_STRING) the way an assignment would: floats saturate into an int, longs
 * wrap, strings are read leniently. Used by read_int / read_float / read_string. */
Value vm_convert(VM *vm, Value v, uint8_t to) {
    switch (to) {
        case K_INT:
            if (v.kind == K_FLOAT) return v_int(f2i(v.as.f));
            if (v.kind == K_LONG) return v_int((int32_t)(uint32_t)(uint64_t)v.as.i64);
            if (v.kind == K_STRING) return v_int(str_to_int(v.as.s));
            return v;
        case K_FLOAT:
            if (v.kind == K_INT) return v_float((double)v.as.i);
            if (v.kind == K_LONG) return v_float((double)v.as.i64);
            if (v.kind == K_STRING) return v_float(str_to_float(v.as.s));
            return v;
        default:
            if (v.kind == K_INT) return v_str(int_to_str(vm, v.as.i));
            if (v.kind == K_LONG) return v_str(long_to_str(vm, v.as.i64));
            if (v.kind == K_FLOAT) return v_str(float_to_str(vm, v.as.f));
            return v;
    }
}

/* First link at or after `k` that is still in its list. */
static ObjLink *iter_skip(ObjLink *k) {
    while (k && !k->list) k = k->next;
    return k;
}

/* ---- the interpreter loop ---- */

#define READ_I32() (ip += 4, read_i32(ip - 4))
#define TOP (sp[-1])
#define POP() (*--sp)
#define PUSH(v) (*sp++ = (v))
#define ERR(...) do { vm->ip = ip; vm->sp = sp; vm_error(vm, __VA_ARGS__); } while (0)

/* The upvalue for stack slot `loc`: the open one already made for it, or a new one. */
static ObjUpvalue *capture_upvalue(VM *vm, Value *loc) {
    ObjUpvalue **link = &vm->openUpvalues;
    while (*link && (*link)->loc > loc) link = &(*link)->nextOpen;
    if (*link && (*link)->loc == loc) return *link;
    ObjUpvalue *u = upvalue_new(vm, loc);
    u->nextOpen = *link;
    *link = u;
    return u;
}

/* A function is returning: its locals at `from` and above move into their upvalues. */
static void close_upvalues(VM *vm, Value *from) {
    while (vm->openUpvalues && vm->openUpvalues->loc >= from) {
        ObjUpvalue *u = vm->openUpvalues;
        u->closed = *u->loc;
        u->loc = &u->closed;
        vm->openUpvalues = u->nextOpen;
        u->nextOpen = NULL;
    }
}

static void call_function(VM *vm, int fidx, int argc, Value **spp, uint8_t **ipp, ObjClosure *closure) {
    FuncDef *fd = &vm->prog->funcs[fidx];
    Value *sp = *spp;
    if (vm->nframes >= GB_MAX_FRAMES || sp + fd->nlocals + 256 >= vm->stack + GB_STACK_SIZE) {
        vm->ip = *ipp;
        vm->sp = sp;
        vm_error(vm, "Stack overflow (too much recursion in %s)", fd->name);
    }
    vm->frames[vm->nframes - 1].ip = *ipp;
    Frame *f = &vm->frames[vm->nframes++];
    f->func = fidx;
    f->base = sp - argc;
    f->closure = closure;
    for (int i = argc; i < fd->nlocals; i++) {
        VType t = fd->localTypes[i];
        *sp++ = default_value(t.dims ? K_ARRAY : t.base);
    }
    *spp = sp;
    *ipp = vm->prog->code + fd->entry;
}

int vm_run(VM *vm) {
    Program *prog = vm->prog;
    if (setjmp(vm->exitJmp)) {
        return vm->exitCode;
    }

    /* enter main */
    vm->nframes = 1;
    Frame *frame = &vm->frames[0];
    frame->func = 0;
    frame->base = vm->stack;
    frame->closure = NULL;
    vm->openUpvalues = NULL;
    Value *sp = vm->stack;
    FuncDef *mainFd = &prog->funcs[0];
    for (int i = 0; i < mainFd->nlocals; i++) {
        VType t = mainFd->localTypes[i];
        *sp++ = default_value(t.dims ? K_ARRAY : t.base);
    }
    uint8_t *ip = prog->code + mainFd->entry;
    Value *base = frame->base;
    ObjClosure *cl = NULL;   /* the function value being run (for its upvalues) */
    int poll_left = GB_POLL_EVERY;
    /* a loop jumping back: let the host look for a stop request now and then */
#define BACK_EDGE() do { \
        if (gb_poll_hook && --poll_left <= 0) { poll_left = GB_POLL_EVERY; vm->sp = sp; gb_poll_hook(); } \
    } while (0)

    for (;;) {
        uint8_t op = *ip++;
        switch (op) {
            case OP_NOP: break;
            case OP_PUSH_INT: { int32_t v = READ_I32(); PUSH(v_int(v)); break; }
            case OP_PUSH_CONST: { int32_t i = READ_I32(); PUSH(prog->consts[i]); break; }
            case OP_PUSH_DEFAULT: { int32_t k = READ_I32(); PUSH(default_value((uint8_t)k)); break; }
            case OP_POP: sp--; break;
            case OP_DUPN: {
                int32_t n = READ_I32();
                for (int i = 0; i < n; i++) sp[i] = sp[i - n];
                sp += n;
                break;
            }
            case OP_LOAD_LOCAL: { int32_t s = READ_I32(); PUSH(base[s]); break; }
            case OP_STORE_LOCAL: { int32_t s = READ_I32(); base[s] = POP(); break; }
            case OP_LOAD_GLOBAL: { int32_t s = READ_I32(); PUSH(vm->globals[s]); break; }
            case OP_STORE_GLOBAL: { int32_t s = READ_I32(); vm->globals[s] = POP(); break; }

            /* Int arithmetic wraps around like 32-bit hardware */
            case OP_ADDI: sp--; TOP.as.i = (int32_t)((uint32_t)TOP.as.i + (uint32_t)sp->as.i); break;
            case OP_SUBI: sp--; TOP.as.i = (int32_t)((uint32_t)TOP.as.i - (uint32_t)sp->as.i); break;
            case OP_MULI: sp--; TOP.as.i = (int32_t)((uint32_t)TOP.as.i * (uint32_t)sp->as.i); break;
            case OP_DIVI:
                sp--;
                if (sp->as.i == 0) ERR("Integer division by zero");
                if (sp->as.i == -1) TOP.as.i = (int32_t)(0u - (uint32_t)TOP.as.i);
                else TOP.as.i /= sp->as.i;
                break;
            case OP_MODI:
                sp--;
                if (sp->as.i == 0) ERR("Integer division by zero (Mod)");
                if (sp->as.i == -1) TOP.as.i = 0;
                else TOP.as.i %= sp->as.i;
                break;
            case OP_NEGI: TOP.as.i = (int32_t)(0u - (uint32_t)TOP.as.i); break;
            case OP_ADDF: sp--; TOP.as.f += sp->as.f; break;
            case OP_SUBF: sp--; TOP.as.f -= sp->as.f; break;
            case OP_MULF: sp--; TOP.as.f *= sp->as.f; break;
            case OP_DIVF: sp--; TOP.as.f /= sp->as.f; break;
            case OP_MODF: sp--; TOP.as.f = fmod(TOP.as.f, sp->as.f); break;
            case OP_NEGF: TOP.as.f = -TOP.as.f; break;
            case OP_POWF: sp--; TOP.as.f = pow(TOP.as.f, sp->as.f); break;
            case OP_CONCAT: {
                vm->sp = sp;
                ObjString *r = concat(vm, sp[-2].as.s, sp[-1].as.s);
                sp--;
                TOP = v_str(r);
                break;
            }

#define CMP(opc, field, expr) case opc: sp--; TOP = v_int((TOP.as.field expr sp->as.field) ? 1 : 0); break;
            CMP(OP_EQI, i, ==) CMP(OP_NEI, i, !=) CMP(OP_LTI, i, <) CMP(OP_GTI, i, >) CMP(OP_LEI, i, <=) CMP(OP_GEI, i, >=)
            CMP(OP_EQF, f, ==) CMP(OP_NEF, f, !=) CMP(OP_LTF, f, <) CMP(OP_GTF, f, >) CMP(OP_LEF, f, <=) CMP(OP_GEF, f, >=)
            CMP(OP_EQO, obj, ==) CMP(OP_NEO, obj, !=)
#undef CMP
#define SCMP(opc, expr) case opc: { sp--; int c = str_cmp(TOP.as.s, sp->as.s); TOP = v_int((c expr 0) ? 1 : 0); break; }
            SCMP(OP_EQS, ==) SCMP(OP_NES, !=) SCMP(OP_LTS, <) SCMP(OP_GTS, >) SCMP(OP_LES, <=) SCMP(OP_GES, >=)
#undef SCMP

            case OP_NOT: TOP.as.i = TOP.as.i == 0; break;
            case OP_BAND: sp--; TOP.as.i &= sp->as.i; break;
            case OP_BOR: sp--; TOP.as.i |= sp->as.i; break;
            case OP_BXOR: sp--; TOP.as.i ^= sp->as.i; break;
            case OP_BNOT: TOP.as.i = ~TOP.as.i; break;
            case OP_SHL: sp--; TOP.as.i = (int32_t)((uint32_t)TOP.as.i << (sp->as.i & 31)); break;
            case OP_SHR: sp--; TOP.as.i = (int32_t)((uint32_t)TOP.as.i >> (sp->as.i & 31)); break;
            case OP_SAR: sp--; TOP.as.i = TOP.as.i >> (sp->as.i & 31); break;

            case OP_I2F: TOP = v_float((double)TOP.as.i); break;
            case OP_F2I: TOP = v_int(f2i(TOP.as.f)); break;
            case OP_I2S: { vm->sp = sp; ObjString *s = int_to_str(vm, TOP.as.i); TOP = v_str(s); break; }
            case OP_F2S: { vm->sp = sp; ObjString *s = float_to_str(vm, TOP.as.f); TOP = v_str(s); break; }
            case OP_S2I: TOP = v_int(str_to_int(TOP.as.s)); break;
            case OP_S2F: TOP = v_float(str_to_float(TOP.as.s)); break;
            case OP_I2F_UNDER: sp[-2] = v_float((double)sp[-2].as.i); break;
            case OP_I2S_UNDER: { vm->sp = sp; ObjString *s = int_to_str(vm, sp[-2].as.i); sp[-2] = v_str(s); break; }
            case OP_F2S_UNDER: { vm->sp = sp; ObjString *s = float_to_str(vm, sp[-2].as.f); sp[-2] = v_str(s); break; }
            case OP_SWAP: { Value t = sp[-1]; sp[-1] = sp[-2]; sp[-2] = t; break; }
            case OP_NARROW: { int32_t nt = READ_I32(); narrow(&TOP, nt); break; }

            /* 64-bit integer arithmetic also wraps */
            case OP_ADDL: sp--; TOP.as.i64 = (int64_t)((uint64_t)TOP.as.i64 + (uint64_t)sp->as.i64); break;
            case OP_SUBL: sp--; TOP.as.i64 = (int64_t)((uint64_t)TOP.as.i64 - (uint64_t)sp->as.i64); break;
            case OP_MULL: sp--; TOP.as.i64 = (int64_t)((uint64_t)TOP.as.i64 * (uint64_t)sp->as.i64); break;
            case OP_DIVL:
                sp--;
                if (sp->as.i64 == 0) ERR("Integer division by zero");
                if (sp->as.i64 == -1) TOP.as.i64 = (int64_t)(0u - (uint64_t)TOP.as.i64);
                else TOP.as.i64 /= sp->as.i64;
                break;
            case OP_MODL:
                sp--;
                if (sp->as.i64 == 0) ERR("Integer division by zero (Mod)");
                if (sp->as.i64 == -1) TOP.as.i64 = 0;
                else TOP.as.i64 %= sp->as.i64;
                break;
            case OP_NEGL: TOP.as.i64 = (int64_t)(0u - (uint64_t)TOP.as.i64); break;
            case OP_DIVU:
                sp--;
                if (sp->as.i64 == 0) ERR("Integer division by zero");
                TOP.as.i64 = (int64_t)((uint64_t)TOP.as.i64 / (uint64_t)sp->as.i64);
                break;
            case OP_MODU:
                sp--;
                if (sp->as.i64 == 0) ERR("Integer division by zero (Mod)");
                TOP.as.i64 = (int64_t)((uint64_t)TOP.as.i64 % (uint64_t)sp->as.i64);
                break;
#define LCMP(opc, T, expr) case opc: sp--; TOP = v_int(((T)TOP.as.i64 expr (T)sp->as.i64) ? 1 : 0); break;
            LCMP(OP_EQL, int64_t, ==) LCMP(OP_NEL, int64_t, !=) LCMP(OP_LTL, int64_t, <)
            LCMP(OP_GTL, int64_t, >) LCMP(OP_LEL, int64_t, <=) LCMP(OP_GEL, int64_t, >=)
            LCMP(OP_LTU, uint64_t, <) LCMP(OP_GTU, uint64_t, >) LCMP(OP_LEU, uint64_t, <=) LCMP(OP_GEU, uint64_t, >=)
#undef LCMP
            case OP_BANDL: sp--; TOP.as.i64 &= sp->as.i64; break;
            case OP_BORL: sp--; TOP.as.i64 |= sp->as.i64; break;
            case OP_BXORL: sp--; TOP.as.i64 ^= sp->as.i64; break;
            case OP_BNOTL: TOP.as.i64 = ~TOP.as.i64; break;
            case OP_SHLL: sp--; TOP.as.i64 = (int64_t)((uint64_t)TOP.as.i64 << (sp->as.i64 & 63)); break;
            case OP_SHRL: sp--; TOP.as.i64 = (int64_t)((uint64_t)TOP.as.i64 >> (sp->as.i64 & 63)); break;
            case OP_SARL: sp--; TOP.as.i64 = TOP.as.i64 >> (sp->as.i64 & 63); break;
            case OP_I2L: TOP = v_long((int64_t)TOP.as.i); break;
            case OP_L2I: TOP = v_int((int32_t)(uint32_t)(uint64_t)TOP.as.i64); break;
            case OP_L2F: TOP = v_float((double)TOP.as.i64); break;
            case OP_U2F: TOP = v_float((double)(uint64_t)TOP.as.i64); break;
            case OP_F2L: TOP = v_long(f2l(TOP.as.f)); break;
            case OP_F2U: TOP = v_long((int64_t)f2u(TOP.as.f)); break;
            case OP_L2S: { vm->sp = sp; ObjString *s = long_to_str(vm, TOP.as.i64); TOP = v_str(s); break; }
            case OP_U2S: { vm->sp = sp; ObjString *s = ulong_to_str(vm, (uint64_t)TOP.as.i64); TOP = v_str(s); break; }
            case OP_S2L: TOP = v_long(str_to_long(TOP.as.s)); break;
            case OP_S2U: TOP = v_long((int64_t)str_to_ulong(TOP.as.s)); break;
            case OP_TRUTH_L: TOP = v_int(TOP.as.i64 != 0); break;

            case OP_TRUTH_I: TOP = v_int(TOP.as.i != 0); break;
            case OP_TRUTH_F: TOP = v_int(TOP.as.f != 0.0); break;
            case OP_TRUTH_S: TOP = v_int(str_len(TOP.as.s) != 0); break;
            case OP_TRUTH_O: TOP = v_int(TOP.as.obj != NULL); break;

            case OP_JMP: {
                uint8_t *to = prog->code + read_i32(ip);
                if (to < ip) BACK_EDGE();          /* while / for / loop */
                ip = to;
                break;
            }
            case OP_JF: {
                int32_t t = READ_I32();
                sp--;
                if (sp->as.i == 0) {
                    uint8_t *to = prog->code + t;
                    if (to < ip) BACK_EDGE();      /* do ... loop until */
                    ip = to;
                }
                break;
            }
            case OP_JT: {
                int32_t t = READ_I32();
                sp--;
                if (sp->as.i != 0) ip = prog->code + t;
                break;
            }

            case OP_CALL: {
                int32_t f = READ_I32();
                int32_t argc = READ_I32();
                call_function(vm, f, argc, &sp, &ip, NULL);
                base = vm->frames[vm->nframes - 1].base;
                cl = NULL;
                break;
            }
            case OP_CALL_VALUE: {
                int32_t argc = READ_I32();
                ObjClosure *c = sp[-argc - 1].as.c;
                if (!c) ERR("Called a null function");
                call_function(vm, c->fidx, argc, &sp, &ip, c);
                base = vm->frames[vm->nframes - 1].base;
                cl = c;
                break;
            }
            case OP_CLOSURE: {
                int32_t f = READ_I32();
                int32_t nup = READ_I32();
                vm->ip = ip;
                vm->sp = sp;
                ObjClosure *c = closure_new(vm, f, nup);
                PUSH(v_closure(c));   /* on the stack now, so it survives the upvalue allocations below */
                for (int i = 0; i < nup; i++) {
                    int32_t isLocal = READ_I32();
                    int32_t idx = READ_I32();
                    vm->sp = sp;
                    c->up[i] = isLocal ? capture_upvalue(vm, base + idx) : cl->up[idx];
                }
                break;
            }
            case OP_GET_UPVAL: { int32_t u = READ_I32(); PUSH(*cl->up[u]->loc); break; }
            case OP_SET_UPVAL: { int32_t u = READ_I32(); *cl->up[u]->loc = POP(); break; }
            /* the main program's frame is the bottom of the stack for the whole run */
            case OP_LOAD_MAIN: { int32_t s = READ_I32(); PUSH(vm->stack[s]); break; }
            case OP_STORE_MAIN: { int32_t s = READ_I32(); vm->stack[s] = POP(); break; }
            case OP_NATIVE: {
                int32_t n = READ_I32();
                int32_t argc = READ_I32();
                const NativeDef *nd = native_get(n);
                vm->ip = ip;
                vm->sp = sp;
                Value r = nd->fn(vm, argc, sp - argc);
                sp -= argc;
                PUSH(r);
                break;
            }
            case OP_RET: {
                Value r = POP();
                Frame *f = &vm->frames[--vm->nframes];
                close_upvalues(vm, f->base);
                sp = f->base - (f->closure ? 1 : 0);   /* a value call also drops the function */
                PUSH(r);
                Frame *caller = &vm->frames[vm->nframes - 1];
                ip = caller->ip;
                base = caller->base;
                cl = caller->closure;
                break;
            }

            case OP_NEW: {
                int32_t tid = READ_I32();
                vm->sp = sp;
                ObjInst *o = inst_new(vm, tid);
                PUSH(v_obj(o));
                break;
            }
            case OP_GETF: {
                int32_t fi = READ_I32();
                ObjInst *o = TOP.as.o;
                if (!o) ERR("Object does not exist (null)");
                TOP = o->fields[fi];
                break;
            }
            case OP_SETF: {
                int32_t fi = READ_I32();
                Value v = POP();
                ObjInst *o = POP().as.o;
                if (!o) ERR("Object does not exist (null)");
                o->fields[fi] = v;
                break;
            }
            case OP_ITER_FIRST: {
                ObjList *l = TOP.as.l;
                if (!l) ERR("eachin: the list is null");
                TOP = v_link(iter_skip(l->head));
                break;
            }
            case OP_ITER_NEXT: TOP = v_link(iter_skip(TOP.as.lk->next)); break;
            case OP_LINK_VALUE: TOP = TOP.as.lk->value; break;

            case OP_NEWARR: {
                int32_t ek = READ_I32();
                int32_t nd = READ_I32();
                int dims[GB_MAX_DIMS];
                for (int i = 0; i < nd; i++) dims[i] = sp[i - nd].as.i;
                vm->ip = ip;
                vm->sp = sp;
                ObjArray *a = array_new(vm, (uint8_t)ek, nd, dims);
                sp -= nd;
                PUSH(v_arr(a));
                break;
            }
            case OP_AGET:
            case OP_ASET: {
                int32_t nd = READ_I32();
                Value v = v_int(0);
                if (op == OP_ASET) v = POP();
                Value *idx = sp - nd;
                ObjArray *a = idx[-1].as.a;
                if (!a) ERR("Array is null (it was never given a size)");
                int off = 0;
                for (int i = 0; i < nd; i++) {
                    int32_t k = idx[i].as.i;
                    if (k < 0 || k >= a->dims[i]) {
                        if (nd == 1) ERR("Array index %d out of bounds (size is %d)", k, a->dims[0]);
                        ERR("Array index %d out of bounds in dimension %d (size is %d)", k, i + 1, a->dims[i]);
                    }
                    off = off * a->dims[i] + k;
                }
                sp = idx - 1;
                if (op == OP_AGET) PUSH(a->data[off]);
                else a->data[off] = v;
                break;
            }
            case OP_ALEN: {
                int32_t d = POP().as.i;
                ObjArray *a = TOP.as.a;
                if (!a) { TOP = v_int(0); break; }
                if (d < 1 || d > a->ndims) ERR("len: array has %d dimension(s), asked for %d", a->ndims, d);
                TOP = v_int(a->dims[d - 1]);
                break;
            }

            case OP_END:
                vm->sp = sp;
                return 0;

            default:
                ERR("internal error: bad opcode %d", op);
        }
    }
}

/* ---- disassembler (goober --disasm) ---- */

static const char *op_names[] = {
    "NOP", "PUSH_INT", "PUSH_CONST", "PUSH_DEFAULT", "POP", "DUPN",
    "LOAD_LOCAL", "STORE_LOCAL", "LOAD_GLOBAL", "STORE_GLOBAL",
    "ADDI", "SUBI", "MULI", "DIVI", "MODI", "NEGI",
    "ADDF", "SUBF", "MULF", "DIVF", "MODF", "NEGF", "POWF", "CONCAT",
    "EQI", "NEI", "LTI", "GTI", "LEI", "GEI",
    "EQF", "NEF", "LTF", "GTF", "LEF", "GEF",
    "EQS", "NES", "LTS", "GTS", "LES", "GES",
    "EQO", "NEO",
    "NOT", "BAND", "BOR", "BXOR", "BNOT", "SHL", "SHR", "SAR",
    "I2F", "F2I", "I2S", "F2S", "S2I", "S2F", "I2F_UNDER", "I2S_UNDER", "F2S_UNDER",
    "SWAP", "NARROW",
    "ADDL", "SUBL", "MULL", "DIVL", "MODL", "NEGL", "DIVU", "MODU",
    "EQL", "NEL", "LTL", "GTL", "LEL", "GEL", "LTU", "GTU", "LEU", "GEU",
    "BANDL", "BORL", "BXORL", "BNOTL", "SHLL", "SHRL", "SARL",
    "I2L", "L2I", "L2F", "U2F", "F2L", "F2U", "L2S", "U2S", "S2L", "S2U", "TRUTH_L",
    "TRUTH_I", "TRUTH_F", "TRUTH_S", "TRUTH_O",
    "JMP", "JF", "JT", "CALL", "CALL_VALUE", "CLOSURE", "GET_UPVAL", "SET_UPVAL", "LOAD_MAIN", "STORE_MAIN", "NATIVE", "RET",
    "NEW", "GETF", "SETF", "ITER_FIRST", "ITER_NEXT", "LINK_VALUE",
    "NEWARR", "AGET", "ASET", "ALEN", "END",
};

static int op_operands(uint8_t op) {
    switch (op) {
        case OP_PUSH_INT: case OP_PUSH_CONST: case OP_PUSH_DEFAULT: case OP_DUPN:
        case OP_LOAD_LOCAL: case OP_STORE_LOCAL: case OP_LOAD_GLOBAL: case OP_STORE_GLOBAL:
        case OP_JMP: case OP_JF: case OP_JT: case OP_NEW:
        case OP_GETF: case OP_SETF: case OP_AGET: case OP_ASET: case OP_NARROW:
        case OP_CALL_VALUE: case OP_GET_UPVAL: case OP_SET_UPVAL: case OP_LOAD_MAIN: case OP_STORE_MAIN:
            return 1;
        case OP_CALL: case OP_NATIVE: case OP_NEWARR:
            return 2;
        default:
            return 0;
    }
}

void disassemble(Program *p) {
    _Static_assert(sizeof(op_names) / sizeof(op_names[0]) == OP_END + 1, "op_names out of sync");
    for (int f = 0; f < p->nfuncs; f++)
        printf("; function %d %s entry=%d params=%d locals=%d\n", f, p->funcs[f].name, p->funcs[f].entry,
               p->funcs[f].nparams, p->funcs[f].nlocals);
    int lastLine = -1;
    for (int off = 0; off < p->count;) {
        for (int f = 0; f < p->nfuncs; f++)
            if (p->funcs[f].entry == off) printf("\n%s:\n", p->funcs[f].name);
        uint8_t op = p->code[off];
        int line = p->lines[off] & 0xFFFFF;
        if (line != lastLine) { printf("%5d ", line); lastLine = line; } else printf("    | ");
        printf("%06d %-13s", off, op <= OP_END ? op_names[op] : "???");
        int n = op_operands(op);
        if (op == OP_CLOSURE) n = 2 + 2 * read_i32(p->code + off + 5);   /* func, nup, then (isLocal, index) pairs */
        for (int i = 0; i < n; i++) printf(" %d", read_i32(p->code + off + 1 + 4 * i));
        if (op == OP_NATIVE) printf("    ; %s", native_get(read_i32(p->code + off + 1))->name);
        if (op == OP_CALL || op == OP_CLOSURE) printf("    ; %s", p->funcs[read_i32(p->code + off + 1)].name);
        if (op == OP_PUSH_CONST) {
            Value c = p->consts[read_i32(p->code + off + 1)];
            if (c.kind == K_FLOAT) printf("    ; %g", c.as.f);
            else if (c.kind == K_LONG) printf("    ; %lld", (long long)c.as.i64);
            else printf("    ; \"%.30s\"", str_chars(c.as.s));
        }
        printf("\n");
        off += 1 + 4 * n;
    }
}
