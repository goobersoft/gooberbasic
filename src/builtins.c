/* Core built-in commands and functions (console, strings, math, time).
 *
 * Each native gets its arguments already converted to the types listed in
 * its parameter spec ("i" Int, "f" Float, "s" String, "|" = the rest are
 * optional), so natives never need to check types themselves; they only need
 * to look at argc for optional arguments.
 */
#include "vm.h"
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>

#define ARG_I(n) (args[n].as.i)
#define ARG_F(n) (args[n].as.f)
#define ARG_S(n) (args[n].as.s)
#define RET_VOID return v_int(0)

static const double DEG = 3.14159265358979323846 / 180.0;

/* ---- console ---- */

static Value n_print(VM *vm, int argc, Value *args) {
    (void)vm;
    if (argc > 0) fwrite(str_chars(ARG_S(0)), 1, (size_t)str_len(ARG_S(0)), stdout);
    fputc('\n', stdout);
    fflush(stdout);
    RET_VOID;
}

static Value n_write(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc;
    fwrite(str_chars(ARG_S(0)), 1, (size_t)str_len(ARG_S(0)), stdout);
    fflush(stdout);
    RET_VOID;
}

static Value n_input(VM *vm, int argc, Value *args) {
    if (argc > 0) { fputs(str_chars(ARG_S(0)), stdout); fflush(stdout); }
    char buf[4096];
    if (!fgets(buf, sizeof buf, stdin)) return v_str(NULL);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    return v_str(str_new(vm, buf, (int)n));
}

static Value n_runtimeerror(VM *vm, int argc, Value *args) {
    (void)argc;
    vm_error(vm, "%s", str_chars(ARG_S(0)));
}

/* ---- time ---- */

static Value n_millisecs(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc; (void)args;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    return v_int((int32_t)(uint32_t)ms);
}

static Value n_delay(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc;
    int ms = ARG_I(0);
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    if (ms > 0) nanosleep(&ts, NULL);
    gfx_pump_events();
    RET_VOID;
}

static Value n_currentdate(VM *vm, int argc, Value *args) {
    (void)argc; (void)args;
    time_t t = time(NULL);
    char buf[64];
    strftime(buf, sizeof buf, "%d %b %Y", localtime(&t));
    return v_str(str_from_c(vm, buf));
}

static Value n_currenttime(VM *vm, int argc, Value *args) {
    (void)argc; (void)args;
    time_t t = time(NULL);
    char buf[64];
    strftime(buf, sizeof buf, "%H:%M:%S", localtime(&t));
    return v_str(str_from_c(vm, buf));
}

/* ---- random numbers (xorshift, so SeedRnd is reproducible everywhere) ---- */

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint64_t rng_next(void) {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return rng_state = x;
}
static double rng_unit(void) { return (double)(rng_next() >> 11) * (1.0 / 9007199254740992.0); }

static Value n_seedrnd(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc;
    rng_state = (uint64_t)(uint32_t)ARG_I(0) * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
    if (!rng_state) rng_state = 1;
    for (int i = 0; i < 8; i++) rng_next();
    RET_VOID;
}

/* Rnd(max) -> 0..max ; Rnd(min, max) -> min..max */
static Value n_rnd(VM *vm, int argc, Value *args) {
    (void)vm;
    double lo = 0, hi = 1;
    if (argc == 1) hi = ARG_F(0);
    else if (argc >= 2) { lo = ARG_F(0); hi = ARG_F(1); }
    return v_float(lo + (hi - lo) * rng_unit());
}

/* Rand(max) -> 1..max ; Rand(min, max) -> min..max (inclusive) */
static Value n_rand(VM *vm, int argc, Value *args) {
    (void)vm;
    int64_t lo = 1, hi = ARG_I(0);
    if (argc >= 2) { lo = ARG_I(0); hi = ARG_I(1); }
    if (hi < lo) { int64_t t = lo; lo = hi; hi = t; }
    uint64_t span = (uint64_t)(hi - lo) + 1;
    return v_int((int32_t)(lo + (int64_t)(rng_next() % span)));
}

