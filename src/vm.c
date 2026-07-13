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
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#endif

#define FRAMES_MAX 220
#define STACK_MAX  (FRAMES_MAX * 256)
#define TRY_MAX    64

struct call_frame {
    struct ud_function *fn;
    uint8_t *ip;
    struct ud_value *slots;
};

/* One armed `try`. setjmp fills `jmp`; on an error ud_error()/ud_raise() longjmp
 * back to it and the VM rewinds frame_count/stack.top to the saved snapshot,
 * then jumps to `catch_ip`. `catch_slot` is the local that receives the error
 * value (or -1 when the catch clause names no variable). */
struct try_handler {
    jmp_buf          jmp;
    int              saved_frame_count;
    struct ud_value *saved_top;
    uint8_t         *catch_ip;
    int              catch_slot;
};

struct vm {
    struct ud_stack stack;
    struct call_frame frames[FRAMES_MAX];
    int frame_count;
    struct ud_map *globals;
    struct ud_program *prog;
    struct try_handler try_handlers[TRY_MAX];
    int try_count;
    struct ud_value pending_throw;   /* value parked by OP_THROW for the catch */
};

/* Keep the global catch landing pad in sync with the handler stack. */
static inline void vm_sync_catch(struct vm *vm) {
    ud_catch_current = vm->try_count > 0
                     ? &vm->try_handlers[vm->try_count - 1].jmp : NULL;
}

/* The interpreter core is re-entrant: `vm_execute` runs the dispatch loop until
 * the frame active on entry returns, and `vm_call_value` uses it to invoke a UD
 * function value from C (how map/filter/reduce/sort/foreach run their
 * callbacks). Forward-declared here so the higher-order builtins above the main
 * loop can reach them. */
static struct ud_value vm_execute(struct vm *vm, int base_frame);
static struct ud_value vm_call_value(struct vm *vm, struct ud_value callee,
                                     struct ud_value *argv, int argc, int line);

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
    if (UD_IS_DICT(v))   return UD_AS_DICT(v)->count;
    if (UD_IS_SET(v))    return UD_AS_SET(v)->backing->count;
    ud_error(UDE_TYPE, line, "len() needs an array, string, dict, or set, not a %s",
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
                case OBJ_MODULE: name = "module"; break;
                case OBJ_DICT: name = "dict"; break;
                case OBJ_SET: name = "set"; break;
            }
            break;
    }
    return ud_obj_val((struct ud_obj *)ud_str_from_cstr(name));
}

/* Helpers defined later in this file; used by the natives below. */
static void want_argc(int got, int want, const char *who, int line);
static struct ud_string *want_string(struct ud_value v, const char *who, int line);
static void *xmalloc(size_t n, int line);

static double want_number(struct ud_value v, const char *who, int line) {
    if (!UD_IS_NUM(v)) ud_error(UDE_TYPE, line, "%s expects a number", who);
    return ud_as_number(v);
}
static long long want_int(struct ud_value v, const char *who, int line) {
    if (!UD_IS_INT(v)) ud_error(UDE_TYPE, line, "%s expects a whole number", who);
    return v.as.i;
}

/* ------------------------------------------------------------------ */
/* File I/O + sleep + format                                          */
/* ------------------------------------------------------------------ */

static struct ud_value native_read_file(int argc, struct ud_value *args, int line) {
    want_argc(argc, 1, "read_file()", line);
    struct ud_string *path = want_string(args[0], "read_file()", line);
    FILE *f = fopen(path->chars, "rb");
    if (!f) ud_error(UDE_IO, line, "could not open '%s' for reading", path->chars);
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); ud_error(UDE_IO, line, "could not read '%s'", path->chars); }
    char *buf = (char *)xmalloc((size_t)sz + 1, line);
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    struct ud_string *s = ud_str_intern(buf, (int)got);
    free(buf);
    return ud_obj_val((struct ud_obj *)s);
}

static struct ud_value write_or_append(struct ud_value *args, const char *mode,
                                       const char *who, int line) {
    struct ud_string *path = want_string(args[0], who, line);
    struct ud_string *data = ud_value_to_string(args[1]);
    FILE *f = fopen(path->chars, mode);
    if (!f) ud_error(UDE_IO, line, "could not open '%s' for writing", path->chars);
    size_t wrote = fwrite(data->chars, 1, (size_t)data->length, f);
    int bad = ferror(f);
    fclose(f);
    if (bad || wrote != (size_t)data->length)
        ud_error(UDE_IO, line, "failed while writing '%s'", path->chars);
    return ud_bool(1);
}
static struct ud_value native_write_file(int argc, struct ud_value *args, int line) {
    want_argc(argc, 2, "write_file()", line);
    return write_or_append(args, "wb", "write_file()", line);
}
static struct ud_value native_append_file(int argc, struct ud_value *args, int line) {
    want_argc(argc, 2, "append_file()", line);
    return write_or_append(args, "ab", "append_file()", line);
}
static struct ud_value native_file_exists(int argc, struct ud_value *args, int line) {
    want_argc(argc, 1, "file_exists()", line);
    struct ud_string *path = want_string(args[0], "file_exists()", line);
    FILE *f = fopen(path->chars, "rb");
    if (f) { fclose(f); return ud_bool(1); }
    return ud_bool(0);
}

static struct ud_value native_sleep(int argc, struct ud_value *args, int line) {
    want_argc(argc, 1, "sleep()", line);
    double secs = want_number(args[0], "sleep()", line);
    if (secs < 0) secs = 0;
    fflush(stdout);
#ifdef _WIN32
    Sleep((DWORD)(secs * 1000.0));
#else
    struct timespec ts;
    ts.tv_sec = (time_t)secs;
    ts.tv_nsec = (long)((secs - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
#endif
    return ud_nil();
}

/* format("{} + {} = {}", a, b, a+b) -- {} substitutes args in order; {{ }} escape. */
static struct ud_value native_format(int argc, struct ud_value *args, int line) {
    if (argc < 1) ud_error(UDE_ARGCOUNT, line, "format() needs at least a format string");
    struct ud_string *fmt = want_string(args[0], "format()", line);
    int cap = fmt->length + 16, len = 0, argi = 1;
    char *buf = (char *)xmalloc((size_t)cap, line);
#define FMT_PUT(ch) do { if (len + 1 > cap) { cap *= 2; buf = (char *)realloc(buf, (size_t)cap); \
        if (!buf) ud_error(UDE_INTERNAL, line, "out of memory"); } buf[len++] = (ch); } while (0)
    for (int i = 0; i < fmt->length; i++) {
        char c = fmt->chars[i];
        if (c == '{' && i + 1 < fmt->length && fmt->chars[i + 1] == '{') { FMT_PUT('{'); i++; }
        else if (c == '}' && i + 1 < fmt->length && fmt->chars[i + 1] == '}') { FMT_PUT('}'); i++; }
        else if (c == '{' && i + 1 < fmt->length && fmt->chars[i + 1] == '}') {
            i++;
            struct ud_value a = argi < argc ? args[argi++] : ud_nil();
            struct ud_string *s = ud_value_to_string(a);
            for (int k = 0; k < s->length; k++) FMT_PUT(s->chars[k]);
        } else {
            FMT_PUT(c);
        }
    }
#undef FMT_PUT
    struct ud_string *out = ud_str_intern(buf, len);
    free(buf);
    return ud_obj_val((struct ud_obj *)out);
}

/* ------------------------------------------------------------------ */
/* math module                                                        */
/* ------------------------------------------------------------------ */

#define UD_PI  3.14159265358979323846
#define UD_E   2.71828182845904523536

#define MATH1(cname, cfun, who) \
    static struct ud_value cname(int argc, struct ud_value *args, int line) { \
        want_argc(argc, 1, who, line); \
        return ud_float(cfun(want_number(args[0], who, line))); \
    }
MATH1(m_sqrt, sqrt,  "math.sqrt()")
MATH1(m_sin,  sin,   "math.sin()")
MATH1(m_cos,  cos,   "math.cos()")
MATH1(m_tan,  tan,   "math.tan()")
MATH1(m_asin, asin,  "math.asin()")
MATH1(m_acos, acos,  "math.acos()")
MATH1(m_atan, atan,  "math.atan()")
MATH1(m_exp,  exp,   "math.exp()")
MATH1(m_log,  log,   "math.log()")
MATH1(m_log10,log10, "math.log10()")
MATH1(m_log2, log2,  "math.log2()")
MATH1(m_cbrt, cbrt,  "math.cbrt()")
#undef MATH1

