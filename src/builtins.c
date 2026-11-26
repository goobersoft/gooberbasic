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

static Value n_error(VM *vm, int argc, Value *args) {
    (void)argc;
    vm_error(vm, "%s", str_chars(ARG_S(0)));
}

/* stop(): ends the program cleanly (exit code 0) from anywhere, inside a function too */
static Value n_stop(VM *vm, int argc, Value *args) {
    (void)argc; (void)args;
    vm_exit(vm, 0);
}

static Value n_assert(VM *vm, int argc, Value *args) {
    if (ARG_I(0)) RET_VOID;
    if (argc > 1) vm_error(vm, "assertion failed: %s", str_chars(ARG_S(1)));
    vm_error(vm, "assertion failed");
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
    net_poll();
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

/* ---- collision ----
 * A rect covers x .. x+w with its right and bottom edges left out, like the
 * pixels draw_rect fills: rects that only share an edge don't overlap. A
 * circle holds the points on its edge; circles that only touch don't overlap. */

static Value n_inrange(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc;
    double x = ARG_F(0);
    return v_int(x >= ARG_F(1) && x <= ARG_F(2));
}

static Value n_inrect(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc;
    double x = ARG_F(0), y = ARG_F(1), rx = ARG_F(2), ry = ARG_F(3);
    return v_int(x >= rx && x < rx + ARG_F(4) && y >= ry && y < ry + ARG_F(5));
}

static Value n_rectinrect(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc;
    double x1 = ARG_F(0), y1 = ARG_F(1), w1 = ARG_F(2), h1 = ARG_F(3);
    double x2 = ARG_F(4), y2 = ARG_F(5), w2 = ARG_F(6), h2 = ARG_F(7);
    return v_int(x1 < x2 + w2 && x2 < x1 + w1 && y1 < y2 + h2 && y2 < y1 + h1);
}

static Value n_incircle(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc;
    double dx = ARG_F(0) - ARG_F(2), dy = ARG_F(1) - ARG_F(3), r = ARG_F(4);
    return v_int(dx * dx + dy * dy <= r * r);
}

static Value n_circleincircle(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc;
    double dx = ARG_F(0) - ARG_F(3), dy = ARG_F(1) - ARG_F(4), r = ARG_F(2) + ARG_F(5);
    return v_int(dx * dx + dy * dy < r * r);
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

/* The first link holding `v` and its position (counted from 1), or NULL and 0. */
static ObjLink *find_link(ObjList *l, Value v, int *pos) {
    int i = 1;
    for (ObjLink *k = l->head; k; k = k->next, i++)
        if (values_equal(k->value, v)) { *pos = i; return k; }
    *pos = 0;
    return NULL;
}

static ObjList *need_items(VM *vm, Value v, const char *fn) {
    ObjList *l = need_list(vm, v, fn);
    if (!l->head) vm_error(vm, "%s: the list is empty", fn);
    return l;
}

static void no_null(VM *vm, Value v, const char *fn) {
    /* the empty string is also a NULL pointer, so check the kind too */
    if (v.kind == K_OBJ && !v.as.o) vm_error(vm, "%s: cannot add null to a list", fn);
}

static Value n_list_new(VM *vm, int argc, Value *args) { (void)argc; (void)args; return v_list(list_new(vm)); }

static void add_common(VM *vm, Value *args, bool front, const char *fn) {
    ObjList *l = need_list(vm, args[0], fn);
    no_null(vm, args[1], fn);
    list_add(vm, l, args[1], front);
}
static Value n_add_last(VM *vm, int argc, Value *args) { (void)argc; add_common(vm, args, false, "add_last"); RET_VOID; }
static Value n_add_first(VM *vm, int argc, Value *args) { (void)argc; add_common(vm, args, true, "add_first"); RET_VOID; }

/* insert(list, n, value): value becomes item n; below 1 goes first, past the end goes last */
static Value n_insert(VM *vm, int argc, Value *args) {
    (void)argc;
    ObjList *l = need_list(vm, args[0], "insert");
    no_null(vm, args[2], "insert");
    int n = ARG_I(1);
    if (n <= 1 || !l->head) { list_add(vm, l, args[2], true); RET_VOID; }
    if (n > l->count) { list_add(vm, l, args[2], false); RET_VOID; }
    ObjLink *at = l->head;
    for (int i = 1; i < n; i++) at = at->next;
    list_insert_before(vm, l, at, args[2]);
    RET_VOID;
}

/* remove(list, value): takes out the first matching item; its position, or 0 if none matched */
static Value n_remove(VM *vm, int argc, Value *args) {
    (void)argc;
    int pos;
    ObjLink *k = find_link(need_list(vm, args[0], "remove"), args[1], &pos);
    if (k) list_unlink(k);
    return v_int(pos);
}
/* contains(list, value): the position of the first matching item, or 0 */
static Value n_contains(VM *vm, int argc, Value *args) {
    (void)argc;
    int pos;
    find_link(need_list(vm, args[0], "contains"), args[1], &pos);
    return v_int(pos);
}
/* count(list): every item; count(list, value): the items that match value */
static Value n_count(VM *vm, int argc, Value *args) {
    ObjList *l = need_list(vm, args[0], "count");
    if (argc < 2) return v_int(l->count);
    int n = 0;
    for (ObjLink *k = l->head; k; k = k->next) n += values_equal(k->value, args[1]);
    return v_int(n);
}
static Value n_is_empty(VM *vm, int argc, Value *args) { (void)argc; return v_int(need_list(vm, args[0], "is_empty")->count == 0); }
static Value n_clear(VM *vm, int argc, Value *args) { (void)argc; list_clear(need_list(vm, args[0], "clear")); RET_VOID; }

static Value n_first(VM *vm, int argc, Value *args) { (void)argc; return need_items(vm, args[0], "first")->head->value; }
static Value n_last(VM *vm, int argc, Value *args) { (void)argc; return need_items(vm, args[0], "last")->tail->value; }
static Value n_remove_first(VM *vm, int argc, Value *args) {
    (void)argc;
    ObjLink *k = need_items(vm, args[0], "remove_first")->head;
    Value v = k->value;
    list_unlink(k);
    return v;
}
static Value n_remove_last(VM *vm, int argc, Value *args) {
    (void)argc;
    ObjLink *k = need_items(vm, args[0], "remove_last")->tail;
    Value v = k->value;
    list_unlink(k);
    return v;
}

/* ---- garbage collector ---- */
static Value n_gccollect(VM *vm, int argc, Value *args) { (void)argc; (void)args; gc_collect(vm); return v_int(0); }
static Value n_gcmemalloced(VM *vm, int argc, Value *args) {
    (void)argc; (void)args;
    return v_int(vm->bytesAllocated > 0x7fffffff ? 0x7fffffff : (int32_t)vm->bytesAllocated);
}

/* ---- defdata blocks ---- */

static int data_block_arg(VM *vm, const char *fn, int b) {
    if (b < 0 || b >= vm->prog->ndataBlocks)
        vm_error(vm, "%s: there is no data block %d (the program has %d)", fn, b, vm->prog->ndataBlocks);
    return b;
}

/* restore() goes back to the first item of the first block; restore(NAME) to NAME's first item. */
static Value n_restore(VM *vm, int argc, Value *args) {
    vm->dataPos = argc > 0 ? vm->prog->dataStart[data_block_arg(vm, "restore", ARG_I(0))] : 0;
    RET_VOID;
}

static Value n_data_count(VM *vm, int argc, Value *args) {
    (void)argc;
    int b = data_block_arg(vm, "data_count", ARG_I(0));
    return v_int(vm->prog->dataStart[b + 1] - vm->prog->dataStart[b]);
}

/* Reading runs on past the end of a block into the next one, like BASIC's READ. */
static Value read_data(VM *vm, const char *fn, uint8_t to) {
    if (vm->dataPos >= vm->prog->ndata)
        vm_error(vm, "%s: out of data (every defdata item has been read; use restore to read again)", fn);
    return vm_convert(vm, vm->prog->data[vm->dataPos++], to);
}
static Value n_read_int(VM *vm, int argc, Value *args) { (void)argc; (void)args; return read_data(vm, "read_int", K_INT); }
static Value n_read_float(VM *vm, int argc, Value *args) { (void)argc; (void)args; return read_data(vm, "read_float", K_FLOAT); }
static Value n_read_string(VM *vm, int argc, Value *args) { (void)argc; (void)args; return read_data(vm, "read_string", K_STRING); }

NativeDef gb_core_natives[] = {
    {"__list_new", 'v', "", n_list_new},
    {"__add_first", 'v', "", n_add_first},
    {"__add_last", 'v', "", n_add_last},
    {"__remove", 'i', "", n_remove},
    {"__contains", 'i', "", n_contains},
    {"__insert", 'v', "", n_insert},
    {"__count", 'i', "", n_count},
    {"__is_empty", 'i', "", n_is_empty},
    {"__clear", 'v', "", n_clear},
    /* these return an element; the compiler knows its type, the 'v' here is never read */
    {"__first", 'v', "", n_first},
    {"__last", 'v', "", n_last},
    {"__remove_first", 'v', "", n_remove_first},
    {"__remove_last", 'v', "", n_remove_last},
    {"gc_collect", 'v', "", n_gccollect},
    {"gc_mem_alloced", 'i', "", n_gcmemalloced},

    {"print", 'v', "|s", n_print},
    {"write", 'v', "s", n_write},
    {"input", 's', "|s", n_input},
    {"error", 'v', "s", n_error},
    {"stop", 'v', "", n_stop},
    {"assert", 'v', "i|s", n_assert},

    {"restore", 'v', "|i", n_restore},
    {"read_int", 'i', "", n_read_int},
    {"read_float", 'f', "", n_read_float},
    {"read_string", 's', "", n_read_string},
    {"data_count", 'i', "i", n_data_count},

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
    {"in_range", 'i', "fff", n_inrange},
    {"in_rect", 'i', "ffffff", n_inrect},
    {"rect_in_rect", 'i', "ffffffff", n_rectinrect},
    {"in_circle", 'i', "fffff", n_incircle},
    {"circle_in_circle", 'i', "ffffff", n_circleincircle},
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

/* ---- registry: core natives first, then graphics, then networking (net.c) ---- */

static int ncore = -1, ngfx = 0, nnet = 0;

static void count_natives(void) {
    if (ncore >= 0) return;
    ncore = 0;
    while (gb_core_natives[ncore].name) ncore++;
    while (gb_gfx_natives[ngfx].name) ngfx++;
    while (gb_net_natives[nnet].name) nnet++;
}

int native_count(void) {
    count_natives();
    return ncore + ngfx + nnet;
}

const NativeDef *native_get(int idx) {
    count_natives();
    if (idx < ncore) return &gb_core_natives[idx];
    if (idx < ncore + ngfx) return &gb_gfx_natives[idx - ncore];
    return &gb_net_natives[idx - ncore - ngfx];
}

static bool name_eq(const char *a, const char *lname) {
    for (; *a && *lname; a++, lname++)
        if (tolower((unsigned char)*a) != *lname) return false;
    return *a == 0 && *lname == 0;
}

/* the graphics table is searched first, so a platform can replace a core command
 * (GooberEngine's print draws on its screen as well as writing to stdout) */
int native_find(const char *lname) {
    count_natives();
    for (int i = 0; i < ngfx; i++) if (name_eq(gb_gfx_natives[i].name, lname)) return ncore + i;
    for (int i = 0; i < ncore; i++) if (name_eq(gb_core_natives[i].name, lname)) return i;
    for (int i = 0; i < nnet; i++) if (name_eq(gb_net_natives[i].name, lname)) return ncore + ngfx + i;
    return -1;
}