/* ---- math (angles in degrees, like Blitz) ---- */

#define MATH1(fname, expr) \
    static Value fname(VM *vm, int argc, Value *args) { (void)vm; (void)argc; double x = ARG_F(0); return v_float(expr); }
MATH1(n_sin, sin(x * DEG))
MATH1(n_cos, cos(x * DEG))
MATH1(n_tan, tan(x * DEG))
MATH1(n_asin, asin(x) / DEG)
MATH1(n_acos, acos(x) / DEG)
MATH1(n_atan, atan(x) / DEG)
MATH1(n_sqr, sqrt(x))
MATH1(n_exp, exp(x))
MATH1(n_log, log(x))
MATH1(n_log10, log10(x))
MATH1(n_floor, floor(x))
MATH1(n_ceil, ceil(x))
MATH1(n_round, round(x))

static Value n_atan2(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc;
    return v_float(atan2(ARG_F(0), ARG_F(1)) / DEG);
}

static Value n_absi(VM *vm, int argc, Value *args) { (void)vm; (void)argc; int32_t i = ARG_I(0); return v_int(i < 0 ? (int32_t)(0u - (uint32_t)i) : i); }
static Value n_absf(VM *vm, int argc, Value *args) { (void)vm; (void)argc; return v_float(fabs(ARG_F(0))); }
static Value n_sgni(VM *vm, int argc, Value *args) { (void)vm; (void)argc; int32_t i = ARG_I(0); return v_int((i > 0) - (i < 0)); }
static Value n_sgnf(VM *vm, int argc, Value *args) { (void)vm; (void)argc; double f = ARG_F(0); return v_float((double)((f > 0) - (f < 0))); }
/* 64-bit versions (the compiler calls these directly for long values) */
static Value n_absl(VM *vm, int argc, Value *args) { (void)vm; (void)argc; int64_t l = args[0].as.i64; return v_long(l < 0 ? (int64_t)(0u - (uint64_t)l) : l); }
static Value n_sgnl(VM *vm, int argc, Value *args) { (void)vm; (void)argc; int64_t l = args[0].as.i64; return v_long((l > 0) - (l < 0)); }
static Value n_sgnu(VM *vm, int argc, Value *args) { (void)vm; (void)argc; return v_long(args[0].as.i64 != 0); }
static Value n_minl(VM *vm, int argc, Value *args) { (void)vm; (void)argc; return args[0].as.i64 < args[1].as.i64 ? args[0] : args[1]; }
static Value n_maxl(VM *vm, int argc, Value *args) { (void)vm; (void)argc; return args[0].as.i64 > args[1].as.i64 ? args[0] : args[1]; }
static Value n_minu(VM *vm, int argc, Value *args) { (void)vm; (void)argc; return (uint64_t)args[0].as.i64 < (uint64_t)args[1].as.i64 ? args[0] : args[1]; }
static Value n_maxu(VM *vm, int argc, Value *args) { (void)vm; (void)argc; return (uint64_t)args[0].as.i64 > (uint64_t)args[1].as.i64 ? args[0] : args[1]; }
static Value n_mini(VM *vm, int argc, Value *args) { (void)vm; (void)argc; return v_int(ARG_I(0) < ARG_I(1) ? ARG_I(0) : ARG_I(1)); }
static Value n_maxi(VM *vm, int argc, Value *args) { (void)vm; (void)argc; return v_int(ARG_I(0) > ARG_I(1) ? ARG_I(0) : ARG_I(1)); }
static Value n_minf(VM *vm, int argc, Value *args) { (void)vm; (void)argc; return v_float(ARG_F(0) < ARG_F(1) ? ARG_F(0) : ARG_F(1)); }
static Value n_maxf(VM *vm, int argc, Value *args) { (void)vm; (void)argc; return v_float(ARG_F(0) > ARG_F(1) ? ARG_F(0) : ARG_F(1)); }

