/* vm.c -- the one UD bytecode VM plus its builtins.
 *
 * Dispatch uses computed goto on GCC/clang (branch-predictor friendly) and
 * falls back to a switch elsewhere. The interpreter body is written once with
 * CASE()/NEXT() macros so both dispatch styles share it.
 */
#include "vm.h"
#include "errors.h"
#include "ast.h" /* DT_* decltypes: param_types/field_types hold these */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#define FRAMES_MAX 220
#define STACK_MAX  (FRAMES_MAX * 256)

struct call_frame {
    struct ud_function *fn;
    uint8_t *ip;
    struct ud_value *slots;
};

struct vm {
    struct ud_stack stack;
    struct call_frame frames[FRAMES_MAX];
    int frame_count;
    struct ud_map *globals;
    struct ud_program *prog;
};

/* ------------------------------------------------------------------ */
/* Program container                                                  */
/* ------------------------------------------------------------------ */

void ud_program_init(struct ud_program *prog) {
    ud_map_init(&prog->globals);
    prog->entry = NULL;
    prog->functions = NULL; prog->function_count = 0; prog->function_cap = 0;
    prog->structs = NULL; prog->struct_count = 0; prog->struct_cap = 0;
}

void ud_program_add_function(struct ud_program *prog, struct ud_function *fn) {
    if (prog->function_count + 1 > prog->function_cap) {
        int nc = prog->function_cap < 8 ? 8 : prog->function_cap * 2;
        struct ud_function **a =
            (struct ud_function **)ud_alloc(sizeof(*a) * (size_t)nc);
        if (prog->function_count)
            memcpy(a, prog->functions, sizeof(*a) * (size_t)prog->function_count);
        prog->functions = a; prog->function_cap = nc;
    }
    prog->functions[prog->function_count++] = fn;
}

void ud_program_add_struct(struct ud_program *prog, struct ud_structdef *sd) {
    if (prog->struct_count + 1 > prog->struct_cap) {
        int nc = prog->struct_cap < 8 ? 8 : prog->struct_cap * 2;
        struct ud_structdef **a =
            (struct ud_structdef **)ud_alloc(sizeof(*a) * (size_t)nc);
        if (prog->struct_count)
            memcpy(a, prog->structs, sizeof(*a) * (size_t)prog->struct_count);
        prog->structs = a; prog->struct_cap = nc;
    }
    prog->structs[prog->struct_count++] = sd;
}

/* ------------------------------------------------------------------ */
/* Input helpers                                                      */
/* ------------------------------------------------------------------ */

static struct ud_string *read_line(void) {
    int cap = 64, len = 0;
    char *buf = (char *)malloc((size_t)cap);
    int ch;
    while ((ch = getchar()) != EOF && ch != '\n') {
        if (len + 1 >= cap) { cap *= 2; buf = (char *)realloc(buf, (size_t)cap); }
        buf[len++] = (char)ch;
    }
    /* drop a trailing '\r' (Windows line endings) */
    if (len > 0 && buf[len - 1] == '\r') len--;
    struct ud_string *s = ud_str_intern(buf, len);
    free(buf);
    return s;
}

static void trim_bounds(const char *s, int len, int *a, int *b) {
    int i = 0, j = len;
    while (i < j && isspace((unsigned char)s[i])) i++;
    while (j > i && isspace((unsigned char)s[j - 1])) j--;
    *a = i; *b = j;
}

/* strict integer: optional sign + digits only (after trimming). */
static int parse_int_strict(const char *s, int len, long long *out) {
    int a, b; trim_bounds(s, len, &a, &b);
    if (a >= b) return 0;
    int i = a;
    if (s[i] == '+' || s[i] == '-') i++;
    if (i >= b) return 0;
    for (int k = i; k < b; k++) if (!isdigit((unsigned char)s[k])) return 0;
    char tmp[32];
    int n = b - a; if (n >= (int)sizeof(tmp)) n = (int)sizeof(tmp) - 1;
    memcpy(tmp, s + a, (size_t)n); tmp[n] = '\0';
    *out = strtoll(tmp, NULL, 10);
    return 1;
}

