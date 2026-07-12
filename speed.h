/* speed.h -- UD performance core.
 *
 * Everything on a hot path lives here: the tagged Value, the object model,
 * the arena allocator, string interning, the open-addressing hash map and the
 * VM value stack. No typedefs anywhere in UD by design -- we spell out
 * `struct ud_value` etc. so the shape of every type is always visible.
 *
 * Value representation: a 16-byte tagged struct (1 tag byte + 8-byte union).
 * We deliberately do NOT use NaN-boxing because UD exposes a real 64-bit `int`
 * distinct from `float`, and NaN-boxing cannot hold a full 64-bit integer
 * inline. Primitive values (nil/bool/int/float) live entirely inside the
 * Value, so arithmetic hot loops never touch the heap.
 */
#ifndef UD_SPEED_H
#define UD_SPEED_H

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Value                                                              */
/* ------------------------------------------------------------------ */

enum ud_type {
    UD_NIL = 0,
    UD_BOOL,
    UD_INT,
    UD_FLOAT,
    UD_OBJ
};

struct ud_obj; /* forward */

struct ud_value {
    uint8_t type;
    union {
        long long i;   /* UD_INT   */
        double    d;   /* UD_FLOAT */
        int       b;   /* UD_BOOL  */
        struct ud_obj *o; /* UD_OBJ */
    } as;
};

/* Inline constructors -- these compile away to a couple of moves. */
static inline struct ud_value ud_nil(void) {
    struct ud_value v; v.type = UD_NIL; v.as.i = 0; return v;
}
static inline struct ud_value ud_bool(int b) {
    struct ud_value v; v.type = UD_BOOL; v.as.b = b ? 1 : 0; return v;
}
static inline struct ud_value ud_int(long long i) {
    struct ud_value v; v.type = UD_INT; v.as.i = i; return v;
}
static inline struct ud_value ud_float(double d) {
    struct ud_value v; v.type = UD_FLOAT; v.as.d = d; return v;
}
static inline struct ud_value ud_obj_val(struct ud_obj *o) {
    struct ud_value v; v.type = UD_OBJ; v.as.o = o; return v;
}

#define UD_IS_NIL(v)   ((v).type == UD_NIL)
#define UD_IS_BOOL(v)  ((v).type == UD_BOOL)
#define UD_IS_INT(v)   ((v).type == UD_INT)
#define UD_IS_FLOAT(v) ((v).type == UD_FLOAT)
#define UD_IS_OBJ(v)   ((v).type == UD_OBJ)
#define UD_IS_NUM(v)   ((v).type == UD_INT || (v).type == UD_FLOAT)

#define UD_AS_INT(v)   ((v).as.i)
#define UD_AS_FLOAT(v) ((v).as.d)
#define UD_AS_BOOL(v)  ((v).as.b)
#define UD_AS_OBJ(v)   ((v).as.o)

/* Number as double regardless of int/float tag (for mixed arithmetic). */
static inline double ud_as_number(struct ud_value v) {
    return v.type == UD_INT ? (double)v.as.i : v.as.d;
}

/* ------------------------------------------------------------------ */
/* Object model                                                       */
/* ------------------------------------------------------------------ */

enum ud_obj_type {
    OBJ_STRING = 0,
    OBJ_ARRAY,
    OBJ_FUNCTION,
    OBJ_NATIVE,
    OBJ_STRUCTDEF,
    OBJ_STRUCT
};

struct ud_obj {
    uint8_t otype;
};

/* Immutable, interned, hash cached. `chars` is a flexible array member so the
 * header and the bytes live in one allocation. */
struct ud_string {
    struct ud_obj obj;
    int      length;
    uint32_t hash;
    char     chars[];
};

struct ud_array {
    struct ud_obj obj;
    int   length;
    int   cap;
    struct ud_value *items; /* grown via arena; old buffers reclaimed at exit */
};

/* One compiled UD function. The bytecode chunk is inlined here. */
struct ud_function {
    struct ud_obj obj;
    struct ud_string *name;
    int      arity;
    uint8_t *param_types; /* enum ud_type per parameter, for coercion */
    uint8_t  return_type; /* enum ud_type, or 0xFF for none */
    /* chunk */
    uint8_t *code;
    int      code_len;
    int      code_cap;
    struct ud_value *consts;
    int      const_count;
    int      const_cap;
    int     *lines;       /* source line per byte, parallel to code */
    int      num_slots;   /* local variable slots needed */
};

/* A C builtin exposed to UD (cout, cin, len, ...). */
struct ud_native {
    struct ud_obj obj;
    const char *name;
    int arity; /* -1 == variadic */
    struct ud_value (*fn)(int argc, struct ud_value *args, int line);
};

struct ud_structdef {
    struct ud_obj obj;
    struct ud_string  *name;
    int      field_count;
    struct ud_string **field_names;
    uint8_t *field_types; /* enum ud_type per field */
};

struct ud_struct {
    struct ud_obj obj;
    struct ud_structdef *def;
    struct ud_value *fields;
};