static Value n_rectsoverlap(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc;
    double x1 = ARG_F(0), y1 = ARG_F(1), w1 = ARG_F(2), h1 = ARG_F(3);
    double x2 = ARG_F(4), y2 = ARG_F(5), w2 = ARG_F(6), h2 = ARG_F(7);
    return v_int(x1 < x2 + w2 && x2 < x1 + w1 && y1 < y2 + h2 && y2 < y1 + h1);
}

static Value n_distance(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc;
    double dx = ARG_F(2) - ARG_F(0), dy = ARG_F(3) - ARG_F(1);
    return v_float(sqrt(dx * dx + dy * dy));
}

/* ---- strings (positions are 1-based, like Blitz) ---- */

static Value n_len(VM *vm, int argc, Value *args) { (void)vm; (void)argc; return v_int(str_len(ARG_S(0))); }

static Value n_left(VM *vm, int argc, Value *args) {
    (void)argc;
    ObjString *s = ARG_S(0);
    int n = ARG_I(1), len = str_len(s);
    if (n >= len) return v_str(s);
    if (n <= 0) return v_str(NULL);
    return v_str(str_new(vm, s->chars, n));
}

static Value n_right(VM *vm, int argc, Value *args) {
    (void)argc;
    ObjString *s = ARG_S(0);
    int n = ARG_I(1), len = str_len(s);
    if (n >= len) return v_str(s);
    if (n <= 0) return v_str(NULL);
    return v_str(str_new(vm, s->chars + len - n, n));
}

static Value n_mid(VM *vm, int argc, Value *args) {
    ObjString *s = ARG_S(0);
    int len = str_len(s);
    int start = ARG_I(1) - 1;
    int count = argc > 2 ? ARG_I(2) : len;
    if (start < 0) { count += start; start = 0; }
    if (start >= len || count <= 0) return v_str(NULL);
    if (count > len - start) count = len - start;
    return v_str(str_new(vm, s->chars + start, count));
}

static Value n_upper(VM *vm, int argc, Value *args) {
    (void)argc;
    ObjString *s = ARG_S(0);
    int len = str_len(s);
    if (!len) return v_str(NULL);
    char *b = malloc((size_t)len);
    for (int i = 0; i < len; i++) b[i] = (char)toupper((unsigned char)s->chars[i]);
    ObjString *r = str_new(vm, b, len);
    free(b);
    return v_str(r);
}

static Value n_lower(VM *vm, int argc, Value *args) {
    (void)argc;
    ObjString *s = ARG_S(0);
    int len = str_len(s);
    if (!len) return v_str(NULL);
    char *b = malloc((size_t)len);
    for (int i = 0; i < len; i++) b[i] = (char)tolower((unsigned char)s->chars[i]);
    ObjString *r = str_new(vm, b, len);
    free(b);
    return v_str(r);
}

static Value n_trim(VM *vm, int argc, Value *args) {
    (void)argc;
    ObjString *s = ARG_S(0);
    int len = str_len(s), a = 0, b = len;
    while (a < b && isspace((unsigned char)s->chars[a])) a++;
    while (b > a && isspace((unsigned char)s->chars[b - 1])) b--;
    if (a == 0 && b == len) return v_str(s);
    return v_str(str_new(vm, s ? s->chars + a : "", b - a));
}

static int find_sub(ObjString *s, ObjString *sub, int from) {
    int ls = str_len(s), lsub = str_len(sub);
    if (lsub == 0) return from < ls ? from : -1;
    for (int i = from; i + lsub <= ls; i++)
        if (memcmp(s->chars + i, sub->chars, (size_t)lsub) == 0) return i;
    return -1;
}

static Value n_instr(VM *vm, int argc, Value *args) {
    (void)vm;
    int from = argc > 2 ? ARG_I(2) - 1 : 0;
    if (from < 0) from = 0;
    int r = find_sub(ARG_S(0), ARG_S(1), from);
    return v_int(r + 1);
}