/* number: integer or float text (after trimming). */
static int parse_number(const char *s, int len, double *out) {
    int a, b; trim_bounds(s, len, &a, &b);
    if (a >= b) return 0;
    char tmp[64];
    int n = b - a; if (n >= (int)sizeof(tmp)) n = (int)sizeof(tmp) - 1;
    memcpy(tmp, s + a, (size_t)n); tmp[n] = '\0';
    char *endp = NULL;
    double d = strtod(tmp, &endp);
    if (endp == tmp || *endp != '\0') return 0;
    *out = d;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Explicit conversions (OP_TO_*)                                     */
/* ------------------------------------------------------------------ */

static struct ud_value convert_to_int(struct ud_value v, int line) {
    switch (v.type) {
        case UD_INT:   return v;
        case UD_FLOAT: return ud_int((long long)v.as.d); /* truncate */
        case UD_BOOL:  return ud_int(v.as.b ? 1 : 0);
        case UD_OBJ:
            if (UD_IS_STRING(v)) {
                long long out;
                struct ud_string *s = UD_AS_STRING(v);
                if (parse_int_strict(s->chars, s->length, &out)) return ud_int(out);
            }
            break;
        default: break;
    }
    ud_error(UDE_CONVERT, line, "cannot convert a %s to an int",
             UD_IS_OBJ(v) ? "value" : ud_type_name(v.type));
    return ud_nil();
}

static struct ud_value convert_to_float(struct ud_value v, int line) {
    switch (v.type) {
        case UD_INT:   return ud_float((double)v.as.i);
        case UD_FLOAT: return v;
        case UD_BOOL:  return ud_float(v.as.b ? 1.0 : 0.0);
        case UD_OBJ:
            if (UD_IS_STRING(v)) {
                double out;
                struct ud_string *s = UD_AS_STRING(v);
                if (parse_number(s->chars, s->length, &out)) return ud_float(out);
            }
            break;
        default: break;
    }
    ud_error(UDE_CONVERT, line, "cannot convert this value to a float");
    return ud_nil();
}

static struct ud_value convert_to_bool(struct ud_value v, int line) {
    switch (v.type) {
        case UD_BOOL:  return v;
        case UD_INT:   return ud_bool(v.as.i != 0);
        case UD_FLOAT: return ud_bool(v.as.d != 0.0);
        case UD_NIL:   return ud_bool(0);
        case UD_OBJ:
            if (UD_IS_STRING(v)) {
                struct ud_string *s = UD_AS_STRING(v);
                int a, b; trim_bounds(s->chars, s->length, &a, &b);
                int n = b - a;
                const char *p = s->chars + a;
                if ((n == 4 && strncmp(p, "true", 4) == 0) || (n == 1 && p[0] == '1'))
                    return ud_bool(1);
                if ((n == 5 && strncmp(p, "false", 5) == 0) || (n == 1 && p[0] == '0'))
                    return ud_bool(0);
            }
            break;
    }
    ud_error(UDE_CONVERT, line, "cannot convert this value to a bool");
    return ud_nil();
}

/* ------------------------------------------------------------------ */
/* Native builtins                                                    */
/* ------------------------------------------------------------------ */

static struct ud_value native_cout(int argc, struct ud_value *args, int line) {
    (void)line;
    for (int i = 0; i < argc; i++) {
        if (i) fputc(' ', stdout);
        struct ud_string *s = ud_value_to_string(args[i]);
        fwrite(s->chars, 1, (size_t)s->length, stdout);
    }
    fputc('\n', stdout);
    return ud_nil();
}

static long long value_length(struct ud_value v, int line) {
    if (UD_IS_STRING(v)) return UD_AS_STRING(v)->length;
    if (UD_IS_ARRAY(v))  return UD_AS_ARRAY(v)->length;
    ud_error(UDE_TYPE, line, "len() needs an array or string, not a %s",
             UD_IS_OBJ(v) ? "value" : ud_type_name(v.type));
    return 0;
}

static struct ud_value native_len(int argc, struct ud_value *args, int line) {
    if (argc != 1) ud_error(UDE_ARGCOUNT, line, "len() takes exactly one value");
    return ud_int(value_length(args[0], line));
}

static struct ud_value native_type(int argc, struct ud_value *args, int line) {
    if (argc != 1) ud_error(UDE_ARGCOUNT, line, "type() takes exactly one value");
    struct ud_value v = args[0];
    const char *name = "nil";
    switch (v.type) {
        case UD_NIL: name = "nil"; break;
        case UD_BOOL: name = "bool"; break;
        case UD_INT: name = "int"; break;
        case UD_FLOAT: name = "float"; break;
        case UD_OBJ:
            switch (v.as.o->otype) {
                case OBJ_STRING: name = "string"; break;
                case OBJ_ARRAY: name = "array"; break;
                case OBJ_FUNCTION: case OBJ_NATIVE: name = "function"; break;
                case OBJ_STRUCTDEF: name = "structdef"; break;
                case OBJ_STRUCT: name = "struct"; break;
            }
            break;
    }
    return ud_obj_val((struct ud_obj *)ud_str_from_cstr(name));
}

void ud_register_builtins(struct ud_program *prog) {
    struct { const char *name; int arity;
             struct ud_value (*fn)(int, struct ud_value *, int); } b[] = {
        {"cout", -1, native_cout},
        {"len",   1, native_len},
        {"type",  1, native_type},
        {NULL, 0, NULL}
    };
    for (int i = 0; b[i].name; i++) {
        struct ud_native *n = ud_native_new(b[i].name, b[i].arity, b[i].fn);
        ud_map_set(&prog->globals, ud_str_from_cstr(b[i].name),
                   ud_obj_val((struct ud_obj *)n));
    }
}

/* ------------------------------------------------------------------ */
/* Runtime helpers used by the interpreter loop                       */
/* ------------------------------------------------------------------ */

/* A readable type name for error messages (spells out object subtypes). */
static const char *human_type(struct ud_value v) {
    if (!UD_IS_OBJ(v)) return ud_type_name(v.type);
    switch (UD_OBJ_TYPE(v)) {
        case OBJ_STRING:    return "string";
        case OBJ_ARRAY:     return "array";
        case OBJ_FUNCTION:  case OBJ_NATIVE: return "function";
        case OBJ_STRUCTDEF: return "structdef";
        case OBJ_STRUCT:    return "struct";
        default:            return "value";
    }
}

/* Coerce a value to a declared type (DT_*). Used for typed parameters and
 * typed struct fields; DT_NONE passes the value through untouched. */
static struct ud_value coerce_decl(struct ud_value v, uint8_t dt, int line) {
    switch (dt) {
        case DT_INT:    return convert_to_int(v, line);
        case DT_FLOAT:  return convert_to_float(v, line);
        case DT_BOOL:   return convert_to_bool(v, line);
        case DT_STRING: return ud_obj_val((struct ud_obj *)ud_value_to_string(v));
        default:        return v; /* DT_NONE */
    }
}

static int structdef_field_index(struct ud_structdef *d, struct ud_string *name) {
    for (int i = 0; i < d->field_count; i++)
        if (d->field_names[i] == name) return i; /* interned: pointer compare */
    return -1;
}

static struct ud_value construct_struct(struct ud_structdef *def,
                                        struct ud_value *args, int argc, int line) {
    if (argc != def->field_count)
        ud_error(UDE_STRUCT, line, "%s needs %d field%s but got %d",
                 def->name->chars, def->field_count,
                 def->field_count == 1 ? "" : "s", argc);
    struct ud_struct *s = ud_struct_new(def);
    for (int i = 0; i < argc; i++)
        s->fields[i] = coerce_decl(args[i], def->field_types[i], line);
    return ud_obj_val((struct ud_obj *)s);
}

/* Print a cin prompt (when one was supplied) without a trailing newline. */
static void cin_prompt(struct ud_value p) {
    if (UD_IS_NIL(p)) return;
    struct ud_string *s = ud_value_to_string(p);
    fwrite(s->chars, 1, (size_t)s->length, stdout);
    fflush(stdout);
}

/* Lexicographic ordering of two strings: <0, 0, >0. */
static int str_order(struct ud_string *a, struct ud_string *b) {
    int n = a->length < b->length ? a->length : b->length;
    int c = n ? memcmp(a->chars, b->chars, (size_t)n) : 0;
    if (c != 0) return c;
    return a->length - b->length;
}

static void *xmalloc(size_t n, int line) {
    void *p = malloc(n ? n : 1);
    if (!p) ud_error(UDE_INTERNAL, line, "out of memory");
    return p;
}

static void want_argc(int got, int want, const char *who, int line) {
    if (got != want)
        ud_error(UDE_ARGCOUNT, line, "%s expects %d argument%s but got %d",
                 who, want, want == 1 ? "" : "s", got);
}

static struct ud_string *want_string(struct ud_value v, const char *who, int line) {
    if (!UD_IS_STRING(v)) ud_error(UDE_TYPE, line, "%s expects a string", who);
    return UD_AS_STRING(v);
}

/* ------------------------------------------------------------------ */
/* String methods                                                     */
/* ------------------------------------------------------------------ */

static struct ud_string *str_case(struct ud_string *s, int upper, int line) {
    if (s->length == 0) return s;
    char *buf = (char *)xmalloc((size_t)s->length, line);
    for (int i = 0; i < s->length; i++) {
        unsigned char ch = (unsigned char)s->chars[i];
        buf[i] = (char)(upper ? toupper(ch) : tolower(ch));
    }
    struct ud_string *r = ud_str_intern(buf, s->length);
    free(buf);
    return r;
}

static struct ud_string *str_trim(struct ud_string *s) {
    int a, b; trim_bounds(s->chars, s->length, &a, &b);
    return ud_str_intern(s->chars + a, b - a);
}

static long long str_find(struct ud_string *s, struct ud_string *sub) {
    if (sub->length == 0) return 0;
    for (int i = 0; i + sub->length <= s->length; i++)
        if (memcmp(s->chars + i, sub->chars, (size_t)sub->length) == 0) return i;
    return -1;
}

static struct ud_string *str_replace(struct ud_string *s, struct ud_string *old,
                                     struct ud_string *neu, int line) {
    if (old->length == 0) return s; /* empty needle would never terminate */
    int count = 0;
    for (int i = 0; i + old->length <= s->length; ) {
        if (memcmp(s->chars + i, old->chars, (size_t)old->length) == 0) {
            count++; i += old->length;
        } else i++;
    }
    if (count == 0) return s;
    int newlen = s->length + count * (neu->length - old->length);
    char *buf = (char *)xmalloc((size_t)newlen + 1, line);
    int w = 0;
    for (int i = 0; i < s->length; ) {
        if (i + old->length <= s->length &&
            memcmp(s->chars + i, old->chars, (size_t)old->length) == 0) {
            memcpy(buf + w, neu->chars, (size_t)neu->length);
            w += neu->length; i += old->length;
        } else buf[w++] = s->chars[i++];
    }
    struct ud_string *r = ud_str_intern(buf, newlen);
    free(buf);
    return r;
}

static struct ud_value str_split(struct ud_string *s, struct ud_string *sep) {
    struct ud_array *arr = ud_array_new(4);
    if (sep == NULL) { /* no separator: split on runs of whitespace */
        int i = 0;
        while (i < s->length) {
            while (i < s->length && isspace((unsigned char)s->chars[i])) i++;
            int start = i;
            while (i < s->length && !isspace((unsigned char)s->chars[i])) i++;
            if (i > start)
                ud_array_push(arr, ud_obj_val((struct ud_obj *)
                    ud_str_intern(s->chars + start, i - start)));
        }
    } else if (sep->length == 0) { /* empty separator: split into characters */
        for (int i = 0; i < s->length; i++)
            ud_array_push(arr, ud_obj_val((struct ud_obj *)
                ud_str_intern(s->chars + i, 1)));
    } else {
        int start = 0;
        for (int i = 0; i + sep->length <= s->length; ) {
            if (memcmp(s->chars + i, sep->chars, (size_t)sep->length) == 0) {
                ud_array_push(arr, ud_obj_val((struct ud_obj *)
                    ud_str_intern(s->chars + start, i - start)));
                i += sep->length; start = i;
            } else i++;
        }
        ud_array_push(arr, ud_obj_val((struct ud_obj *)
            ud_str_intern(s->chars + start, s->length - start)));
    }
    return ud_obj_val((struct ud_obj *)arr);
}

static struct ud_string *join_array(struct ud_string *sep, struct ud_array *a) {
    struct ud_string *out = ud_str_intern("", 0);
    for (int i = 0; i < a->length; i++) {
        if (i) out = ud_str_concat(out, sep);
        out = ud_str_concat(out, ud_value_to_string(a->items[i]));
    }
    return out;
}

static struct ud_value str_method(struct ud_string *s, struct ud_string *name,
                                  int argc, struct ud_value *args, int line) {
    const char *m = name->chars;
    if (strcmp(m, "upper") == 0) { want_argc(argc, 0, "upper()", line);
        return ud_obj_val((struct ud_obj *)str_case(s, 1, line)); }
    if (strcmp(m, "lower") == 0) { want_argc(argc, 0, "lower()", line);
        return ud_obj_val((struct ud_obj *)str_case(s, 0, line)); }
    if (strcmp(m, "trim") == 0 || strcmp(m, "strip") == 0) {
        want_argc(argc, 0, "trim()", line);
        return ud_obj_val((struct ud_obj *)str_trim(s)); }
    if (strcmp(m, "length") == 0 || strcmp(m, "len") == 0) {
        want_argc(argc, 0, "length()", line); return ud_int(s->length); }
    if (strcmp(m, "replace") == 0) { want_argc(argc, 2, "replace()", line);
        return ud_obj_val((struct ud_obj *)str_replace(s,
            want_string(args[0], "replace()", line),
            want_string(args[1], "replace()", line), line)); }
    if (strcmp(m, "split") == 0) {
        if (argc == 0) return str_split(s, NULL);
        if (argc == 1) return str_split(s, want_string(args[0], "split()", line));
        want_argc(argc, 1, "split()", line); }
    if (strcmp(m, "find") == 0 || strcmp(m, "index") == 0) {
        want_argc(argc, 1, "find()", line);
        return ud_int(str_find(s, want_string(args[0], "find()", line))); }
    if (strcmp(m, "contains") == 0) { want_argc(argc, 1, "contains()", line);
        return ud_bool(str_find(s, want_string(args[0], "contains()", line)) >= 0); }
    if (strcmp(m, "join") == 0) { want_argc(argc, 1, "join()", line);
        if (!UD_IS_ARRAY(args[0]))
            ud_error(UDE_TYPE, line, "join() needs an array to join");
        return ud_obj_val((struct ud_obj *)join_array(s, UD_AS_ARRAY(args[0]))); }
    ud_error(UDE_ATTR, line, "strings have no method '%s'", m);
    return ud_nil();
}

/* ------------------------------------------------------------------ */
/* Array methods                                                      */
/* ------------------------------------------------------------------ */

static struct ud_value arr_method(struct ud_array *a, struct ud_string *name,
                                  int argc, struct ud_value *args, int line) {
    const char *m = name->chars;
    if (strcmp(m, "append") == 0 || strcmp(m, "push") == 0) {
        want_argc(argc, 1, "append()", line);
        ud_array_push(a, args[0]); return ud_nil(); }
    if (strcmp(m, "pop") == 0) { want_argc(argc, 0, "pop()", line);
        if (a->length == 0) ud_error(UDE_INDEX, line, "cannot pop from an empty array");
        return a->items[--a->length]; }
    if (strcmp(m, "length") == 0 || strcmp(m, "len") == 0) {
        want_argc(argc, 0, "length()", line); return ud_int(a->length); }
    if (strcmp(m, "contains") == 0) { want_argc(argc, 1, "contains()", line);
        for (int i = 0; i < a->length; i++)
            if (ud_value_equal(a->items[i], args[0])) return ud_bool(1);
        return ud_bool(0); }
    if (strcmp(m, "index") == 0 || strcmp(m, "find") == 0) {
        want_argc(argc, 1, "index()", line);
        for (int i = 0; i < a->length; i++)
            if (ud_value_equal(a->items[i], args[0])) return ud_int(i);
        return ud_int(-1); }
    if (strcmp(m, "join") == 0) { want_argc(argc, 1, "join()", line);
        return ud_obj_val((struct ud_obj *)
            join_array(want_string(args[0], "join()", line), a)); }
    ud_error(UDE_ATTR, line, "arrays have no method '%s'", m);
    return ud_nil();
}

/* Resolve a Python-style slice bound against a container length. */
static long long slice_bound(struct ud_value v, long long def, long long len, int line) {
    if (UD_IS_NIL(v)) return def;
    if (!UD_IS_INT(v)) ud_error(UDE_SLICE, line, "slice bounds must be whole numbers");
    long long x = v.as.i;
    if (x < 0) x += len;
    if (x < 0) x = 0;
    if (x > len) x = len;
    return x;
}

/* Integer exponentiation for the int**int (non-negative) fast path. */
static long long ipow(long long base, long long exp) {
    long long r = 1;
    while (exp > 0) {
        if (exp & 1) r *= base;
        base *= base;
        exp >>= 1;
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* The interpreter                                                    */
/* ------------------------------------------------------------------ */

int ud_vm_run(struct ud_program *prog) {
    struct vm vm;
    ud_stack_init(&vm.stack, STACK_MAX);
    vm.frame_count = 0;
    vm.globals = &prog->globals;
    vm.prog = prog;

    struct ud_function *entry = prog->entry;

    /* Seed the entry frame. The callee sits one slot below its locals so the
     * uniform "slots = top - argc" layout works for entry() too. */
    ud_push(&vm.stack, ud_obj_val((struct ud_obj *)entry));
    struct call_frame *frame = &vm.frames[vm.frame_count++];
    frame->fn = entry;
    frame->ip = entry->code;
    frame->slots = vm.stack.top;

    int exit_code = 0;

#define READ_BYTE()  (*frame->ip++)
#define READ_U16()   (frame->ip += 2, \
                      (int)((uint16_t)frame->ip[-2] | ((uint16_t)frame->ip[-1] << 8)))
#define READ_CONST() (frame->fn->consts[READ_U16()])
#define LINE()       (frame->fn->lines[(int)((frame->ip - 1) - frame->fn->code)])
#define PUSH(v)      ud_push(&vm.stack, (v))
#define POP()        ud_pop(&vm.stack)

#if defined(__GNUC__) || defined(__clang__)
# define UD_USE_GOTO 1
#endif

#ifdef UD_USE_GOTO
    /* Order-independent dispatch table keyed by opcode -- robust against enum
     * reordering. Computed goto keeps the branch predictor happy. */
    static const void *const dtab[] = {
        [OP_CONST]=&&L_OP_CONST, [OP_NIL]=&&L_OP_NIL, [OP_TRUE]=&&L_OP_TRUE,
        [OP_FALSE]=&&L_OP_FALSE, [OP_POP]=&&L_OP_POP, [OP_DUP]=&&L_OP_DUP,
        [OP_GET_LOCAL]=&&L_OP_GET_LOCAL, [OP_SET_LOCAL]=&&L_OP_SET_LOCAL,
        [OP_GET_GLOBAL]=&&L_OP_GET_GLOBAL, [OP_SET_GLOBAL]=&&L_OP_SET_GLOBAL,
        [OP_ADD]=&&L_OP_ADD, [OP_SUB]=&&L_OP_SUB, [OP_MUL]=&&L_OP_MUL,
        [OP_DIV]=&&L_OP_DIV, [OP_MOD]=&&L_OP_MOD, [OP_POW]=&&L_OP_POW,
        [OP_NEG]=&&L_OP_NEG, [OP_EQ]=&&L_OP_EQ, [OP_NE]=&&L_OP_NE,
        [OP_LT]=&&L_OP_LT, [OP_GT]=&&L_OP_GT, [OP_LE]=&&L_OP_LE, [OP_GE]=&&L_OP_GE,
        [OP_NOT]=&&L_OP_NOT, [OP_BAND]=&&L_OP_BAND, [OP_BOR]=&&L_OP_BOR,
        [OP_BXOR]=&&L_OP_BXOR, [OP_BNOT]=&&L_OP_BNOT, [OP_SHL]=&&L_OP_SHL,
        [OP_SHR]=&&L_OP_SHR, [OP_CONCAT]=&&L_OP_CONCAT, [OP_LEN]=&&L_OP_LEN,
        [OP_JUMP]=&&L_OP_JUMP, [OP_JUMP_IF_FALSE]=&&L_OP_JUMP_IF_FALSE,
        [OP_JUMP_IF_TRUE]=&&L_OP_JUMP_IF_TRUE, [OP_LOOP]=&&L_OP_LOOP,
        [OP_CALL]=&&L_OP_CALL, [OP_RETURN]=&&L_OP_RETURN, [OP_ARRAY]=&&L_OP_ARRAY,
        [OP_INDEX_GET]=&&L_OP_INDEX_GET, [OP_INDEX_SET]=&&L_OP_INDEX_SET,
        [OP_SLICE]=&&L_OP_SLICE, [OP_INVOKE]=&&L_OP_INVOKE,
        [OP_STRUCT_NEW]=&&L_OP_STRUCT_NEW, [OP_FIELD_GET]=&&L_OP_FIELD_GET,
        [OP_FIELD_SET]=&&L_OP_FIELD_SET, [OP_CIN]=&&L_OP_CIN,
        [OP_CIN_INT]=&&L_OP_CIN_INT, [OP_CIN_FLOAT]=&&L_OP_CIN_FLOAT,
        [OP_CIN_BOOL]=&&L_OP_CIN_BOOL, [OP_TO_INT]=&&L_OP_TO_INT,
        [OP_TO_FLOAT]=&&L_OP_TO_FLOAT, [OP_TO_BOOL]=&&L_OP_TO_BOOL,
        [OP_TO_STRING]=&&L_OP_TO_STRING, [OP_PRINT]=&&L_OP_PRINT,
        [OP_HALT]=&&L_OP_HALT
    };
# define CASE(op) L_##op
# define NEXT()   goto *dtab[READ_BYTE()]
    NEXT();
#else
# define CASE(op) case op
# define NEXT()   goto vm_loop
    for (;;) {
    vm_loop:
        switch (READ_BYTE()) {
#endif

    CASE(OP_CONST):   PUSH(READ_CONST()); NEXT();
    CASE(OP_NIL):     PUSH(ud_nil());     NEXT();
    CASE(OP_TRUE):    PUSH(ud_bool(1));   NEXT();
    CASE(OP_FALSE):   PUSH(ud_bool(0));   NEXT();
    CASE(OP_POP):     (void)POP();        NEXT();
    CASE(OP_DUP):     PUSH(ud_peek(&vm.stack, 0)); NEXT();

    CASE(OP_GET_LOCAL): { uint8_t slot = READ_BYTE(); PUSH(frame->slots[slot]); NEXT(); }
    CASE(OP_SET_LOCAL): { uint8_t slot = READ_BYTE();
                          frame->slots[slot] = ud_peek(&vm.stack, 0); NEXT(); }

    CASE(OP_GET_GLOBAL): {
        int line = LINE();
        struct ud_string *name = UD_AS_STRING(READ_CONST());
        struct ud_value out;
        if (!ud_map_get(vm.globals, name, &out))
            ud_error(UDE_UNDEF_VAR, line, "nothing named '%s' is defined", name->chars);
        PUSH(out); NEXT();
    }
    CASE(OP_SET_GLOBAL): {
        struct ud_string *name = UD_AS_STRING(READ_CONST());
        ud_map_set(vm.globals, name, ud_peek(&vm.stack, 0));
        NEXT();
    }

    CASE(OP_ADD): {
        struct ud_value b = POP(), a = POP();
        if (UD_IS_INT(a) && UD_IS_INT(b)) PUSH(ud_int(a.as.i + b.as.i));
        else if (UD_IS_NUM(a) && UD_IS_NUM(b))
            PUSH(ud_float(ud_as_number(a) + ud_as_number(b)));
        else ud_error(UDE_TYPE, LINE(),
                      "'+' needs two numbers; use '..' to join strings");
        NEXT();
    }
    CASE(OP_SUB): {
        struct ud_value b = POP(), a = POP();
        if (UD_IS_INT(a) && UD_IS_INT(b)) PUSH(ud_int(a.as.i - b.as.i));
        else if (UD_IS_NUM(a) && UD_IS_NUM(b))
            PUSH(ud_float(ud_as_number(a) - ud_as_number(b)));
        else ud_error(UDE_TYPE, LINE(), "'-' needs two numbers");
        NEXT();
    }
    CASE(OP_MUL): {
        struct ud_value b = POP(), a = POP();
        if (UD_IS_INT(a) && UD_IS_INT(b)) PUSH(ud_int(a.as.i * b.as.i));
        else if (UD_IS_NUM(a) && UD_IS_NUM(b))
            PUSH(ud_float(ud_as_number(a) * ud_as_number(b)));
        else ud_error(UDE_TYPE, LINE(), "'*' needs two numbers");
        NEXT();
    }
    CASE(OP_DIV): {
        int line = LINE();
        struct ud_value b = POP(), a = POP();
        if (!UD_IS_NUM(a) || !UD_IS_NUM(b))
            ud_error(UDE_TYPE, line, "'/' needs two numbers");
        if (UD_IS_INT(a) && UD_IS_INT(b)) {
            if (b.as.i == 0) ud_error(UDE_DIVZERO, line, "you divided by zero");
            PUSH(ud_int(a.as.i / b.as.i));
        } else {
            double db = ud_as_number(b);
            if (db == 0.0) ud_error(UDE_DIVZERO, line, "you divided by zero");
            PUSH(ud_float(ud_as_number(a) / db));
        }
        NEXT();
    }
    CASE(OP_MOD): {
        int line = LINE();
        struct ud_value b = POP(), a = POP();
        if (!UD_IS_NUM(a) || !UD_IS_NUM(b))
            ud_error(UDE_TYPE, line, "'%%' needs two numbers");
        if (UD_IS_INT(a) && UD_IS_INT(b)) {
            if (b.as.i == 0) ud_error(UDE_DIVZERO, line, "you took a remainder by zero");
            PUSH(ud_int(a.as.i % b.as.i));
        } else {
            double db = ud_as_number(b);
            if (db == 0.0) ud_error(UDE_DIVZERO, line, "you took a remainder by zero");
            PUSH(ud_float(fmod(ud_as_number(a), db)));
        }
        NEXT();
    }
    CASE(OP_POW): {
        struct ud_value b = POP(), a = POP();
        if (!UD_IS_NUM(a) || !UD_IS_NUM(b))
            ud_error(UDE_TYPE, LINE(), "'**' needs two numbers");
        if (UD_IS_INT(a) && UD_IS_INT(b) && b.as.i >= 0)
            PUSH(ud_int(ipow(a.as.i, b.as.i)));
        else PUSH(ud_float(pow(ud_as_number(a), ud_as_number(b))));
        NEXT();
    }
    CASE(OP_NEG): {
        struct ud_value a = POP();
        if (UD_IS_INT(a)) PUSH(ud_int(-a.as.i));
        else if (UD_IS_FLOAT(a)) PUSH(ud_float(-a.as.d));
        else ud_error(UDE_TYPE, LINE(), "you can only negate a number");
        NEXT();
    }

    CASE(OP_EQ): { struct ud_value b = POP(), a = POP();
                   PUSH(ud_bool(ud_value_equal(a, b))); NEXT(); }
    CASE(OP_NE): { struct ud_value b = POP(), a = POP();
                   PUSH(ud_bool(!ud_value_equal(a, b))); NEXT(); }

#define CMP(opname, cmpop) \
    CASE(opname): { \
        int line = LINE(); \
        struct ud_value b = POP(), a = POP(); \
        if (UD_IS_NUM(a) && UD_IS_NUM(b)) \
            PUSH(ud_bool(ud_as_number(a) cmpop ud_as_number(b))); \
        else if (UD_IS_STRING(a) && UD_IS_STRING(b)) \
            PUSH(ud_bool(str_order(UD_AS_STRING(a), UD_AS_STRING(b)) cmpop 0)); \
        else ud_error(UDE_TYPE, line, "can only compare two numbers or two strings"); \
        NEXT(); \
    }
    CMP(OP_LT, <) CMP(OP_GT, >) CMP(OP_LE, <=) CMP(OP_GE, >=)
#undef CMP

    CASE(OP_NOT): { struct ud_value a = POP();
                    PUSH(ud_bool(!ud_value_truthy(a))); NEXT(); }

#define BITOP(opname, bitop, sym) \
    CASE(opname): { \
        int line = LINE(); \
        struct ud_value b = POP(), a = POP(); \
        if (!UD_IS_INT(a) || !UD_IS_INT(b)) \
            ud_error(UDE_TYPE, line, "'" sym "' needs two ints"); \
        PUSH(ud_int(a.as.i bitop b.as.i)); \
        NEXT(); \
    }
    BITOP(OP_BAND, &, "&") BITOP(OP_BOR, |, "|") BITOP(OP_BXOR, ^, "^")
#undef BITOP
    CASE(OP_SHL): { int line = LINE(); struct ud_value b = POP(), a = POP();
        if (!UD_IS_INT(a) || !UD_IS_INT(b)) ud_error(UDE_TYPE, line, "'<<' needs two ints");
        PUSH(ud_int(a.as.i << (b.as.i & 63))); NEXT(); }
    CASE(OP_SHR): { int line = LINE(); struct ud_value b = POP(), a = POP();
        if (!UD_IS_INT(a) || !UD_IS_INT(b)) ud_error(UDE_TYPE, line, "'>>' needs two ints");
        PUSH(ud_int(a.as.i >> (b.as.i & 63))); NEXT(); }
    CASE(OP_BNOT): { struct ud_value a = POP();
        if (!UD_IS_INT(a)) ud_error(UDE_TYPE, LINE(), "'~' needs an int");
        PUSH(ud_int(~a.as.i)); NEXT(); }

    CASE(OP_CONCAT): {
        struct ud_value b = POP(), a = POP();
        PUSH(ud_obj_val((struct ud_obj *)
            ud_str_concat(ud_value_to_string(a), ud_value_to_string(b))));
        NEXT();
    }
    CASE(OP_LEN): { int line = LINE(); struct ud_value a = POP();
                    PUSH(ud_int(value_length(a, line))); NEXT(); }

    CASE(OP_JUMP): { int off = READ_U16(); frame->ip += off; NEXT(); }
    CASE(OP_JUMP_IF_FALSE): {
        int off = READ_U16(); struct ud_value v = POP();
        if (!ud_value_truthy(v)) frame->ip += off;
        NEXT();
    }
    CASE(OP_JUMP_IF_TRUE): {
        int off = READ_U16(); struct ud_value v = POP();
        if (ud_value_truthy(v)) frame->ip += off;
        NEXT();
    }
    CASE(OP_LOOP): { int off = READ_U16(); frame->ip -= off; NEXT(); }

    CASE(OP_CALL): {
        int line = LINE();
        uint8_t argc = READ_BYTE();
        struct ud_value callee = ud_peek(&vm.stack, argc);
        if (UD_IS_FUNCTION(callee)) {
            struct ud_function *fn = UD_AS_FUNCTION(callee);
            if (argc != fn->arity)
                ud_error(UDE_ARGCOUNT, line, "%s() expects %d argument%s but got %d",
                         fn->name ? fn->name->chars : "?", fn->arity,
                         fn->arity == 1 ? "" : "s", argc);
            if (vm.frame_count >= FRAMES_MAX)
                ud_error(UDE_STACKOVERFLOW, line,
                         "too many nested calls (runaway recursion?)");
            struct ud_value *slots = vm.stack.top - argc;
            if (fn->param_types)
                for (int i = 0; i < argc; i++)
                    if (fn->param_types[i] != DT_NONE)
                        slots[i] = coerce_decl(slots[i], fn->param_types[i], line);
            if (slots + fn->num_slots + 8 > vm.stack.base + vm.stack.cap)
                ud_error(UDE_STACKOVERFLOW, line, "the value stack overflowed");
            frame = &vm.frames[vm.frame_count++];
            frame->fn = fn;
            frame->ip = fn->code;
            frame->slots = slots;
            NEXT();
        }
        if (UD_IS_NATIVE(callee)) {
            struct ud_native *nat = UD_AS_NATIVE(callee);
            if (nat->arity >= 0 && argc != nat->arity)
                ud_error(UDE_ARGCOUNT, line, "%s() expects %d argument%s but got %d",
                         nat->name, nat->arity, nat->arity == 1 ? "" : "s", argc);
            struct ud_value *args = vm.stack.top - argc;
            struct ud_value r = nat->fn(argc, args, line);
            vm.stack.top -= (argc + 1);
            PUSH(r);
            NEXT();
        }
        if (UD_IS_STRUCTDEF(callee)) {
            struct ud_value *args = vm.stack.top - argc;
            struct ud_value s = construct_struct(UD_AS_STRUCTDEF(callee), args, argc, line);
            vm.stack.top -= (argc + 1);
            PUSH(s);
            NEXT();
        }
        ud_error(UDE_NOT_CALLABLE, line,
                 "you tried to call a %s, which is not a function", human_type(callee));
        NEXT();
    }

    CASE(OP_RETURN): {
        struct ud_value result = POP();
        vm.frame_count--;
        if (vm.frame_count == 0) {
            exit_code = UD_IS_INT(result) ? (int)result.as.i
                      : UD_IS_BOOL(result) ? result.as.b : 0;
            goto finished;
        }
        vm.stack.top = frame->slots - 1; /* drop callee + args + locals */
        PUSH(result);
        frame = &vm.frames[vm.frame_count - 1];
        NEXT();
    }

    CASE(OP_ARRAY): {
        int count = READ_U16();
        struct ud_array *arr = ud_array_new(count > 0 ? count : 4);
        struct ud_value *src = vm.stack.top - count;
        for (int i = 0; i < count; i++) ud_array_push(arr, src[i]);
        vm.stack.top -= count;
        PUSH(ud_obj_val((struct ud_obj *)arr));
        NEXT();
    }

    CASE(OP_INDEX_GET): {
        int line = LINE();
        struct ud_value idx = POP(), cont = POP();
        if (UD_IS_ARRAY(cont)) {
            struct ud_array *a = UD_AS_ARRAY(cont);
            if (!UD_IS_INT(idx)) ud_error(UDE_TYPE, line, "array index must be a whole number");
            long long i = idx.as.i; if (i < 0) i += a->length;
            if (i < 0 || i >= a->length)
                ud_error(UDE_INDEX, line,
                         "index %lld is out of range for an array of length %d",
                         idx.as.i, a->length);
            PUSH(a->items[i]);
        } else if (UD_IS_STRING(cont)) {
            struct ud_string *s = UD_AS_STRING(cont);
            if (!UD_IS_INT(idx)) ud_error(UDE_TYPE, line, "string index must be a whole number");
            long long i = idx.as.i; if (i < 0) i += s->length;
            if (i < 0 || i >= s->length)
                ud_error(UDE_INDEX, line,
                         "index %lld is out of range for a string of length %d",
                         idx.as.i, s->length);
            PUSH(ud_obj_val((struct ud_obj *)ud_str_intern(s->chars + i, 1)));
        } else {
            ud_error(UDE_NOT_INDEXABLE, line,
                     "you can only index arrays and strings, not a %s", human_type(cont));
        }
        NEXT();
    }
    CASE(OP_INDEX_SET): {
        int line = LINE();
        struct ud_value val = POP(), idx = POP(), cont = POP();
        if (UD_IS_ARRAY(cont)) {
            struct ud_array *a = UD_AS_ARRAY(cont);
            if (!UD_IS_INT(idx)) ud_error(UDE_TYPE, line, "array index must be a whole number");
            long long i = idx.as.i; if (i < 0) i += a->length;
            if (i < 0 || i >= a->length)
                ud_error(UDE_INDEX, line,
                         "index %lld is out of range for an array of length %d",
                         idx.as.i, a->length);
            a->items[i] = val;
        } else if (UD_IS_STRING(cont)) {
            ud_error(UDE_TYPE, line, "strings can't be changed in place");
        } else {
            ud_error(UDE_NOT_INDEXABLE, line,
                     "you can only index arrays and strings, not a %s", human_type(cont));
        }
        PUSH(val);
        NEXT();
    }
    CASE(OP_SLICE): {
        int line = LINE();
        uint8_t flags = READ_BYTE();
        struct ud_value stopv = ud_nil(), startv = ud_nil();
        if (flags & 2) stopv = POP();
        if (flags & 1) startv = POP();
        struct ud_value target = POP();
        long long len;
        if (UD_IS_ARRAY(target))       len = UD_AS_ARRAY(target)->length;
        else if (UD_IS_STRING(target)) len = UD_AS_STRING(target)->length;
        else { ud_error(UDE_NOT_INDEXABLE, line, "you can only slice arrays and strings"); return 0; }
        long long start = slice_bound(startv, 0, len, line);
        long long stop  = slice_bound(stopv, len, len, line);
        if (stop < start) stop = start;
        if (UD_IS_ARRAY(target)) {
            struct ud_array *src = UD_AS_ARRAY(target);
            struct ud_array *out = ud_array_new((int)(stop - start > 0 ? stop - start : 4));
            for (long long i = start; i < stop; i++) ud_array_push(out, src->items[i]);
            PUSH(ud_obj_val((struct ud_obj *)out));
        } else {
            struct ud_string *src = UD_AS_STRING(target);
            PUSH(ud_obj_val((struct ud_obj *)
                ud_str_intern(src->chars + start, (int)(stop - start))));
        }
        NEXT();
    }

    CASE(OP_INVOKE): {
        int line = LINE();
        struct ud_string *mname = UD_AS_STRING(READ_CONST());
        uint8_t argc = READ_BYTE();
        struct ud_value target = ud_peek(&vm.stack, argc);
        struct ud_value *args = vm.stack.top - argc;
        struct ud_value r;
        if (UD_IS_STRING(target))      r = str_method(UD_AS_STRING(target), mname, argc, args, line);
        else if (UD_IS_ARRAY(target))  r = arr_method(UD_AS_ARRAY(target), mname, argc, args, line);
        else ud_error(UDE_ATTR, line, "a %s has no method '%s'", human_type(target), mname->chars);
        vm.stack.top -= (argc + 1);
        PUSH(r);
        NEXT();
    }
    CASE(OP_STRUCT_NEW): {
        int line = LINE();
        struct ud_string *sname = UD_AS_STRING(READ_CONST());
        uint8_t argc = READ_BYTE();
        struct ud_value defv;
        if (!ud_map_get(vm.globals, sname, &defv) || !UD_IS_STRUCTDEF(defv))
            ud_error(UDE_STRUCT, line, "no struct named '%s'", sname->chars);
        struct ud_value *args = vm.stack.top - argc;
        struct ud_value s = construct_struct(UD_AS_STRUCTDEF(defv), args, argc, line);
        vm.stack.top -= argc;
        PUSH(s);
        NEXT();
    }
    CASE(OP_FIELD_GET): {
        int line = LINE();
        struct ud_string *fname = UD_AS_STRING(READ_CONST());
        struct ud_value target = POP();
        if (!UD_IS_STRUCT(target))
            ud_error(UDE_FIELD, line, "only structs have fields; this is a %s", human_type(target));
        struct ud_struct *st = UD_AS_STRUCT(target);
        int idx = structdef_field_index(st->def, fname);
        if (idx < 0)
            ud_error(UDE_FIELD, line, "struct %s has no field '%s'",
                     st->def->name->chars, fname->chars);
        PUSH(st->fields[idx]);
        NEXT();
    }
    CASE(OP_FIELD_SET): {
        int line = LINE();
        struct ud_string *fname = UD_AS_STRING(READ_CONST());
        struct ud_value val = POP(), target = POP();
        if (!UD_IS_STRUCT(target))
            ud_error(UDE_FIELD, line, "only structs have fields; this is a %s", human_type(target));
        struct ud_struct *st = UD_AS_STRUCT(target);
        int idx = structdef_field_index(st->def, fname);
        if (idx < 0)
            ud_error(UDE_FIELD, line, "struct %s has no field '%s'",
                     st->def->name->chars, fname->chars);
        val = coerce_decl(val, st->def->field_types[idx], line);
        st->fields[idx] = val;
        PUSH(val);
        NEXT();
    }

    CASE(OP_CIN): {
        struct ud_value prompt = POP();
        cin_prompt(prompt);
        PUSH(ud_obj_val((struct ud_obj *)read_line()));
        NEXT();
    }
    CASE(OP_CIN_INT): {
        int line = LINE();
        struct ud_value prompt = POP();
        cin_prompt(prompt);
        struct ud_string *s = read_line();
        long long out;
        if (!parse_int_strict(s->chars, s->length, &out))
            ud_error(UDE_CIN_INT, line,
                     "expected a whole number but got \"%.*s\"", s->length, s->chars);
        PUSH(ud_int(out));
        NEXT();
    }
    CASE(OP_CIN_FLOAT): {
        int line = LINE();
        struct ud_value prompt = POP();
        cin_prompt(prompt);
        struct ud_string *s = read_line();
        double out;
        if (!parse_number(s->chars, s->length, &out))
            ud_error(UDE_CIN_FLOAT, line,
                     "expected a number but got \"%.*s\"", s->length, s->chars);
        PUSH(ud_float(out));
        NEXT();
    }
    CASE(OP_CIN_BOOL): {
        int line = LINE();
        struct ud_value prompt = POP();
        cin_prompt(prompt);
        struct ud_string *s = read_line();
        int a, b; trim_bounds(s->chars, s->length, &a, &b);
        int n = b - a; const char *p = s->chars + a;
        if ((n == 4 && strncmp(p, "true", 4) == 0) || (n == 1 && p[0] == '1')) PUSH(ud_bool(1));
        else if ((n == 5 && strncmp(p, "false", 5) == 0) || (n == 1 && p[0] == '0')) PUSH(ud_bool(0));
        else ud_error(UDE_CIN_BOOL, line,
                      "expected true/false/1/0 but got \"%.*s\"", s->length, s->chars);
        NEXT();
    }

    CASE(OP_TO_INT):   { int line = LINE(); struct ud_value v = POP();
                         PUSH(convert_to_int(v, line)); NEXT(); }
    CASE(OP_TO_FLOAT): { int line = LINE(); struct ud_value v = POP();
                         PUSH(convert_to_float(v, line)); NEXT(); }
    CASE(OP_TO_BOOL):  { int line = LINE(); struct ud_value v = POP();
                         PUSH(convert_to_bool(v, line)); NEXT(); }
    CASE(OP_TO_STRING):{ struct ud_value v = POP();
                         PUSH(ud_obj_val((struct ud_obj *)ud_value_to_string(v))); NEXT(); }

    CASE(OP_PRINT): {
        struct ud_value v = POP();
        struct ud_string *s = ud_value_to_string(v);
        fwrite(s->chars, 1, (size_t)s->length, stdout);
        fputc('\n', stdout);
        NEXT();
    }
    CASE(OP_HALT): { exit_code = 0; goto finished; }

#ifndef UD_USE_GOTO
        default:
            ud_error(UDE_BADBYTECODE, LINE(), "the bytecode contains an unknown instruction");
        }
    }
#endif

finished:
    free(vm.stack.base);
    return exit_code;

#undef READ_BYTE
#undef READ_U16
#undef READ_CONST
#undef LINE
#undef PUSH
#undef POP
#undef CASE
#undef NEXT
}
