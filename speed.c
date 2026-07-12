/* speed.c -- implementation of UD's performance core.
 *
 * See speed.h for the design notes. The short version: primitives are inline
 * in the Value, objects come from a bump arena that is freed once at exit,
 * strings are interned so equality is a pointer compare, and the hash map uses
 * open addressing with cached hashes.
 */
#include "speed.h"
#include "errors.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================== */
/* Arena                                                              */
/* ================================================================== */

struct ud_arena ud_heap;

static struct ud_arena_block *arena_new_block(size_t cap) {
    struct ud_arena_block *b = (struct ud_arena_block *)malloc(sizeof(*b) + cap);
    if (!b) {
        fprintf(stderr, "UD Error 99: Internal error\n  Why: out of memory\n");
        exit(70);
    }
    b->next = NULL;
    b->used = 0;
    b->cap  = cap;
    return b;
}

void ud_arena_init(struct ud_arena *a, size_t block_size) {
    a->block_size = block_size ? block_size : (size_t)(256 * 1024);
    a->total = 0;
    a->head = arena_new_block(a->block_size);
}

void *ud_arena_alloc(struct ud_arena *a, size_t n) {
    /* 16-byte align so doubles/pointers/Values are always aligned. */
    n = (n + 15u) & ~(size_t)15u;
    struct ud_arena_block *b = a->head;
    if (b->used + n > b->cap) {
        /* Oversized request gets its own block; otherwise start a fresh one. */
        size_t cap = n > a->block_size ? n : a->block_size;
        struct ud_arena_block *nb = arena_new_block(cap);
        nb->next = a->head;
        a->head = nb;
        b = nb;
    }
    void *p = b->data + b->used;
    b->used += n;
    a->total += n;
    return p;
}