static Value n_replace(VM *vm, int argc, Value *args) {
    (void)argc;
    ObjString *s = ARG_S(0), *from = ARG_S(1), *to = ARG_S(2);
    int lf = str_len(from);
    if (lf == 0 || find_sub(s, from, 0) < 0) return v_str(s);
    size_t cap = (size_t)str_len(s) + 16, n = 0;
    char *b = malloc(cap);
    int i = 0, ls = str_len(s), lt = str_len(to);
    for (;;) {
        int k = find_sub(s, from, i);
        int chunk = (k < 0 ? ls : k) - i;
        while (n + (size_t)chunk + (size_t)lt + 1 > cap) { cap *= 2; b = realloc(b, cap); }
        memcpy(b + n, s->chars + i, (size_t)chunk);
        n += (size_t)chunk;
        if (k < 0) break;
        memcpy(b + n, str_chars(to), (size_t)lt);
        n += (size_t)lt;
        i = k + lf;
    }
    ObjString *r = str_new(vm, b, (int)n);
    free(b);
    return v_str(r);
}

static Value n_chr(VM *vm, int argc, Value *args) {
    (void)argc;
    char c = (char)ARG_I(0);
    return v_str(str_new(vm, &c, 1));
}

static Value n_asc(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc;
    ObjString *s = ARG_S(0);
    return v_int(str_len(s) ? (unsigned char)s->chars[0] : -1);
}

static Value n_hex(VM *vm, int argc, Value *args) {
    (void)argc;
    char buf[16];
    snprintf(buf, sizeof buf, "%08X", (uint32_t)ARG_I(0));
    return v_str(str_from_c(vm, buf));
}

static Value n_bin(VM *vm, int argc, Value *args) {
    (void)argc;
    char buf[33];
    uint32_t v = (uint32_t)ARG_I(0);
    for (int i = 0; i < 32; i++) buf[i] = (v >> (31 - i)) & 1 ? '1' : '0';
    buf[32] = 0;
    return v_str(str_from_c(vm, buf));
}

static Value n_repeatstr(VM *vm, int argc, Value *args) {
    (void)argc;
    ObjString *s = ARG_S(0);
    int n = ARG_I(1), l = str_len(s);
    if (n <= 0 || l == 0) return v_str(NULL);
    if ((long long)n * l > 0x7fffffff) vm_error(vm, "repeat_string: result too long");
    char *b = malloc((size_t)n * (size_t)l);
    for (int i = 0; i < n; i++) memcpy(b + (size_t)i * (size_t)l, s->chars, (size_t)l);
    ObjString *r = str_new(vm, b, n * l);
    free(b);
    return v_str(r);
}

static Value n_lset(VM *vm, int argc, Value *args) {
    (void)argc;
    ObjString *s = ARG_S(0);
    int n = ARG_I(1), l = str_len(s);
    if (n <= 0) return v_str(NULL);
    char *b = malloc((size_t)n);
    for (int i = 0; i < n; i++) b[i] = i < l ? s->chars[i] : ' ';
    ObjString *r = str_new(vm, b, n);
    free(b);
    return v_str(r);
}

static Value n_rset(VM *vm, int argc, Value *args) {
    (void)argc;
    ObjString *s = ARG_S(0);
    int n = ARG_I(1), l = str_len(s);
    if (n <= 0) return v_str(NULL);
    char *b = malloc((size_t)n);
    for (int i = 0; i < n; i++) {
        int k = l - n + i;
        b[i] = k >= 0 ? s->chars[k] : ' ';
    }
    ObjString *r = str_new(vm, b, n);
    free(b);
    return v_str(r);
}

/* ---- typed lists ----
 * These are internal (__list_*): the compiler calls them from the list
 * intrinsics after checking that the value matches the list's element type,
 * which a native's parameter spec cannot express. */