#define MATHWHOLE(cname, cfun, who) \
    static struct ud_value cname(int argc, struct ud_value *args, int line) { \
        want_argc(argc, 1, who, line); \
        return ud_int((long long)cfun(want_number(args[0], who, line))); \
    }
MATHWHOLE(m_floor, floor, "math.floor()")
MATHWHOLE(m_ceil,  ceil,  "math.ceil()")
MATHWHOLE(m_trunc, trunc, "math.trunc()")
#undef MATHWHOLE

static struct ud_value m_round(int argc, struct ud_value *args, int line) {
    want_argc(argc, 1, "math.round()", line);
    return ud_int((long long)round(want_number(args[0], "math.round()", line)));
}
static struct ud_value m_abs(int argc, struct ud_value *args, int line) {
    want_argc(argc, 1, "math.abs()", line);
    if (UD_IS_INT(args[0])) return ud_int(args[0].as.i < 0 ? -args[0].as.i : args[0].as.i);
    double d = want_number(args[0], "math.abs()", line);
    return ud_float(d < 0 ? -d : d);
}
static struct ud_value m_sign(int argc, struct ud_value *args, int line) {
    want_argc(argc, 1, "math.sign()", line);
    double d = want_number(args[0], "math.sign()", line);
    return ud_int(d > 0 ? 1 : (d < 0 ? -1 : 0));
}
static struct ud_value m_pow(int argc, struct ud_value *args, int line) {
    want_argc(argc, 2, "math.pow()", line);
    return ud_float(pow(want_number(args[0], "math.pow()", line),
                        want_number(args[1], "math.pow()", line)));
}
static struct ud_value m_atan2(int argc, struct ud_value *args, int line) {
    want_argc(argc, 2, "math.atan2()", line);
    return ud_float(atan2(want_number(args[0], "math.atan2()", line),
                          want_number(args[1], "math.atan2()", line)));
}
static struct ud_value m_hypot(int argc, struct ud_value *args, int line) {
    want_argc(argc, 2, "math.hypot()", line);
    return ud_float(hypot(want_number(args[0], "math.hypot()", line),
                          want_number(args[1], "math.hypot()", line)));
}
static struct ud_value m_min(int argc, struct ud_value *args, int line) {
    if (argc < 1) ud_error(UDE_ARGCOUNT, line, "math.min() needs at least one number");
    struct ud_value best = args[0];
    for (int i = 1; i < argc; i++)
        if (want_number(args[i], "math.min()", line) < want_number(best, "math.min()", line))
            best = args[i];
    (void)want_number(best, "math.min()", line);
    return best;
}
static struct ud_value m_max(int argc, struct ud_value *args, int line) {
    if (argc < 1) ud_error(UDE_ARGCOUNT, line, "math.max() needs at least one number");
    struct ud_value best = args[0];
    for (int i = 1; i < argc; i++)
        if (want_number(args[i], "math.max()", line) > want_number(best, "math.max()", line))
            best = args[i];
    (void)want_number(best, "math.max()", line);
    return best;
}
static struct ud_value m_gcd(int argc, struct ud_value *args, int line) {
    want_argc(argc, 2, "math.gcd()", line);
    long long a = want_int(args[0], "math.gcd()", line);
    long long b = want_int(args[1], "math.gcd()", line);
    if (a < 0) a = -a; if (b < 0) b = -b;
    while (b) { long long t = a % b; a = b; b = t; }
    return ud_int(a);
}

/* ------------------------------------------------------------------ */
/* random module                                                      */
/* ------------------------------------------------------------------ */