void ud_arena_free(struct ud_arena *a) {
    struct ud_arena_block *b = a->head;
    while (b) {
        struct ud_arena_block *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
    a->total = 0;
}

/* ================================================================== */
/* String interning                                                   */
/* ================================================================== */

/* FNV-1a: fast, decent distribution, tiny code. */
uint32_t ud_hash_bytes(const char *s, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

/* Open-addressing set of interned strings. */
static struct ud_string **intern_slots = NULL;
static int intern_cap   = 0;
static int intern_count = 0;

static void intern_grow(void) {
    int newcap = intern_cap < 256 ? 256 : intern_cap * 2;
    struct ud_string **slots =
        (struct ud_string **)calloc((size_t)newcap, sizeof(*slots));
    if (!slots) { fprintf(stderr, "UD Error 99: out of memory\n"); exit(70); }
    for (int i = 0; i < intern_cap; i++) {
        struct ud_string *s = intern_slots[i];
        if (!s) continue;
        uint32_t idx = s->hash & (uint32_t)(newcap - 1);
        while (slots[idx]) idx = (idx + 1) & (uint32_t)(newcap - 1);
        slots[idx] = s;
    }
    free(intern_slots);
    intern_slots = slots;
    intern_cap = newcap;
}

struct ud_string *ud_str_intern(const char *chars, int len) {
    if (intern_cap == 0) intern_grow();
    if (intern_count + 1 > intern_cap * 3 / 4) intern_grow();

    uint32_t hash = ud_hash_bytes(chars, len);
    uint32_t idx = hash & (uint32_t)(intern_cap - 1);
    for (;;) {
        struct ud_string *s = intern_slots[idx];
        if (!s) break;
        if (s->hash == hash && s->length == len &&
            memcmp(s->chars, chars, (size_t)len) == 0)
            return s; /* already interned */
        idx = (idx + 1) & (uint32_t)(intern_cap - 1);
    }

    struct ud_string *s =
        (struct ud_string *)ud_alloc(sizeof(struct ud_string) + (size_t)len + 1);
    s->obj.otype = OBJ_STRING;
    s->length = len;
    s->hash = hash;
    memcpy(s->chars, chars, (size_t)len);
    s->chars[len] = '\0';

    intern_slots[idx] = s;
    intern_count++;
    return s;
}

struct ud_string *ud_str_from_cstr(const char *s) {
    return ud_str_intern(s, (int)strlen(s));
}

struct ud_string *ud_str_concat(struct ud_string *a, struct ud_string *b) {
    int len = a->length + b->length;
    /* Build on a small stack buffer when possible, else a temp arena chunk. */
    char *buf = (char *)ud_alloc((size_t)len + 1);
    memcpy(buf, a->chars, (size_t)a->length);
    memcpy(buf + a->length, b->chars, (size_t)b->length);
    buf[len] = '\0';
    return ud_str_intern(buf, len);
}

/* ================================================================== */
/* Hash map: interned-string key -> Value                             */
/* ================================================================== */

#define MAP_LOAD_NUM 3
#define MAP_LOAD_DEN 4

void ud_map_init(struct ud_map *m) {
    m->count = 0;
    m->cap = 0;
    m->entries = NULL;
}

static struct ud_map_entry *map_find(struct ud_map_entry *entries, int cap,
                                     struct ud_string *key) {
    uint32_t idx = key->hash & (uint32_t)(cap - 1);
    for (;;) {
        struct ud_map_entry *e = &entries[idx];
        if (e->key == NULL || e->key == key) return e;
        idx = (idx + 1) & (uint32_t)(cap - 1);
    }
}

static void map_grow(struct ud_map *m) {
    int newcap = m->cap < 16 ? 16 : m->cap * 2;
    struct ud_map_entry *entries =
        (struct ud_map_entry *)calloc((size_t)newcap, sizeof(*entries));
    if (!entries) { fprintf(stderr, "UD Error 99: out of memory\n"); exit(70); }
    for (int i = 0; i < m->cap; i++) {
        struct ud_map_entry *e = &m->entries[i];
        if (!e->key) continue;
        struct ud_map_entry *dst = map_find(entries, newcap, e->key);
        dst->key = e->key;
        dst->val = e->val;
    }
    free(m->entries);
    m->entries = entries;
    m->cap = newcap;
}

int ud_map_get(struct ud_map *m, struct ud_string *key, struct ud_value *out) {
    if (m->cap == 0) return 0;
    struct ud_map_entry *e = map_find(m->entries, m->cap, key);
    if (e->key == NULL) return 0;
    if (out) *out = e->val;
    return 1;
}

int ud_map_has(struct ud_map *m, struct ud_string *key) {
    return ud_map_get(m, key, NULL);
}

void ud_map_set(struct ud_map *m, struct ud_string *key, struct ud_value val) {
    if (m->cap == 0 || m->count + 1 > m->cap * MAP_LOAD_NUM / MAP_LOAD_DEN)
        map_grow(m);
    struct ud_map_entry *e = map_find(m->entries, m->cap, key);
    if (e->key == NULL) m->count++;
    e->key = key;
    e->val = val;
}

/* ================================================================== */
/* Object constructors                                                */
/* ================================================================== */

struct ud_array *ud_array_new(int cap) {
    if (cap < 4) cap = 4;
    struct ud_array *a = (struct ud_array *)ud_alloc(sizeof(struct ud_array));
    a->obj.otype = OBJ_ARRAY;
    a->length = 0;
    a->cap = cap;
    a->items = (struct ud_value *)ud_alloc(sizeof(struct ud_value) * (size_t)cap);
    return a;
}

void ud_array_push(struct ud_array *a, struct ud_value v) {
    if (a->length + 1 > a->cap) {
        int newcap = a->cap * 2;
        struct ud_value *items =
            (struct ud_value *)ud_alloc(sizeof(struct ud_value) * (size_t)newcap);
        memcpy(items, a->items, sizeof(struct ud_value) * (size_t)a->length);
        a->items = items; /* old buffer reclaimed when the arena is freed */
        a->cap = newcap;
    }
    a->items[a->length++] = v;
}

struct ud_function *ud_function_new(struct ud_string *name) {
    struct ud_function *f =
        (struct ud_function *)ud_alloc(sizeof(struct ud_function));
    f->obj.otype = OBJ_FUNCTION;
    f->name = name;
    f->arity = 0;
    f->param_types = NULL;
    f->return_type = 0xFF;
    f->code = NULL;
    f->code_len = 0;
    f->code_cap = 0;
    f->consts = NULL;
    f->const_count = 0;
    f->const_cap = 0;
    f->lines = NULL;
    f->num_slots = 0;
    return f;
}

struct ud_native *ud_native_new(const char *name, int arity,
                                struct ud_value (*fn)(int, struct ud_value *, int)) {
    struct ud_native *n = (struct ud_native *)ud_alloc(sizeof(struct ud_native));
    n->obj.otype = OBJ_NATIVE;
    n->name = name;
    n->arity = arity;
    n->fn = fn;
    return n;
}

struct ud_structdef *ud_structdef_new(struct ud_string *name, int field_count) {
    struct ud_structdef *d =
        (struct ud_structdef *)ud_alloc(sizeof(struct ud_structdef));
    d->obj.otype = OBJ_STRUCTDEF;
    d->name = name;
    d->field_count = field_count;
    d->field_names = (struct ud_string **)ud_alloc(
        sizeof(struct ud_string *) * (size_t)(field_count ? field_count : 1));
    d->field_types = (uint8_t *)ud_alloc((size_t)(field_count ? field_count : 1));
    return d;
}

struct ud_struct *ud_struct_new(struct ud_structdef *def) {
    struct ud_struct *s = (struct ud_struct *)ud_alloc(sizeof(struct ud_struct));
    s->obj.otype = OBJ_STRUCT;
    s->def = def;
    int n = def->field_count ? def->field_count : 1;
    s->fields = (struct ud_value *)ud_alloc(sizeof(struct ud_value) * (size_t)n);
    for (int i = 0; i < def->field_count; i++) s->fields[i] = ud_nil();
    return s;
}

/* ================================================================== */
/* Heap lifecycle                                                     */
/* ================================================================== */

void ud_heap_init(void) {
    ud_arena_init(&ud_heap, 256 * 1024);
}

void ud_heap_shutdown(void) {
    ud_arena_free(&ud_heap);
    free(intern_slots);
    intern_slots = NULL;
    intern_cap = intern_count = 0;
}

/* ================================================================== */
/* VM value stack                                                     */
/* ================================================================== */

void ud_stack_init(struct ud_stack *s, int cap) {
    s->base = (struct ud_value *)malloc(sizeof(struct ud_value) * (size_t)cap);
    if (!s->base) { fprintf(stderr, "UD Error 99: out of memory\n"); exit(70); }
    s->top = s->base;
    s->cap = cap;
}

/* ================================================================== */
/* Value helpers                                                      */
/* ================================================================== */

const char *ud_type_name(enum ud_type t) {
    switch (t) {
        case UD_NIL:   return "nil";
        case UD_BOOL:  return "bool";
        case UD_INT:   return "int";
        case UD_FLOAT: return "float";
        case UD_OBJ:   return "object";
        default:       return "?";
    }
}

int ud_value_truthy(struct ud_value v) {
    /* Lua/Python blend: nil and false are falsey; 0 and "" are truthy like Lua. */
    switch (v.type) {
        case UD_NIL:  return 0;
        case UD_BOOL: return v.as.b;
        default:      return 1;
    }
}

int ud_value_equal(struct ud_value a, struct ud_value b) {
    if (a.type != b.type) {
        /* int and float compare by numeric value across types. */
        if (UD_IS_NUM(a) && UD_IS_NUM(b))
            return ud_as_number(a) == ud_as_number(b);
        return 0;
    }
    switch (a.type) {
        case UD_NIL:   return 1;
        case UD_BOOL:  return a.as.b == b.as.b;
        case UD_INT:   return a.as.i == b.as.i;
        case UD_FLOAT: return a.as.d == b.as.d;
        case UD_OBJ:
            if (a.as.o->otype == OBJ_STRING && b.as.o->otype == OBJ_STRING)
                return a.as.o == b.as.o; /* interned: pointer identity */
            return a.as.o == b.as.o;
        default: return 0;
    }
}

/* Render a double the way UD prints floats: always with a decimal point so a
 * float never looks like an int. */
static void format_float(char *buf, size_t n, double d) {
    snprintf(buf, n, "%.14g", d);
    /* Ensure it reads as a float (has '.', 'e', 'n' for nan, or 'i' for inf). */
    for (const char *p = buf; *p; p++) {
        if (*p == '.' || *p == 'e' || *p == 'E' || *p == 'n' || *p == 'i')
            return;
    }
    size_t len = strlen(buf);
    if (len + 2 < n) { buf[len] = '.'; buf[len+1] = '0'; buf[len+2] = '\0'; }
}

struct ud_string *ud_value_to_string(struct ud_value v) {
    char buf[64];
    switch (v.type) {
        case UD_NIL:  return ud_str_intern("nil", 3);
        case UD_BOOL: return v.as.b ? ud_str_intern("true", 4)
                                    : ud_str_intern("false", 5);
        case UD_INT:
            snprintf(buf, sizeof(buf), "%lld", v.as.i);
            return ud_str_from_cstr(buf);
        case UD_FLOAT:
            format_float(buf, sizeof(buf), v.as.d);
            return ud_str_from_cstr(buf);
        case UD_OBJ: break;
    }
    switch (v.as.o->otype) {
        case OBJ_STRING:
            return UD_AS_STRING(v);
        case OBJ_FUNCTION: {
            struct ud_function *f = UD_AS_FUNCTION(v);
            char b2[128];
            snprintf(b2, sizeof(b2), "<function %s>",
                     f->name ? f->name->chars : "?");
            return ud_str_from_cstr(b2);
        }
        case OBJ_NATIVE:
            return ud_str_from_cstr("<builtin>");
        case OBJ_STRUCTDEF: {
            struct ud_structdef *d = UD_AS_STRUCTDEF(v);
            char b2[128];
            snprintf(b2, sizeof(b2), "<struct %s>", d->name->chars);
            return ud_str_from_cstr(b2);
        }
        case OBJ_ARRAY: {
            struct ud_array *a = UD_AS_ARRAY(v);
            /* "[a, b, c]" -- build up, interning the final result. */
            struct ud_string *out = ud_str_intern("[", 1);
            for (int i = 0; i < a->length; i++) {
                if (i) out = ud_str_concat(out, ud_str_intern(", ", 2));
                struct ud_value e = a->items[i];
                struct ud_string *es;
                if (UD_IS_STRING(e)) {
                    /* quote nested strings for readability */
                    struct ud_string *q = ud_str_intern("\"", 1);
                    es = ud_str_concat(ud_str_concat(q, UD_AS_STRING(e)), q);
                } else {
                    es = ud_value_to_string(e);
                }
                out = ud_str_concat(out, es);
            }
            return ud_str_concat(out, ud_str_intern("]", 1));
        }
        case OBJ_STRUCT: {
            struct ud_struct *s = UD_AS_STRUCT(v);
            struct ud_string *out = ud_str_concat(s->def->name,
                                                  ud_str_intern("(", 1));
            for (int i = 0; i < s->def->field_count; i++) {
                if (i) out = ud_str_concat(out, ud_str_intern(", ", 2));
                out = ud_str_concat(out, s->def->field_names[i]);
                out = ud_str_concat(out, ud_str_intern("=", 1));
                out = ud_str_concat(out, ud_value_to_string(s->fields[i]));
            }
            return ud_str_concat(out, ud_str_intern(")", 1));
        }
    }
    return ud_str_intern("?", 1);
}