static ObjList *need_list(VM *vm, Value v, const char *fn) {
    if (!v.as.l) vm_error(vm, "%s: the list is null", fn);
    return v.as.l;
}

/* Both values have the list's element kind (Int, Float, String or object). */
static bool values_equal(Value a, Value b) {
    switch (a.kind) {
        case K_INT: return a.as.i == b.as.i;
        case K_FLOAT: return a.as.f == b.as.f;
        case K_LONG: case K_ULONG: return a.as.i64 == b.as.i64;
        case K_STRING: return str_len(a.as.s) == str_len(b.as.s) &&
                              memcmp(str_chars(a.as.s), str_chars(b.as.s), (size_t)str_len(a.as.s)) == 0;
        default: return a.as.obj == b.as.obj;
    }
}

static ObjLink *find_link(ObjList *l, Value v) {
    for (ObjLink *k = l->head; k; k = k->next) if (values_equal(k->value, v)) return k;
    return NULL;
}

static Value n_list_new(VM *vm, int argc, Value *args) { (void)argc; (void)args; return v_list(list_new(vm)); }

static void add_common(VM *vm, Value *args, bool front, const char *fn) {
    ObjList *l = need_list(vm, args[0], fn);
    /* the empty string is also a NULL pointer, so check the kind too */
    if (args[1].kind == K_OBJ && !args[1].as.o) vm_error(vm, "%s: cannot add null to a list", fn);
    list_add(vm, l, args[1], front);
}
static Value n_list_add_last(VM *vm, int argc, Value *args) { (void)argc; add_common(vm, args, false, "list_add_last"); return v_int(0); }
static Value n_list_add_first(VM *vm, int argc, Value *args) { (void)argc; add_common(vm, args, true, "list_add_first"); return v_int(0); }

static Value n_list_remove(VM *vm, int argc, Value *args) {
    (void)argc;
    ObjLink *k = find_link(need_list(vm, args[0], "list_remove"), args[1]);
    if (k) list_unlink(k);
    return v_int(0);
}
static Value n_list_contains(VM *vm, int argc, Value *args) {
    (void)argc;
    return v_int(find_link(need_list(vm, args[0], "list_contains"), args[1]) != NULL);
}
static Value n_count_list(VM *vm, int argc, Value *args) { (void)argc; return v_int(need_list(vm, args[0], "count_list")->count); }
static Value n_list_is_empty(VM *vm, int argc, Value *args) { (void)argc; return v_int(need_list(vm, args[0], "list_is_empty")->count == 0); }
static Value n_clear_list(VM *vm, int argc, Value *args) { (void)argc; list_clear(need_list(vm, args[0], "clear_list")); return v_int(0); }

/* ---- garbage collector ---- */
static Value n_gccollect(VM *vm, int argc, Value *args) { (void)argc; (void)args; gc_collect(vm); return v_int(0); }
static Value n_gcmemalloced(VM *vm, int argc, Value *args) {
    (void)argc; (void)args;
    return v_int(vm->bytesAllocated > 0x7fffffff ? 0x7fffffff : (int32_t)vm->bytesAllocated);
}