static uint64_t rng_state = 0;
static int rng_seeded = 0;
static uint64_t rng_next(void) {
    if (!rng_seeded) {
        rng_state = (uint64_t)time(NULL) ^ ((uint64_t)(uintptr_t)&rng_state << 16) ^ 0x2545F4914F6CDD1Dull;
        rng_seeded = 1;
    }
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static double rng_double(void) { return (double)(rng_next() >> 11) * (1.0 / 9007199254740992.0); }

static struct ud_value r_seed(int argc, struct ud_value *args, int line) {
    want_argc(argc, 1, "random.seed()", line);
    rng_state = (uint64_t)want_int(args[0], "random.seed()", line);
    rng_seeded = 1;
    return ud_nil();
}
static struct ud_value r_float(int argc, struct ud_value *args, int line) {
    (void)args;
    if (argc != 0) ud_error(UDE_ARGCOUNT, line, "random.float() takes no arguments");
    return ud_float(rng_double());
}
static struct ud_value r_integer(int argc, struct ud_value *args, int line) {
    want_argc(argc, 2, "random.integer()", line);
    long long a = want_int(args[0], "random.integer()", line);
    long long b = want_int(args[1], "random.integer()", line);
    if (a > b) { long long t = a; a = b; b = t; }
    uint64_t span = (uint64_t)(b - a) + 1;
    return ud_int(a + (long long)(rng_next() % span));
}
static struct ud_value r_range(int argc, struct ud_value *args, int line) {
    want_argc(argc, 2, "random.range()", line);
    long long a = want_int(args[0], "random.range()", line);
    long long b = want_int(args[1], "random.range()", line);
    if (a >= b) ud_error(UDE_TYPE, line, "random.range(a, b) needs a < b");
    uint64_t span = (uint64_t)(b - a);
    return ud_int(a + (long long)(rng_next() % span));
}
static struct ud_value r_bool(int argc, struct ud_value *args, int line) {
    (void)args; (void)line; (void)argc;
    return ud_bool((int)(rng_next() & 1));
}
static struct ud_value r_choice(int argc, struct ud_value *args, int line) {
    want_argc(argc, 1, "random.choice()", line);
    if (!UD_IS_ARRAY(args[0])) ud_error(UDE_TYPE, line, "random.choice() needs an array");
    struct ud_array *a = UD_AS_ARRAY(args[0]);
    if (a->length == 0) ud_error(UDE_INDEX, line, "random.choice() got an empty array");
    return a->items[rng_next() % (uint64_t)a->length];
}
static struct ud_value r_shuffle(int argc, struct ud_value *args, int line) {
    want_argc(argc, 1, "random.shuffle()", line);
    if (!UD_IS_ARRAY(args[0])) ud_error(UDE_TYPE, line, "random.shuffle() needs an array");
    struct ud_array *a = UD_AS_ARRAY(args[0]);
    for (int i = a->length - 1; i > 0; i--) {
        int j = (int)(rng_next() % (uint64_t)(i + 1));
        struct ud_value t = a->items[i]; a->items[i] = a->items[j]; a->items[j] = t;
    }
    return args[0];
}

/* ------------------------------------------------------------------ */
/* Builtin registration                                               */
/* ------------------------------------------------------------------ */

static void mod_fn(struct ud_module *m, const char *name, int arity,
                   struct ud_value (*fn)(int, struct ud_value *, int)) {
    char full[64];
    snprintf(full, sizeof(full), "%s.%s", m->name->chars, name);
    ud_module_set(m, name, ud_obj_val((struct ud_obj *)
        ud_native_new(ud_str_from_cstr(full)->chars, arity, fn)));
}

/* ------------------------------------------------------------------ */
/* regex module -- compact backtracking engine                        */
/*                                                                    */
/* Supports: literals, . ^ $, char classes [a-z] [^...], the          */
/* shorthands \d \w \s \D \W \S, groups (...), alternation |, and the */
/* greedy/lazy quantifiers * + ? (append ? for lazy). Patterns are    */
/* parsed to a small AST, compiled to a split/jmp program, and run    */
/* with a recursive backtracking VM. Compiled per call, then freed.   */
/* ------------------------------------------------------------------ */

enum rx_kind { RX_CHAR, RX_ANY, RX_CLASS, RX_BOL, RX_EOL,
               RX_CONCAT, RX_ALT, RX_STAR, RX_PLUS, RX_QUEST, RX_EMPTY };

struct rx_node {
    int kind;
    unsigned char ch;        /* RX_CHAR */
    uint8_t bits[32]; int neg; /* RX_CLASS */
    struct rx_node *a, *b;   /* children */
    int lazy;                /* STAR/PLUS/QUEST */
};

struct rx_parser { const char *p, *end; int ok; };

static struct rx_node *rx_new(int kind) {
    struct rx_node *n = (struct rx_node *)calloc(1, sizeof *n);
    if (!n) ud_error(UDE_INTERNAL, 0, "out of memory compiling a regex");
    n->kind = kind;
    return n;
}
static void rx_free(struct rx_node *n) {
    if (!n) return;
    rx_free(n->a); rx_free(n->b);
    free(n);
}

static void rx_set_char(uint8_t bits[32], int c) { bits[(c & 0xff) >> 3] |= (uint8_t)(1 << (c & 7)); }
static void rx_set_range(uint8_t bits[32], int lo, int hi) { for (int c = lo; c <= hi; c++) rx_set_char(bits, c); }

/* Add a \d \w \s (or negated \D \W \S) shorthand's members into a class map. */
static void rx_add_shorthand(uint8_t bits[32], char c) {
    uint8_t tmp[32]; memset(tmp, 0, sizeof tmp);
    char base = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    switch (base) {
        case 'd': rx_set_range(tmp, '0', '9'); break;
        case 'w': rx_set_range(tmp, 'a', 'z'); rx_set_range(tmp, 'A', 'Z');
                  rx_set_range(tmp, '0', '9'); rx_set_char(tmp, '_'); break;
        case 's': rx_set_char(tmp, ' '); rx_set_char(tmp, '\t'); rx_set_char(tmp, '\n');
                  rx_set_char(tmp, '\r'); rx_set_char(tmp, '\f'); rx_set_char(tmp, '\v'); break;
        default: return;
    }
    int negate = (c >= 'A' && c <= 'Z');
    for (int i = 0; i < 256; i++) {
        int set = (tmp[i >> 3] >> (i & 7)) & 1;
        if (negate) set = !set;
        if (set) bits[i >> 3] |= (uint8_t)(1 << (i & 7));
    }
}
static int rx_is_shorthand(char c) {
    return c=='d'||c=='w'||c=='s'||c=='D'||c=='W'||c=='S';
}
/* Map an escaped literal like \n \t to its byte; most escapes are themselves. */
static char rx_escaped_char(char c) {
    switch (c) { case 'n': return '\n'; case 't': return '\t';
                 case 'r': return '\r'; case 'f': return '\f'; case 'v': return '\v';
                 case '0': return '\0'; default: return c; }
}

static struct rx_node *rx_parse_alt(struct rx_parser *ps);

static struct rx_node *rx_parse_atom(struct rx_parser *ps) {
    if (ps->p >= ps->end) return rx_new(RX_EMPTY);
    char c = *ps->p;
    if (c == '(') {
        ps->p++;
        struct rx_node *inner = rx_parse_alt(ps);
        if (ps->p < ps->end && *ps->p == ')') ps->p++;
        else ps->ok = 0;
        return inner;
    }
    if (c == '[') {
        ps->p++;
        struct rx_node *n = rx_new(RX_CLASS);
        if (ps->p < ps->end && *ps->p == '^') { n->neg = 1; ps->p++; }
        if (ps->p < ps->end && *ps->p == ']') { rx_set_char(n->bits, ']'); ps->p++; }
        while (ps->p < ps->end && *ps->p != ']') {
            if (*ps->p == '\\' && ps->p + 1 < ps->end) {
                char e = ps->p[1];
                if (rx_is_shorthand(e)) { rx_add_shorthand(n->bits, e); ps->p += 2; continue; }
                rx_set_char(n->bits, rx_escaped_char(e)); ps->p += 2; continue;
            }
            char lo = *ps->p;
            if (ps->p + 2 < ps->end && ps->p[1] == '-' && ps->p[2] != ']') {
                char hi = ps->p[2];
                rx_set_range(n->bits, (unsigned char)lo, (unsigned char)hi);
                ps->p += 3;
            } else { rx_set_char(n->bits, lo); ps->p++; }
        }
        if (ps->p < ps->end && *ps->p == ']') ps->p++; else ps->ok = 0;
        return n;
    }
    if (c == '.') { ps->p++; return rx_new(RX_ANY); }
    if (c == '^') { ps->p++; return rx_new(RX_BOL); }
    if (c == '$') { ps->p++; return rx_new(RX_EOL); }
    if (c == '\\' && ps->p + 1 < ps->end) {
        char e = ps->p[1];
        if (rx_is_shorthand(e)) {
            struct rx_node *n = rx_new(RX_CLASS);
            rx_add_shorthand(n->bits, e);
            ps->p += 2;
            return n;
        }
        struct rx_node *n = rx_new(RX_CHAR);
        n->ch = (unsigned char)rx_escaped_char(e);
        ps->p += 2;
        return n;
    }
    struct rx_node *n = rx_new(RX_CHAR);
    n->ch = (unsigned char)c;
    ps->p++;
    return n;
}

static struct rx_node *rx_parse_repeat(struct rx_parser *ps) {
    struct rx_node *atom = rx_parse_atom(ps);
    while (ps->p < ps->end && (*ps->p == '*' || *ps->p == '+' || *ps->p == '?')) {
        int kind = (*ps->p == '*') ? RX_STAR : (*ps->p == '+') ? RX_PLUS : RX_QUEST;
        ps->p++;
        struct rx_node *q = rx_new(kind);
        q->a = atom;
        if (ps->p < ps->end && *ps->p == '?') { q->lazy = 1; ps->p++; }
        atom = q;
    }
    return atom;
}

static int rx_at_concat_end(struct rx_parser *ps) {
    return ps->p >= ps->end || *ps->p == '|' || *ps->p == ')';
}

static struct rx_node *rx_parse_concat(struct rx_parser *ps) {
    if (rx_at_concat_end(ps)) return rx_new(RX_EMPTY);
    struct rx_node *left = rx_parse_repeat(ps);
    while (!rx_at_concat_end(ps)) {
        struct rx_node *right = rx_parse_repeat(ps);
        struct rx_node *cat = rx_new(RX_CONCAT);
        cat->a = left; cat->b = right;
        left = cat;
    }
    return left;
}

static struct rx_node *rx_parse_alt(struct rx_parser *ps) {
    struct rx_node *left = rx_parse_concat(ps);
    while (ps->p < ps->end && *ps->p == '|') {
        ps->p++;
        struct rx_node *right = rx_parse_concat(ps);
        struct rx_node *alt = rx_new(RX_ALT);
        alt->a = left; alt->b = right;
        left = alt;
    }
    return left;
}

enum { RI_CHAR, RI_ANY, RI_CLASS, RI_BOL, RI_EOL, RI_SPLIT, RI_JMP, RI_MATCH };
struct rx_inst { int op; unsigned char c; struct rx_node *cls; int x, y; };
struct rx_prog { struct rx_inst *code; int len, cap; };

static int rx_emit(struct rx_prog *pr, int op) {
    if (pr->len + 1 > pr->cap) {
        pr->cap = pr->cap < 16 ? 16 : pr->cap * 2;
        pr->code = (struct rx_inst *)realloc(pr->code, sizeof(struct rx_inst) * (size_t)pr->cap);
        if (!pr->code) ud_error(UDE_INTERNAL, 0, "out of memory compiling a regex");
    }
    int i = pr->len++;
    pr->code[i].op = op; pr->code[i].c = 0; pr->code[i].cls = NULL;
    pr->code[i].x = 0; pr->code[i].y = 0;
    return i;
}

static void rx_compile(struct rx_prog *pr, struct rx_node *n) {
    switch (n->kind) {
        case RX_EMPTY: break;
        case RX_CHAR:  { int i = rx_emit(pr, RI_CHAR); pr->code[i].c = n->ch; } break;
        case RX_ANY:   rx_emit(pr, RI_ANY); break;
        case RX_CLASS: { int i = rx_emit(pr, RI_CLASS); pr->code[i].cls = n; } break;
        case RX_BOL:   rx_emit(pr, RI_BOL); break;
        case RX_EOL:   rx_emit(pr, RI_EOL); break;
        case RX_CONCAT: rx_compile(pr, n->a); rx_compile(pr, n->b); break;
        case RX_ALT: {
            int sp = rx_emit(pr, RI_SPLIT);
            pr->code[sp].x = pr->len;
            rx_compile(pr, n->a);
            int jm = rx_emit(pr, RI_JMP);
            pr->code[sp].y = pr->len;
            rx_compile(pr, n->b);
            pr->code[jm].x = pr->len;
        } break;
        case RX_STAR: {
            int l1 = pr->len;
            int sp = rx_emit(pr, RI_SPLIT);
            pr->code[sp].x = pr->len;
            rx_compile(pr, n->a);
            int jm = rx_emit(pr, RI_JMP); pr->code[jm].x = l1;
            pr->code[sp].y = pr->len;
            if (n->lazy) { int t = pr->code[sp].x; pr->code[sp].x = pr->code[sp].y; pr->code[sp].y = t; }
        } break;
        case RX_PLUS: {
            int l1 = pr->len;
            rx_compile(pr, n->a);
            int sp = rx_emit(pr, RI_SPLIT);
            pr->code[sp].x = l1;
            pr->code[sp].y = pr->len;
            if (n->lazy) { int t = pr->code[sp].x; pr->code[sp].x = pr->code[sp].y; pr->code[sp].y = t; }
        } break;
        case RX_QUEST: {
            int sp = rx_emit(pr, RI_SPLIT);
            pr->code[sp].x = pr->len;
            rx_compile(pr, n->a);
            pr->code[sp].y = pr->len;
            if (n->lazy) { int t = pr->code[sp].x; pr->code[sp].x = pr->code[sp].y; pr->code[sp].y = t; }
        } break;
    }
}

static int rx_class_has(struct rx_node *n, unsigned char c) {
    int set = (n->bits[c >> 3] >> (c & 7)) & 1;
    return n->neg ? !set : set;
}

static long rx_steps;
static int rx_vm(struct rx_prog *pr, int pc, const char *s, int sp, int len, int *mend) {
    for (;;) {
        if (++rx_steps > 20000000L) return 0; /* runaway backtracking valve */
        struct rx_inst *in = &pr->code[pc];
        switch (in->op) {
            case RI_CHAR:  if (sp < len && (unsigned char)s[sp] == in->c) { pc++; sp++; break; } return 0;
            case RI_ANY:   if (sp < len) { pc++; sp++; break; } return 0;
            case RI_CLASS: if (sp < len && rx_class_has(in->cls, (unsigned char)s[sp])) { pc++; sp++; break; } return 0;
            case RI_BOL:   if (sp == 0) { pc++; break; } return 0;
            case RI_EOL:   if (sp == len) { pc++; break; } return 0;
            case RI_JMP:   pc = in->x; break;
            case RI_SPLIT: if (rx_vm(pr, in->x, s, sp, len, mend)) return 1; pc = in->y; break;
            case RI_MATCH: *mend = sp; return 1;
        }
    }
}

/* Compile `pat` into `pr` (+ trailing RI_MATCH). Returns the AST to free, or
 * NULL on a malformed pattern (in which case *pr is already cleaned up). */
static struct rx_node *rx_build(const char *pat, int patlen, struct rx_prog *pr) {
    struct rx_parser ps; ps.p = pat; ps.end = pat + patlen; ps.ok = 1;
    struct rx_node *root = rx_parse_alt(&ps);
    if (!ps.ok || ps.p != ps.end) { rx_free(root); return NULL; }
    pr->code = NULL; pr->len = 0; pr->cap = 0;
    rx_compile(pr, root);
    rx_emit(pr, RI_MATCH);
    return root;
}

/* Leftmost search from `from`; fills *ms/*me on success. */
static int rx_search(struct rx_prog *pr, const char *s, int len, int from, int *ms, int *me) {
    for (int start = from; start <= len; start++) {
        rx_steps = 0;
        if (rx_vm(pr, 0, s, start, len, me)) { *ms = start; return 1; }
    }
    return 0;
}

static struct rx_prog rx_compile_arg(struct ud_value v, const char *who, int line, struct rx_node **root) {
    struct ud_string *pat = want_string(v, who, line);
    struct rx_prog pr;
    *root = rx_build(pat->chars, pat->length, &pr);
    if (!*root) ud_error(UDE_TYPE, line, "%s got an invalid regex pattern", who);
    return pr;
}

static struct ud_value rx_test(int argc, struct ud_value *args, int line) {
    want_argc(argc, 2, "regex.test()", line);
    struct rx_node *root; struct rx_prog pr = rx_compile_arg(args[0], "regex.test()", line, &root);
    struct ud_string *txt = want_string(args[1], "regex.test()", line);
    int ms, me, hit = rx_search(&pr, txt->chars, txt->length, 0, &ms, &me);
    free(pr.code); rx_free(root);
    return ud_bool(hit);
}

static struct ud_value rx_match(int argc, struct ud_value *args, int line) {
    want_argc(argc, 2, "regex.match()", line);
    struct rx_node *root; struct rx_prog pr = rx_compile_arg(args[0], "regex.match()", line, &root);
    struct ud_string *txt = want_string(args[1], "regex.match()", line);
    int ms, me;
    struct ud_value r = ud_nil();
    if (rx_search(&pr, txt->chars, txt->length, 0, &ms, &me))
        r = ud_obj_val((struct ud_obj *)ud_str_intern(txt->chars + ms, me - ms));
    free(pr.code); rx_free(root);
    return r;
}

static struct ud_value rx_find_all(int argc, struct ud_value *args, int line) {
    want_argc(argc, 2, "regex.find_all()", line);
    struct rx_node *root; struct rx_prog pr = rx_compile_arg(args[0], "regex.find_all()", line, &root);
    struct ud_string *txt = want_string(args[1], "regex.find_all()", line);
    struct ud_array *out = ud_array_new(4);
    int pos = 0, ms, me;
    while (pos <= txt->length && rx_search(&pr, txt->chars, txt->length, pos, &ms, &me)) {
        ud_array_push(out, ud_obj_val((struct ud_obj *)ud_str_intern(txt->chars + ms, me - ms)));
        pos = (me > ms) ? me : ms + 1;
    }
    free(pr.code); rx_free(root);
    return ud_obj_val((struct ud_obj *)out);
}

static struct ud_value rx_replace(int argc, struct ud_value *args, int line) {
    want_argc(argc, 3, "regex.replace()", line);
    struct rx_node *root; struct rx_prog pr = rx_compile_arg(args[0], "regex.replace()", line, &root);
    struct ud_string *txt = want_string(args[1], "regex.replace()", line);
    struct ud_string *rep = want_string(args[2], "regex.replace()", line);
    int cap = txt->length + 16, len = 0;
    char *buf = (char *)xmalloc((size_t)cap, line);
#define RPUT(ch) do { if (len + 1 > cap) { cap *= 2; buf = (char *)realloc(buf, (size_t)cap); \
        if (!buf) ud_error(UDE_INTERNAL, line, "out of memory"); } buf[len++] = (ch); } while (0)
    int pos = 0, ms, me;
    while (pos <= txt->length && rx_search(&pr, txt->chars, txt->length, pos, &ms, &me)) {
        for (int k = pos; k < ms; k++) RPUT(txt->chars[k]);
        for (int k = 0; k < rep->length; k++) RPUT(rep->chars[k]);
        if (me > ms) pos = me;
        else { if (ms < txt->length) RPUT(txt->chars[ms]); pos = ms + 1; }
    }
    for (int k = pos; k < txt->length; k++) RPUT(txt->chars[k]);