#define UD_OBJ_TYPE(v)   (UD_AS_OBJ(v)->otype)
#define UD_IS_STRING(v)  (UD_IS_OBJ(v) && UD_OBJ_TYPE(v) == OBJ_STRING)
#define UD_IS_ARRAY(v)   (UD_IS_OBJ(v) && UD_OBJ_TYPE(v) == OBJ_ARRAY)
#define UD_IS_FUNCTION(v)(UD_IS_OBJ(v) && UD_OBJ_TYPE(v) == OBJ_FUNCTION)
#define UD_IS_NATIVE(v)  (UD_IS_OBJ(v) && UD_OBJ_TYPE(v) == OBJ_NATIVE)
#define UD_IS_STRUCTDEF(v)(UD_IS_OBJ(v) && UD_OBJ_TYPE(v) == OBJ_STRUCTDEF)
#define UD_IS_STRUCT(v)  (UD_IS_OBJ(v) && UD_OBJ_TYPE(v) == OBJ_STRUCT)

#define UD_AS_STRING(v)  ((struct ud_string *)UD_AS_OBJ(v))
#define UD_AS_ARRAY(v)   ((struct ud_array *)UD_AS_OBJ(v))
#define UD_AS_FUNCTION(v)((struct ud_function *)UD_AS_OBJ(v))
#define UD_AS_NATIVE(v)  ((struct ud_native *)UD_AS_OBJ(v))
#define UD_AS_STRUCTDEF(v)((struct ud_structdef *)UD_AS_OBJ(v))
#define UD_AS_STRUCT(v)  ((struct ud_struct *)UD_AS_OBJ(v))

/* ------------------------------------------------------------------ */
/* Arena allocator                                                    */
/* ------------------------------------------------------------------ */
/* Bump allocator. UD never frees individual allocations while a program
 * runs -- everything comes from one growable arena that is destroyed at
 * process exit. That makes value/object creation branch-free and keeps the
 * hot path free of malloc bookkeeping. Long-running allocation-heavy loops
 * trade memory for speed (documented in the README). */

struct ud_arena_block {
    struct ud_arena_block *next;
    size_t used;
    size_t cap;
    char   data[];
};

struct ud_arena {
    struct ud_arena_block *head;
    size_t block_size;
    size_t total;
};

void  ud_arena_init(struct ud_arena *a, size_t block_size);
void *ud_arena_alloc(struct ud_arena *a, size_t n);
void  ud_arena_free(struct ud_arena *a);

/* The one global runtime arena. */
extern struct ud_arena ud_heap;
void  ud_heap_init(void);
void  ud_heap_shutdown(void);
static inline void *ud_alloc(size_t n) { return ud_arena_alloc(&ud_heap, n); }

/* ------------------------------------------------------------------ */
/* String interning                                                   */
/* ------------------------------------------------------------------ */

uint32_t ud_hash_bytes(const char *s, int len);

/* Returns a canonical (interned) string object for the given bytes. Equal
 * strings always return the same pointer, so string equality is a pointer
 * compare. */
struct ud_string *ud_str_intern(const char *chars, int len);
struct ud_string *ud_str_from_cstr(const char *s);
/* Take ownership of a heap buffer's contents (copied into the arena). */
struct ud_string *ud_str_concat(struct ud_string *a, struct ud_string *b);

/* ------------------------------------------------------------------ */
/* Hash map: interned-string key -> Value                             */
/* ------------------------------------------------------------------ */
/* Open addressing, linear probing. Keys are interned strings, so lookups
 * compare pointers after a cached-hash bucket probe -- no memcmp on hits. */

struct ud_map_entry {
    struct ud_string *key;
    struct ud_value   val;
};

struct ud_map {
    int count;
    int cap;
    struct ud_map_entry *entries;
};

void ud_map_init(struct ud_map *m);
int  ud_map_get(struct ud_map *m, struct ud_string *key, struct ud_value *out);
void ud_map_set(struct ud_map *m, struct ud_string *key, struct ud_value val);
int  ud_map_has(struct ud_map *m, struct ud_string *key);

/* ------------------------------------------------------------------ */
/* Object constructors                                                */
/* ------------------------------------------------------------------ */

struct ud_array    *ud_array_new(int cap);
void                ud_array_push(struct ud_array *a, struct ud_value v);
struct ud_function *ud_function_new(struct ud_string *name);
struct ud_native   *ud_native_new(const char *name, int arity,
                                  struct ud_value (*fn)(int, struct ud_value *, int));
struct ud_structdef *ud_structdef_new(struct ud_string *name, int field_count);
struct ud_struct    *ud_struct_new(struct ud_structdef *def);

/* ------------------------------------------------------------------ */
/* VM value stack                                                     */
/* ------------------------------------------------------------------ */
/* A single contiguous array of Values. push/pop are inline and do no bounds
 * checks on the hot path; overflow is guarded once per call, not per push. */

struct ud_stack {
    struct ud_value *base;
    struct ud_value *top;
    int cap;
};

void ud_stack_init(struct ud_stack *s, int cap);

static inline void ud_push(struct ud_stack *s, struct ud_value v) {
    *s->top++ = v;
}
static inline struct ud_value ud_pop(struct ud_stack *s) {
    return *(--s->top);
}
static inline struct ud_value ud_peek(struct ud_stack *s, int distance) {
    return s->top[-1 - distance];
}

/* ------------------------------------------------------------------ */
/* Value helpers                                                      */
/* ------------------------------------------------------------------ */

int  ud_value_truthy(struct ud_value v);
int  ud_value_equal(struct ud_value a, struct ud_value b);
struct ud_string *ud_value_to_string(struct ud_value v);
const char *ud_type_name(enum ud_type t);

#endif /* UD_SPEED_H */