NativeDef gb_core_natives[] = {
    {"__list_new", 'v', "", n_list_new},
    {"__list_add_first", 'v', "", n_list_add_first},
    {"__list_add_last", 'v', "", n_list_add_last},
    {"__list_remove", 'v', "", n_list_remove},
    {"__list_contains", 'i', "", n_list_contains},
    {"__count_list", 'i', "", n_count_list},
    {"__list_is_empty", 'i', "", n_list_is_empty},
    {"__clear_list", 'v', "", n_clear_list},
    {"gc_collect", 'v', "", n_gccollect},
    {"gc_mem_alloced", 'i', "", n_gcmemalloced},

    {"print", 'v', "|s", n_print},
    {"write", 'v', "s", n_write},
    {"input", 's', "|s", n_input},
    {"runtime_error", 'v', "s", n_runtimeerror},

    {"millisecs", 'i', "", n_millisecs},
    {"delay", 'v', "i", n_delay},
    {"current_date", 's', "", n_currentdate},
    {"current_time", 's', "", n_currenttime},

    {"seed_rnd", 'v', "i", n_seedrnd},
    {"rnd", 'f', "|ff", n_rnd},
    {"rand", 'i', "i|i", n_rand},

    {"sin", 'f', "f", n_sin},
    {"cos", 'f', "f", n_cos},
    {"tan", 'f', "f", n_tan},
    {"asin", 'f', "f", n_asin},
    {"acos", 'f', "f", n_acos},
    {"atan", 'f', "f", n_atan},
    {"atan2", 'f', "ff", n_atan2},
    {"sqr", 'f', "f", n_sqr},
    {"sqrt", 'f', "f", n_sqr},
    {"exp", 'f', "f", n_exp},
    {"log", 'f', "f", n_log},
    {"log10", 'f', "f", n_log10},
    {"floor", 'f', "f", n_floor},
    {"ceil", 'f', "f", n_ceil},
    {"round", 'f', "f", n_round},
    {"rects_overlap", 'i', "ffffffff", n_rectsoverlap},
    {"distance", 'f', "ffff", n_distance},

    {"left", 's', "si", n_left},
    {"right", 's', "si", n_right},
    {"mid", 's', "si|i", n_mid},
    {"upper", 's', "s", n_upper},
    {"lower", 's', "s", n_lower},
    {"trim", 's', "s", n_trim},
    {"instr", 'i', "ss|i", n_instr},
    {"replace", 's', "sss", n_replace},
    {"chr", 's', "i", n_chr},
    {"asc", 'i', "s", n_asc},
    {"hex", 's', "i", n_hex},
    {"bin", 's', "i", n_bin},
    {"repeat_string", 's', "si", n_repeatstr},
    {"lset", 's', "si", n_lset},
    {"rset", 's', "si", n_rset},

    /* used by compiler intrinsics */
    {"__len", 'i', "s", n_len},
    {"__absi", 'i', "i", n_absi},
    {"__absf", 'f', "f", n_absf},
    {"__sgni", 'i', "i", n_sgni},
    {"__sgnf", 'f', "f", n_sgnf},
    {"__mini", 'i', "ii", n_mini},
    {"__absl", 'i', "i", n_absl},
    {"__sgnl", 'i', "i", n_sgnl},
    {"__sgnu", 'i', "i", n_sgnu},
    {"__minl", 'i', "ii", n_minl},
    {"__maxl", 'i', "ii", n_maxl},
    {"__minu", 'i', "ii", n_minu},
    {"__maxu", 'i', "ii", n_maxu},
    {"__maxi", 'i', "ii", n_maxi},
    {"__minf", 'f', "ff", n_minf},
    {"__maxf", 'f', "ff", n_maxf},
    {NULL, 0, NULL, NULL},
};

/* ---- registry: core natives first, then graphics ---- */

static int ncore = -1, ngfx = 0;

static void count_natives(void) {
    if (ncore >= 0) return;
    ncore = 0;
    while (gb_core_natives[ncore].name) ncore++;
    while (gb_gfx_natives[ngfx].name) ngfx++;
}

int native_count(void) {
    count_natives();
    return ncore + ngfx;
}

const NativeDef *native_get(int idx) {
    count_natives();
    return idx < ncore ? &gb_core_natives[idx] : &gb_gfx_natives[idx - ncore];
}

static bool name_eq(const char *a, const char *lname) {
    for (; *a && *lname; a++, lname++)
        if (tolower((unsigned char)*a) != *lname) return false;
    return *a == 0 && *lname == 0;
}

int native_find(const char *lname) {
    count_natives();
    for (int i = 0; i < ncore; i++) if (name_eq(gb_core_natives[i].name, lname)) return i;
    for (int i = 0; i < ngfx; i++) if (name_eq(gb_gfx_natives[i].name, lname)) return ncore + i;
    return -1;
}