#undef RPUT
    struct ud_string *out = ud_str_intern(buf, len);
    free(buf);
    free(pr.code); rx_free(root);
    return ud_obj_val((struct ud_obj *)out);
}

void ud_register_builtins(struct ud_program *prog) {
    struct { const char *name; int arity;
             struct ud_value (*fn)(int, struct ud_value *, int); } b[] = {
        {"cout", -1, native_cout},
        {"len",   1, native_len},
        {"type",  1, native_type},
        {"read_file",   1, native_read_file},
        {"write_file",  2, native_write_file},
        {"append_file", 2, native_append_file},
        {"file_exists", 1, native_file_exists},
        {"sleep",       1, native_sleep},
        {"format",     -1, native_format},
        {NULL, 0, NULL}
    };
    for (int i = 0; b[i].name; i++) {
        struct ud_native *n = ud_native_new(b[i].name, b[i].arity, b[i].fn);
        ud_map_set(&prog->globals, ud_str_from_cstr(b[i].name),
                   ud_obj_val((struct ud_obj *)n));
    }

    /* math module */
    struct ud_module *math = ud_module_new(ud_str_from_cstr("math"));
    ud_module_set(math, "pi",  ud_float(UD_PI));
    ud_module_set(math, "e",   ud_float(UD_E));
    ud_module_set(math, "tau", ud_float(2.0 * UD_PI));
    ud_module_set(math, "inf", ud_float(HUGE_VAL));
    mod_fn(math, "sqrt", 1, m_sqrt);   mod_fn(math, "abs", 1, m_abs);
    mod_fn(math, "floor", 1, m_floor); mod_fn(math, "ceil", 1, m_ceil);
    mod_fn(math, "round", 1, m_round); mod_fn(math, "trunc", 1, m_trunc);
    mod_fn(math, "sin", 1, m_sin);     mod_fn(math, "cos", 1, m_cos);
    mod_fn(math, "tan", 1, m_tan);     mod_fn(math, "asin", 1, m_asin);
    mod_fn(math, "acos", 1, m_acos);   mod_fn(math, "atan", 1, m_atan);
    mod_fn(math, "atan2", 2, m_atan2); mod_fn(math, "hypot", 2, m_hypot);
    mod_fn(math, "exp", 1, m_exp);     mod_fn(math, "log", 1, m_log);
    mod_fn(math, "log10", 1, m_log10); mod_fn(math, "log2", 1, m_log2);
    mod_fn(math, "cbrt", 1, m_cbrt);   mod_fn(math, "pow", 2, m_pow);
    mod_fn(math, "sign", 1, m_sign);   mod_fn(math, "gcd", 2, m_gcd);
    mod_fn(math, "min", -1, m_min);    mod_fn(math, "max", -1, m_max);
    ud_map_set(&prog->globals, ud_str_from_cstr("math"),
               ud_obj_val((struct ud_obj *)math));

    /* random module */
    struct ud_module *random = ud_module_new(ud_str_from_cstr("random"));
    mod_fn(random, "integer", 2, r_integer); mod_fn(random, "float", 0, r_float);
    mod_fn(random, "range", 2, r_range);     mod_fn(random, "choice", 1, r_choice);
    mod_fn(random, "bool", 0, r_bool);       mod_fn(random, "seed", 1, r_seed);
    mod_fn(random, "shuffle", 1, r_shuffle);
    ud_map_set(&prog->globals, ud_str_from_cstr("random"),
               ud_obj_val((struct ud_obj *)random));

    /* regex module */
    struct ud_module *regex = ud_module_new(ud_str_from_cstr("regex"));
    mod_fn(regex, "test", 2, rx_test);       mod_fn(regex, "match", 2, rx_match);
    mod_fn(regex, "find_all", 2, rx_find_all); mod_fn(regex, "replace", 3, rx_replace);
    ud_map_set(&prog->globals, ud_str_from_cstr("regex"),
               ud_obj_val((struct ud_obj *)regex));
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
        case OBJ_MODULE:    return "module";
        case OBJ_DICT:      return "dict";
        case OBJ_SET:       return "set";
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

/* Default ordering for sort(): ascending numbers, or lexicographic strings.
 * Anything else needs an explicit comparator. Returns <0, 0 or >0. */
static int value_order(struct ud_value x, struct ud_value y, int line) {
    if (UD_IS_NUM(x) && UD_IS_NUM(y)) {
        double dx = ud_as_number(x), dy = ud_as_number(y);
        return dx < dy ? -1 : dx > dy ? 1 : 0;
    }
    if (UD_IS_STRING(x) && UD_IS_STRING(y)) {
        int c = strcmp(UD_AS_STRING(x)->chars, UD_AS_STRING(y)->chars);
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    ud_error(UDE_TYPE, line,
             "sort() orders numbers or strings by default; pass a comparator to "
             "order %s values", human_type(x));
    return 0;
}

/* One comparison for sort(): the default order, or a UD comparator that returns
 * a number (<0 means x before y) or a bool (true means x before y). */
static int sort_cmp(struct vm *vm, struct ud_value cmp, struct ud_value x,
                    struct ud_value y, int line) {
    if (UD_IS_NIL(cmp)) return value_order(x, y, line);
    struct ud_value pair[2]; pair[0] = x; pair[1] = y;
    struct ud_value r = vm_call_value(vm, cmp, pair, 2, line);
    if (UD_IS_NUM(r)) { double d = ud_as_number(r); return d < 0 ? -1 : d > 0 ? 1 : 0; }
    if (UD_IS_BOOL(r)) return r.as.b ? -1 : 1;
    ud_error(UDE_TYPE, line,
             "a sort comparator must return a number (or a bool), not a %s",
             human_type(r));
    return 0;
}

/* Stable bottom-up-friendly merge sort. Comparisons may re-enter the VM, so we
 * cannot use qsort (no context) -- and stability keeps user comparators sane. */
static void merge_sort(struct vm *vm, struct ud_value cmp, struct ud_value *arr,
                       struct ud_value *tmp, int lo, int hi, int line) {
    if (hi - lo < 2) return;
    int mid = lo + (hi - lo) / 2;
    merge_sort(vm, cmp, arr, tmp, lo, mid, line);
    merge_sort(vm, cmp, arr, tmp, mid, hi, line);
    int i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        if (sort_cmp(vm, cmp, arr[j], arr[i], line) < 0) tmp[k++] = arr[j++];
        else                                             tmp[k++] = arr[i++];
    }
    while (i < mid) tmp[k++] = arr[i++];
    while (j < hi)  tmp[k++] = arr[j++];
    for (int t = lo; t < hi; t++) arr[t] = tmp[t];
}

static void sort_array(struct vm *vm, struct ud_array *a, struct ud_value cmp, int line) {
    if (a->length < 2) return;
    struct ud_value *tmp =
        (struct ud_value *)ud_alloc(sizeof(struct ud_value) * (size_t)a->length);
    merge_sort(vm, cmp, a->items, tmp, 0, a->length, line);
}

static struct ud_value arr_method(struct vm *vm, struct ud_array *a,
                                  struct ud_string *name,
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
    /* Higher-order methods -- each callback re-enters the VM via vm_call_value.
     * Array length is snapshotted so a callback that appends can't loop forever. */
    if (strcmp(m, "map") == 0) { want_argc(argc, 1, "map()", line);
        int n = a->length;
        struct ud_array *out = ud_array_new(n > 0 ? n : 4);
        for (int i = 0; i < n; i++) {
            struct ud_value el = a->items[i];
            ud_array_push(out, vm_call_value(vm, args[0], &el, 1, line));
        }
        return ud_obj_val((struct ud_obj *)out); }
    if (strcmp(m, "filter") == 0) { want_argc(argc, 1, "filter()", line);
        int n = a->length;
        struct ud_array *out = ud_array_new(4);
        for (int i = 0; i < n; i++) {
            struct ud_value el = a->items[i];
            if (ud_value_truthy(vm_call_value(vm, args[0], &el, 1, line)))
                ud_array_push(out, el);
        }
        return ud_obj_val((struct ud_obj *)out); }
    if (strcmp(m, "reduce") == 0) {
        if (argc != 1 && argc != 2)
            ud_error(UDE_ARGCOUNT, line, "reduce() expects 1 or 2 arguments but got %d", argc);
        int n = a->length, start;
        struct ud_value acc;
        if (argc == 2) { acc = args[1]; start = 0; }
        else {
            if (n == 0) ud_error(UDE_INDEX, line,
                "reduce() on an empty array needs a starting value");
            acc = a->items[0]; start = 1;
        }
        for (int i = start; i < n; i++) {
            struct ud_value pair[2]; pair[0] = acc; pair[1] = a->items[i];
            acc = vm_call_value(vm, args[0], pair, 2, line);
        }
        return acc; }
    if (strcmp(m, "foreach") == 0 || strcmp(m, "each") == 0) {
        want_argc(argc, 1, "foreach()", line);
        int n = a->length;
        for (int i = 0; i < n; i++) {
            struct ud_value el = a->items[i];
            vm_call_value(vm, args[0], &el, 1, line);
        }
        return ud_nil(); }
    if (strcmp(m, "sort") == 0) {
        if (argc != 0 && argc != 1)
            ud_error(UDE_ARGCOUNT, line, "sort() expects 0 or 1 arguments but got %d", argc);
        int n = a->length;
        struct ud_array *out = ud_array_new(n > 0 ? n : 4);
        for (int i = 0; i < n; i++) ud_array_push(out, a->items[i]);
        sort_array(vm, out, argc == 1 ? args[0] : ud_nil(), line);
        return ud_obj_val((struct ud_obj *)out); }
    ud_error(UDE_ATTR, line, "arrays have no method '%s'", m);
    return ud_nil();
}

static struct ud_value dict_method(struct ud_dict *d, struct ud_string *name,
                                   int argc, struct ud_value *args, int line) {
    const char *m = name->chars;
    if (strcmp(m, "keys") == 0) { want_argc(argc, 0, "keys()", line);
        struct ud_array *a = ud_array_new(d->count > 0 ? d->count : 4);
        for (int i = 0; i < d->order_count; i++) { struct ud_value k = d->order[i];
            if (ud_dict_get(d, k, NULL)) ud_array_push(a, k); }
        return ud_obj_val((struct ud_obj *)a); }
    if (strcmp(m, "values") == 0) { want_argc(argc, 0, "values()", line);
        struct ud_array *a = ud_array_new(d->count > 0 ? d->count : 4);
        for (int i = 0; i < d->order_count; i++) { struct ud_value k = d->order[i], v;
            if (ud_dict_get(d, k, &v)) ud_array_push(a, v); }
        return ud_obj_val((struct ud_obj *)a); }
    if (strcmp(m, "has") == 0 || strcmp(m, "contains") == 0) {
        want_argc(argc, 1, "has()", line);
        return ud_bool(ud_dict_get(d, args[0], NULL)); }
    if (strcmp(m, "get") == 0) {
        if (argc != 1 && argc != 2)
            ud_error(UDE_ARGCOUNT, line, "get() expects 1 or 2 arguments but got %d", argc);
        struct ud_value v;
        if (ud_dict_get(d, args[0], &v)) return v;
        return argc == 2 ? args[1] : ud_nil(); }
    if (strcmp(m, "set") == 0) { want_argc(argc, 2, "set()", line);
        ud_dict_set(d, args[0], args[1]); return ud_nil(); }
    if (strcmp(m, "remove") == 0 || strcmp(m, "delete") == 0) {
        want_argc(argc, 1, "remove()", line);
        return ud_bool(ud_dict_remove(d, args[0])); }
    if (strcmp(m, "length") == 0 || strcmp(m, "len") == 0) {
        want_argc(argc, 0, "length()", line); return ud_int(d->count); }
    ud_error(UDE_ATTR, line, "dicts have no method '%s'", m);
    return ud_nil();
}

static struct ud_value set_method(struct ud_set *s, struct ud_string *name,
                                  int argc, struct ud_value *args, int line) {
    const char *m = name->chars;
    struct ud_dict *d = s->backing;
    if (strcmp(m, "add") == 0) { want_argc(argc, 1, "add()", line);
        ud_set_add(s, args[0]); return ud_nil(); }
    if (strcmp(m, "has") == 0 || strcmp(m, "contains") == 0) {
        want_argc(argc, 1, "has()", line);
        return ud_bool(ud_set_has(s, args[0])); }
    if (strcmp(m, "remove") == 0 || strcmp(m, "delete") == 0) {
        want_argc(argc, 1, "remove()", line);
        return ud_bool(ud_set_remove(s, args[0])); }
    if (strcmp(m, "values") == 0 || strcmp(m, "items") == 0) {
        want_argc(argc, 0, "values()", line);
        struct ud_array *a = ud_array_new(d->count > 0 ? d->count : 4);
        for (int i = 0; i < d->order_count; i++) { struct ud_value k = d->order[i];
            if (ud_dict_get(d, k, NULL)) ud_array_push(a, k); }
        return ud_obj_val((struct ud_obj *)a); }
    if (strcmp(m, "length") == 0 || strcmp(m, "len") == 0) {
        want_argc(argc, 0, "length()", line); return ud_int(d->count); }
    ud_error(UDE_ATTR, line, "sets have no method '%s'", m);
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
    struct vm vmst;
    struct vm *vm = &vmst;
    ud_stack_init(&vm->stack, STACK_MAX);
    vm->frame_count = 0;
    vm->globals = &prog->globals;
    vm->prog = prog;
    vm->try_count = 0;
    vm->pending_throw = ud_nil();
    ud_catch_current = NULL;

    struct ud_function *entry = prog->entry;

    /* Seed the entry frame. The callee sits one slot below its locals so the
     * uniform "slots = top - argc" layout works for entry() too. */
    ud_push(&vm->stack, ud_obj_val((struct ud_obj *)entry));
    struct call_frame *f0 = &vm->frames[vm->frame_count++];
    f0->fn = entry;
    f0->ip = entry->code;
    f0->slots = vm->stack.top;

    /* Run the whole program, then turn entry()'s return into the exit code. */
    struct ud_value result = vm_execute(vm, 0);
    int exit_code = UD_IS_INT(result) ? (int)result.as.i
                  : UD_IS_BOOL(result) ? result.as.b : 0;
    free(vm->stack.base);
    return exit_code;
}

static struct ud_value vm_execute(struct vm *vm, int base_frame) {
    struct call_frame *frame = &vm->frames[vm->frame_count - 1];

#define READ_BYTE()  (*frame->ip++)
#define READ_U16()   (frame->ip += 2, \
                      (int)((uint16_t)frame->ip[-2] | ((uint16_t)frame->ip[-1] << 8)))
#define READ_CONST() (frame->fn->consts[READ_U16()])
#define LINE()       (frame->fn->lines[(int)((frame->ip - 1) - frame->fn->code)])
#define PUSH(v)      ud_push(&vm->stack, (v))
#define POP()        ud_pop(&vm->stack)

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
        [OP_IN]=&&L_OP_IN,
        [OP_NOT]=&&L_OP_NOT, [OP_BAND]=&&L_OP_BAND, [OP_BOR]=&&L_OP_BOR,
        [OP_BXOR]=&&L_OP_BXOR, [OP_BNOT]=&&L_OP_BNOT, [OP_SHL]=&&L_OP_SHL,
        [OP_SHR]=&&L_OP_SHR, [OP_CONCAT]=&&L_OP_CONCAT, [OP_LEN]=&&L_OP_LEN,
        [OP_JUMP]=&&L_OP_JUMP, [OP_JUMP_IF_FALSE]=&&L_OP_JUMP_IF_FALSE,
        [OP_JUMP_IF_TRUE]=&&L_OP_JUMP_IF_TRUE, [OP_LOOP]=&&L_OP_LOOP,
        [OP_CALL]=&&L_OP_CALL, [OP_RETURN]=&&L_OP_RETURN, [OP_ARRAY]=&&L_OP_ARRAY,
        [OP_DICT]=&&L_OP_DICT, [OP_SET]=&&L_OP_SET, [OP_ITER_KEYS]=&&L_OP_ITER_KEYS,
        [OP_INDEX_GET]=&&L_OP_INDEX_GET, [OP_INDEX_SET]=&&L_OP_INDEX_SET,
        [OP_SLICE]=&&L_OP_SLICE, [OP_INVOKE]=&&L_OP_INVOKE,
        [OP_STRUCT_NEW]=&&L_OP_STRUCT_NEW, [OP_FIELD_GET]=&&L_OP_FIELD_GET,
        [OP_FIELD_SET]=&&L_OP_FIELD_SET, [OP_CIN]=&&L_OP_CIN,
        [OP_CIN_INT]=&&L_OP_CIN_INT, [OP_CIN_FLOAT]=&&L_OP_CIN_FLOAT,
        [OP_CIN_BOOL]=&&L_OP_CIN_BOOL, [OP_TO_INT]=&&L_OP_TO_INT,
        [OP_TO_FLOAT]=&&L_OP_TO_FLOAT, [OP_TO_BOOL]=&&L_OP_TO_BOOL,
        [OP_TO_STRING]=&&L_OP_TO_STRING,
        [OP_TRY]=&&L_OP_TRY, [OP_POP_TRY]=&&L_OP_POP_TRY, [OP_THROW]=&&L_OP_THROW,
        [OP_PRINT]=&&L_OP_PRINT,
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
    CASE(OP_DUP):     PUSH(ud_peek(&vm->stack, 0)); NEXT();

    CASE(OP_GET_LOCAL): { uint8_t slot = READ_BYTE(); PUSH(frame->slots[slot]); NEXT(); }
    CASE(OP_SET_LOCAL): { uint8_t slot = READ_BYTE();
                          frame->slots[slot] = ud_peek(&vm->stack, 0); NEXT(); }

    CASE(OP_GET_GLOBAL): {
        int line = LINE();
        struct ud_string *name = UD_AS_STRING(READ_CONST());
        struct ud_value out;
        if (!ud_map_get(vm->globals, name, &out))
            ud_error(UDE_UNDEF_VAR, line, "nothing named '%s' is defined", name->chars);
        PUSH(out); NEXT();
    }
    CASE(OP_SET_GLOBAL): {
        struct ud_string *name = UD_AS_STRING(READ_CONST());
        ud_map_set(vm->globals, name, ud_peek(&vm->stack, 0));
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

    CASE(OP_IN): {
        int line = LINE();
        struct ud_value cont = POP(), item = POP();
        int found = 0;
        if (UD_IS_ARRAY(cont)) {
            struct ud_array *a = UD_AS_ARRAY(cont);
            for (int i = 0; i < a->length; i++)
                if (ud_value_equal(item, a->items[i])) { found = 1; break; }
        } else if (UD_IS_DICT(cont)) {
            struct ud_value tmp;
            found = ud_dict_get(UD_AS_DICT(cont), item, &tmp);
        } else if (UD_IS_SET(cont)) {
            found = ud_set_has(UD_AS_SET(cont), item);
        } else if (UD_IS_STRING(cont) && UD_IS_STRING(item)) {
            struct ud_string *hay = UD_AS_STRING(cont), *needle = UD_AS_STRING(item);
            if (needle->length == 0) found = 1;
            else for (int i = 0; i + needle->length <= hay->length; i++)
                if (memcmp(hay->chars + i, needle->chars, (size_t)needle->length) == 0) { found = 1; break; }
        } else {
            ud_error(UDE_TYPE, line,
                     "'in' needs an array, dict, set, or string on the right");
        }
        PUSH(ud_bool(found));
        NEXT();
    }

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
        struct ud_value callee = ud_peek(&vm->stack, argc);
        if (UD_IS_FUNCTION(callee)) {
            struct ud_function *fn = UD_AS_FUNCTION(callee);
            if (argc != fn->arity)
                ud_error(UDE_ARGCOUNT, line, "%s() expects %d argument%s but got %d",
                         fn->name ? fn->name->chars : "?", fn->arity,
                         fn->arity == 1 ? "" : "s", argc);
            if (vm->frame_count >= FRAMES_MAX)
                ud_error(UDE_STACKOVERFLOW, line,
                         "too many nested calls (runaway recursion?)");
            struct ud_value *slots = vm->stack.top - argc;
            if (fn->param_types)
                for (int i = 0; i < argc; i++)
                    if (fn->param_types[i] != DT_NONE)
                        slots[i] = coerce_decl(slots[i], fn->param_types[i], line);
            if (slots + fn->num_slots + 8 > vm->stack.base + vm->stack.cap)
                ud_error(UDE_STACKOVERFLOW, line, "the value stack overflowed");
            frame = &vm->frames[vm->frame_count++];
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
            struct ud_value *args = vm->stack.top - argc;
            struct ud_value r = nat->fn(argc, args, line);
            vm->stack.top -= (argc + 1);
            PUSH(r);
            NEXT();
        }
        if (UD_IS_STRUCTDEF(callee)) {
            struct ud_value *args = vm->stack.top - argc;
            struct ud_value s = construct_struct(UD_AS_STRUCTDEF(callee), args, argc, line);
            vm->stack.top -= (argc + 1);
            PUSH(s);
            NEXT();
        }
        ud_error(UDE_NOT_CALLABLE, line,
                 "you tried to call a %s, which is not a function", human_type(callee));
        NEXT();
    }

    CASE(OP_RETURN): {
        struct ud_value result = POP();
        vm->frame_count--;
        /* a `return` may jump out of an armed try: disarm every handler that
         * belonged to the frame we are leaving so it never fires stale. */
        if (vm->try_count > 0 &&
            vm->try_handlers[vm->try_count - 1].saved_frame_count > vm->frame_count) {
            while (vm->try_count > 0 &&
                   vm->try_handlers[vm->try_count - 1].saved_frame_count > vm->frame_count)
                vm->try_count--;
            vm_sync_catch(vm);
        }
        vm->stack.top = frame->slots - 1; /* drop callee + args + locals */
        if (vm->frame_count == base_frame)
            return result;                /* hand back to ud_vm_run / vm_call_value */
        PUSH(result);
        frame = &vm->frames[vm->frame_count - 1];
        NEXT();
    }

    CASE(OP_ARRAY): {
        int count = READ_U16();
        struct ud_array *arr = ud_array_new(count > 0 ? count : 4);
        struct ud_value *src = vm->stack.top - count;
        for (int i = 0; i < count; i++) ud_array_push(arr, src[i]);
        vm->stack.top -= count;
        PUSH(ud_obj_val((struct ud_obj *)arr));
        NEXT();
    }

    CASE(OP_DICT): {
        int count = READ_U16();                 /* number of key/val pairs */
        struct ud_dict *d = ud_dict_new();
        struct ud_value *src = vm->stack.top - count * 2;
        for (int i = 0; i < count; i++)
            ud_dict_set(d, src[i * 2], src[i * 2 + 1]);
        vm->stack.top -= count * 2;
        PUSH(ud_obj_val((struct ud_obj *)d));
        NEXT();
    }

    CASE(OP_SET): {
        int count = READ_U16();
        struct ud_set *s = ud_set_new();
        struct ud_value *src = vm->stack.top - count;
        for (int i = 0; i < count; i++) ud_set_add(s, src[i]);
        vm->stack.top -= count;
        PUSH(ud_obj_val((struct ud_obj *)s));
        NEXT();
    }

    CASE(OP_ITER_KEYS): {
        int line = LINE();
        struct ud_value it = ud_peek(&vm->stack, 0);
        if (UD_IS_ARRAY(it) || UD_IS_STRING(it)) { NEXT(); } /* iterate as-is */
        if (UD_IS_DICT(it)) {
            struct ud_dict *d = UD_AS_DICT(it);
            struct ud_array *arr = ud_array_new(d->count > 0 ? d->count : 4);
            for (int i = 0; i < d->order_count; i++) {
                struct ud_value k = d->order[i];
                if (!ud_dict_get(d, k, NULL)) continue; /* skip removed keys */
                ud_array_push(arr, k);
            }
            (void)POP();
            PUSH(ud_obj_val((struct ud_obj *)arr));
            NEXT();
        }
        if (UD_IS_SET(it)) {
            struct ud_dict *d = UD_AS_SET(it)->backing;
            struct ud_array *arr = ud_array_new(d->count > 0 ? d->count : 4);
            for (int i = 0; i < d->order_count; i++) {
                struct ud_value k = d->order[i];
                if (!ud_dict_get(d, k, NULL)) continue;
                ud_array_push(arr, k);
            }
            (void)POP();
            PUSH(ud_obj_val((struct ud_obj *)arr));
            NEXT();
        }
        ud_error(UDE_TYPE, line, "cannot iterate over a %s with for-in",
                 human_type(it));
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
        } else if (UD_IS_DICT(cont)) {
            struct ud_value out;
            if (ud_dict_get(UD_AS_DICT(cont), idx, &out)) PUSH(out);
            else PUSH(ud_nil());
        } else {
            ud_error(UDE_NOT_INDEXABLE, line,
                     "you can only index arrays, strings, and dicts, not a %s", human_type(cont));
        }
        NEXT();
    }
    CASE(OP_INDEX_SET): {
        int line = LINE();
        struct ud_value val = POP(), idx = POP(), cont = POP();
        if (UD_IS_DICT(cont)) {
            ud_dict_set(UD_AS_DICT(cont), idx, val);
            PUSH(val);
            NEXT();
        }
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
        else { ud_error(UDE_NOT_INDEXABLE, line, "you can only slice arrays and strings"); return ud_nil(); }
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
        struct ud_value target = ud_peek(&vm->stack, argc);
        struct ud_value *args = vm->stack.top - argc;
        struct ud_value r;
        if (UD_IS_STRING(target))      r = str_method(UD_AS_STRING(target), mname, argc, args, line);
        else if (UD_IS_ARRAY(target))  r = arr_method(vm, UD_AS_ARRAY(target), mname, argc, args, line);
        else if (UD_IS_DICT(target))   r = dict_method(UD_AS_DICT(target), mname, argc, args, line);
        else if (UD_IS_SET(target))    r = set_method(UD_AS_SET(target), mname, argc, args, line);
        else if (UD_IS_MODULE(target)) {
            struct ud_module *mod = UD_AS_MODULE(target);
            struct ud_value fnv;
            if (!ud_map_get(&mod->members, mname, &fnv) || !UD_IS_NATIVE(fnv))
                ud_error(UDE_ATTR, line, "module %s has no function '%s'",
                         mod->name->chars, mname->chars);
            struct ud_native *nat = UD_AS_NATIVE(fnv);
            if (nat->arity >= 0 && argc != nat->arity)
                ud_error(UDE_ARGCOUNT, line, "%s() expects %d argument%s but got %d",
                         nat->name, nat->arity, nat->arity == 1 ? "" : "s", argc);
            r = nat->fn(argc, args, line);
        }
        else ud_error(UDE_ATTR, line, "a %s has no method '%s'", human_type(target), mname->chars);
        vm->stack.top -= (argc + 1);
        PUSH(r);
        NEXT();
    }
    CASE(OP_STRUCT_NEW): {
        int line = LINE();
        struct ud_string *sname = UD_AS_STRING(READ_CONST());
        uint8_t argc = READ_BYTE();
        struct ud_value defv;
        if (!ud_map_get(vm->globals, sname, &defv) || !UD_IS_STRUCTDEF(defv))
            ud_error(UDE_STRUCT, line, "no struct named '%s'", sname->chars);
        struct ud_value *args = vm->stack.top - argc;
        struct ud_value s = construct_struct(UD_AS_STRUCTDEF(defv), args, argc, line);
        vm->stack.top -= argc;
        PUSH(s);
        NEXT();
    }
    CASE(OP_FIELD_GET): {
        int line = LINE();
        struct ud_string *fname = UD_AS_STRING(READ_CONST());
        struct ud_value target = POP();
        if (UD_IS_MODULE(target)) {
            struct ud_module *mod = UD_AS_MODULE(target);
            struct ud_value out;
            if (!ud_map_get(&mod->members, fname, &out))
                ud_error(UDE_ATTR, line, "module %s has no member '%s'",
                         mod->name->chars, fname->chars);
            PUSH(out);
            NEXT();
        }
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

    CASE(OP_TRY): {
        uint8_t slot = READ_BYTE();
        int catch_off = READ_U16();       /* frame->ip now at the try body */
        if (vm->try_count >= TRY_MAX)
            ud_error(UDE_STACKOVERFLOW, LINE(), "too many nested try blocks");
        struct try_handler *h = &vm->try_handlers[vm->try_count++];
        h->saved_frame_count = vm->frame_count;
        h->saved_top         = vm->stack.top;
        h->catch_ip          = frame->ip + catch_off;
        h->catch_slot        = (slot == 0xFF) ? -1 : (int)slot;
        vm_sync_catch(vm);
        if (setjmp(h->jmp) != 0) {
            /* an error unwound into this handler (locals may be clobbered:
             * everything we need is reloaded from the vm struct below). */
            struct try_handler *hh = &vm->try_handlers[vm->try_count - 1];
            struct ud_value errval;
            if (ud_pending_error.has_value)
                errval = vm->pending_throw;
            else
                errval = ud_obj_val((struct ud_obj *)
                                    ud_str_from_cstr(ud_pending_error.message));
            vm->try_count--;              /* disarm before running the handler */
            vm_sync_catch(vm);
            vm->frame_count = hh->saved_frame_count;
            vm->stack.top   = hh->saved_top;
            frame = &vm->frames[vm->frame_count - 1];
            frame->ip = hh->catch_ip;
            PUSH(errval);                 /* becomes the catch variable (or is
                                           * popped when none was named) */
            NEXT();
        }
        NEXT();                           /* first pass: fall into the try body */
    }
    CASE(OP_POP_TRY): {
        if (vm->try_count > 0) vm->try_count--;
        vm_sync_catch(vm);
        NEXT();
    }
    CASE(OP_THROW): {
        int line = LINE();
        struct ud_value v = POP();
        vm->pending_throw = v;
        struct ud_string *s = ud_value_to_string(v);
        ud_raise(UDE_THROWN, line, s->chars, 1);
        NEXT();                           /* unreachable: ud_raise longjmps */
    }

    CASE(OP_PRINT): {
        struct ud_value v = POP();
        struct ud_string *s = ud_value_to_string(v);
        fwrite(s->chars, 1, (size_t)s->length, stdout);
        fputc('\n', stdout);
        NEXT();
    }
    CASE(OP_HALT): return ud_int(0); /* stops the run; unreachable in practice */

#ifndef UD_USE_GOTO
        default:
            ud_error(UDE_BADBYTECODE, LINE(), "the bytecode contains an unknown instruction");
        }
    }
