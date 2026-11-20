/* Heap objects and the mark-and-sweep garbage collector.
 *
 * Like BlitzMax, objects have no manual lifetime: anything (string, Type
 * instance, array, TList) that can no longer be reached from the roots is
 * freed. Roots: the value stack (locals and temporaries), globals and the
 * constant pool.
 */
#include "vm.h"
#include <ctype.h>
#include <math.h>

VM *gb_vm = NULL;

void *gb_alloc(size_t size) {
    void *p = calloc(1, size ? size : 1);
    if (!p) { fprintf(stderr, "fatal: out of memory\n"); exit(1); }
    return p;
}

void *gb_realloc(void *p, size_t size) {
    void *q = realloc(p, size ? size : 1);
    if (!q) { fprintf(stderr, "fatal: out of memory\n"); exit(1); }
    return q;
}

char *gb_strndup(const char *s, size_t n) {
    char *d = gb_alloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

void (*gb_diag_hook)(const char *text) = NULL;
jmp_buf *gb_error_jmp = NULL;

void gb_vdiag(const char *fmt, va_list ap) {
    if (!gb_diag_hook) { vfprintf(stderr, fmt, ap); return; }
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    gb_diag_hook(buf);
}

void gb_diag(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    gb_vdiag(fmt, ap);
    va_end(ap);
}

void gb_fatal(void) {
    if (gb_error_jmp) longjmp(*gb_error_jmp, 1);
    exit(1);
}

char *gb_strdup(const char *s) { return gb_strndup(s, strlen(s)); }

char *gb_lower(const char *s) {
    char *d = gb_strdup(s);
    for (char *p = d; *p; p++) *p = (char)tolower((unsigned char)*p);
    return d;
}

void gb_format_float(double f, char *buf, size_t n) {
    if (isnan(f)) { snprintf(buf, n, "NaN"); return; }
    if (isinf(f)) { snprintf(buf, n, f > 0 ? "Infinity" : "-Infinity"); return; }
    double a = f < 0 ? -f : f;
    if (a != 0 && (a >= 1e15 || a < 1e-5)) {
        snprintf(buf, n, "%g", f);
        return;
    }
    snprintf(buf, n, "%.6f", f);
    /* trim trailing zeros, keep one decimal */
    char *dot = strchr(buf, '.');
    if (dot) {
        char *end = buf + strlen(buf) - 1;
        while (end > dot + 1 && *end == '0') *end-- = 0;
    }
    if (strcmp(buf, "-0.0") == 0) strcpy(buf, "0.0");
}

Value default_value(uint8_t kind) {
    Value v;
    memset(&v, 0, sizeof v);
    v.kind = kind;
    return v;
}

static void *gc_obj(VM *vm, size_t size, ObjKind okind) {
    if (vm->bytesAllocated > vm->nextGC || vm->gcStress) gc_collect(vm);
    Obj *o = gb_alloc(size);
    o->okind = (uint8_t)okind;
    o->next = vm->objects;
    vm->objects = o;
    vm->bytesAllocated += size;
    return o;
}

ObjString *str_new(VM *vm, const char *s, int len) {
    if (len <= 0) return NULL; /* the empty string is represented by NULL */
    ObjString *str = gc_obj(vm, sizeof(ObjString) + (size_t)len + 1, O_STRING);
    str->len = len;
    memcpy(str->chars, s, (size_t)len);
    str->chars[len] = 0;
    return str;
}

ObjString *str_from_c(VM *vm, const char *s) { return str_new(vm, s, (int)strlen(s)); }

ObjInst *inst_new(VM *vm, int tid) {
    TypeDef *td = &vm->prog->types[tid];
    ObjInst *o = gc_obj(vm, sizeof(ObjInst) + sizeof(Value) * (size_t)td->nfields, O_INST);
    o->tid = tid;
    o->nfields = td->nfields;
    for (int i = 0; i < td->nfields; i++) {
        VType ft = td->fieldTypes[i];
        o->fields[i] = default_value(ft.dims ? K_ARRAY : ft.base);
    }
    return o;
}

ObjArray *array_new(VM *vm, uint8_t elemKind, int ndims, const int *dims) {
    long long count = 1;
    for (int i = 0; i < ndims; i++) {
        if (dims[i] < 0) vm_error(vm, "Negative array size %d", dims[i]);
        count *= dims[i];
        if (count > 0x7fffffff / (long long)sizeof(Value)) vm_error(vm, "Array too large");
    }
    ObjArray *a = gc_obj(vm, sizeof(ObjArray) + sizeof(Value) * (size_t)count, O_ARRAY);
    a->elemKind = elemKind;
    a->ndims = (uint8_t)ndims;
    for (int i = 0; i < ndims; i++) a->dims[i] = dims[i];
    a->count = (int)count;
    Value d = default_value(elemKind);
    for (long long i = 0; i < count; i++) a->data[i] = d;
    return a;
}

ObjList *list_new(VM *vm) {
    return gc_obj(vm, sizeof(ObjList), O_LIST);
}

/* `v` must be reachable from the stack while this allocates. */
void list_add(VM *vm, ObjList *l, Value v, bool atFront) {
    ObjLink *k = gc_obj(vm, sizeof(ObjLink), O_LINK);
    k->list = l;
    k->value = v;
    if (atFront) {
        k->next = l->head;
        if (l->head) l->head->prev = k; else l->tail = k;
        l->head = k;
    } else {
        k->prev = l->tail;
        if (l->tail) l->tail->next = k; else l->head = k;
        l->tail = k;
    }
    l->count++;
}

void list_unlink(ObjLink *k) {
    ObjList *l = k->list;
    if (!l) return;
    if (k->prev) k->prev->next = k->next; else l->head = k->next;
    if (k->next) k->next->prev = k->prev; else l->tail = k->prev;
    l->count--;
    k->list = NULL;
    k->prev = NULL;
    /* `next` is kept on purpose: see ObjLink in common.h */
}

void list_clear(ObjList *l) {
    while (l->head) list_unlink(l->head);
}

/* ---- collector ---- */

static void mark_obj(VM *vm, Obj *o) {
    if (!o || o->marked) return;
    o->marked = 1;
    if (o->okind == O_STRING) return; /* strings hold no references */
    if (vm->ngray == vm->capGray) {
        vm->capGray = vm->capGray ? vm->capGray * 2 : 256;
        vm->gray = gb_realloc(vm->gray, sizeof(Obj *) * (size_t)vm->capGray);
    }
    vm->gray[vm->ngray++] = o;
}

static void mark_value(VM *vm, Value v) {
    if (kind_is_ref(v.kind)) mark_obj(vm, v.as.obj);
}

static void trace(VM *vm) {
    while (vm->ngray > 0) {
        Obj *o = vm->gray[--vm->ngray];
        if (o->okind == O_INST) {
            ObjInst *in = (ObjInst *)o;
            for (int i = 0; i < in->nfields; i++) mark_value(vm, in->fields[i]);
        } else if (o->okind == O_ARRAY) {
            ObjArray *a = (ObjArray *)o;
            if (kind_is_ref(a->elemKind))
                for (int i = 0; i < a->count; i++) mark_value(vm, a->data[i]);
        } else if (o->okind == O_LIST) {
            mark_obj(vm, (Obj *)((ObjList *)o)->head);
        } else if (o->okind == O_LINK) {
            ObjLink *k = (ObjLink *)o;
            mark_value(vm, k->value);
            mark_obj(vm, (Obj *)k->next);
            mark_obj(vm, (Obj *)k->list);
        }
    }
}

void gc_collect(VM *vm) {
    for (Value *v = vm->stack; v < vm->sp; v++) mark_value(vm, *v);
    for (int i = 0; i < vm->prog->nglobals; i++) mark_value(vm, vm->globals[i]);
    for (int i = 0; i < vm->prog->nconsts; i++) mark_value(vm, vm->prog->consts[i]);
    trace(vm);

    Obj **link = &vm->objects;
    size_t live = 0;
    while (*link) {
        Obj *o = *link;
        if (o->marked) {
            o->marked = 0;
            switch (o->okind) {
                case O_STRING: live += sizeof(ObjString) + (size_t)((ObjString *)o)->len + 1; break;
                case O_INST: live += sizeof(ObjInst) + sizeof(Value) * (size_t)((ObjInst *)o)->nfields; break;
                case O_ARRAY: live += sizeof(ObjArray) + sizeof(Value) * (size_t)((ObjArray *)o)->count; break;
                case O_LIST: live += sizeof(ObjList); break;
                case O_LINK: live += sizeof(ObjLink); break;
            }
            link = &o->next;
        } else {
            *link = o->next;
            free(o);
        }
    }
    vm->bytesAllocated = live;
    vm->nextGC = live * 2 < (8u << 20) ? (8u << 20) : live * 2;
}

void gc_free_all(VM *vm) {
    Obj *o = vm->objects;
    while (o) {
        Obj *n = o->next;
        free(o);
        o = n;
    }
    vm->objects = NULL;
    free(vm->gray);
    vm->gray = NULL;
}