#endif

    return ud_nil(); /* control leaves only via OP_RETURN / OP_HALT above */

#undef READ_BYTE
#undef READ_U16
#undef READ_CONST
#undef LINE
#undef PUSH
#undef POP
#undef CASE
#undef NEXT
}

/* Call a UD function/lambda value from C. Mirrors OP_CALL's frame setup, then
 * runs vm_execute pinned to the current depth so it returns the moment that one
 * call completes. The value stack is left exactly as it was on entry. */
static struct ud_value vm_call_value(struct vm *vm, struct ud_value callee,
                                     struct ud_value *argv, int argc, int line) {
    if (!UD_IS_FUNCTION(callee))
        ud_error(UDE_NOT_CALLABLE, line,
                 "this callback is a %s, not a function", human_type(callee));
    struct ud_function *fn = UD_AS_FUNCTION(callee);
    if (argc != fn->arity)
        ud_error(UDE_ARGCOUNT, line, "%s() expects %d argument%s but got %d",
                 fn->name ? fn->name->chars : "?", fn->arity,
                 fn->arity == 1 ? "" : "s", argc);
    if (vm->frame_count >= FRAMES_MAX)
        ud_error(UDE_STACKOVERFLOW, line, "too many nested calls (runaway recursion?)");

    ud_push(&vm->stack, callee);
    for (int i = 0; i < argc; i++) ud_push(&vm->stack, argv[i]);

    struct ud_value *slots = vm->stack.top - argc;
    if (fn->param_types)
        for (int i = 0; i < argc; i++)
            if (fn->param_types[i] != DT_NONE)
                slots[i] = coerce_decl(slots[i], fn->param_types[i], line);
    if (slots + fn->num_slots + 8 > vm->stack.base + vm->stack.cap)
        ud_error(UDE_STACKOVERFLOW, line, "the value stack overflowed");

    int base = vm->frame_count;
    struct call_frame *frame = &vm->frames[vm->frame_count++];
    frame->fn = fn;
    frame->ip = fn->code;
    frame->slots = slots;
    /* On return, OP_RETURN has already reset stack.top below the pushed callee. */
    return vm_execute(vm, base);
}
