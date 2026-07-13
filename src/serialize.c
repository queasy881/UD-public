/* serialize.c -- the .ldx container format.
 *
 * Layout (all integers little-endian):
 *
 *   magic   "UDLX"            (4 bytes)
 *   version u32
 *   fcount  u32               number of functions
 *   scount  u32               number of structs
 *   entry   u32               index into the function table
 *
 *   scount x struct:
 *     name  str               (u32 length + bytes)
 *     fc    u32               field count
 *     fc x (name str, type u8)
 *
 *   fcount x function:
 *     name       str
 *     arity      u32
 *     arity x param_type u8
 *     return_type u8
 *     num_slots  u32
 *     code_len   u32
 *     code_len x code byte
 *     code_len x line u32
 *     const_count u32
 *     const_count x const (tag u8 + payload)
 *
 * Constants only ever hold ints, floats and strings (the compiler never emits
 * function/array/struct literals into a chunk), but nil/bool tags are handled
 * for completeness.
 *
 * A .ldx comes in two shapes that share the exact same payload:
 *
 *   thin       [UDLX payload]
 *   standalone [ud runtime executable][UDLX payload][u64 offset LE]["UDLXFUSE"]
 *
 * The thin form is portable (the same bytes run on any platform via `ud run`).
 * The standalone form prepends a full copy of the native `ud` runtime, so the
 * file is *also* a valid executable that runs its own embedded payload with no
 * `ud` installed -- while still being a .ldx that `ud run` accepts (the reader
 * spots the trailer and skips to the payload). The 16-byte trailer makes the
 * two round-trippable: the offset says where the payload starts, the magic
 * marks the file as fused.
 */
#include "serialize.h"
#include "errors.h"
#include "ast.h" /* DT_NONE */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h> /* chmod the standalone output executable */
#endif

#define LDX_MAGIC      "UDLX"
#define LDX_VERSION    1u
#define LDX_FUSE_MAGIC "UDLXFUSE" /* 8-byte trailer tag on a standalone .ldx */

/* ------------------------------------------------------------------ */
/* Writing                                                            */
/* ------------------------------------------------------------------ */

static void w_u8(FILE *f, uint8_t v) { fputc((int)v, f); }

static void w_u32(FILE *f, uint32_t v) {
    fputc((int)(v & 0xFF), f);        fputc((int)((v >> 8) & 0xFF), f);
    fputc((int)((v >> 16) & 0xFF), f); fputc((int)((v >> 24) & 0xFF), f);
}

static void w_u64(FILE *f, uint64_t v) {
    for (int i = 0; i < 8; i++) { fputc((int)(v & 0xFF), f); v >>= 8; }
}

static void w_str(FILE *f, struct ud_string *s) {
    w_u32(f, (uint32_t)s->length);
    if (s->length) fwrite(s->chars, 1, (size_t)s->length, f);
}

/* Serialize one function chunk: header, code, line table, then constants.
 * Recursive -- a constant may itself be a function (a lambda), tagged 5. */
static void write_function(FILE *f, struct ud_function *fn) {
    w_str(f, fn->name);
    w_u32(f, (uint32_t)fn->arity);
    for (int j = 0; j < fn->arity; j++)
        w_u8(f, fn->param_types ? fn->param_types[j] : (uint8_t)DT_NONE);
    w_u8(f, fn->return_type);
    w_u32(f, (uint32_t)fn->num_slots);
    w_u32(f, (uint32_t)fn->code_len);
    if (fn->code_len) fwrite(fn->code, 1, (size_t)fn->code_len, f);
    for (int j = 0; j < fn->code_len; j++) w_u32(f, (uint32_t)fn->lines[j]);
    w_u32(f, (uint32_t)fn->const_count);
    for (int j = 0; j < fn->const_count; j++) {
        struct ud_value v = fn->consts[j];
        switch (v.type) {
            case UD_NIL:   w_u8(f, 0); break;
            case UD_BOOL:  w_u8(f, 1); w_u8(f, (uint8_t)v.as.b); break;
            case UD_INT:   w_u8(f, 2); w_u64(f, (uint64_t)v.as.i); break;
            case UD_FLOAT: {
                uint64_t bits; memcpy(&bits, &v.as.d, 8);
                w_u8(f, 3); w_u64(f, bits);
                break;
            }
            case UD_OBJ:
                if (UD_IS_STRING(v)) { w_u8(f, 4); w_str(f, UD_AS_STRING(v)); }
                else if (UD_IS_FUNCTION(v)) {
                    w_u8(f, 5); write_function(f, UD_AS_FUNCTION(v));
                } else ud_error(UDE_INTERNAL, 0,
                                "internal: cannot serialize this constant");
                break;
            default:
                ud_error(UDE_INTERNAL, 0, "internal: bad constant type");
        }
    }
}

/* Write just the portable UDLX payload to an already-open stream. Shared by the
 * thin and standalone writers -- the only difference between the two formats is
 * what surrounds this payload. */
static void write_payload(FILE *f, struct ud_program *prog) {
    fwrite(LDX_MAGIC, 1, 4, f);
    w_u32(f, LDX_VERSION);
    w_u32(f, (uint32_t)prog->function_count);
    w_u32(f, (uint32_t)prog->struct_count);

    int entry_idx = -1;
    for (int i = 0; i < prog->function_count; i++)
        if (prog->functions[i] == prog->entry) { entry_idx = i; break; }
    w_u32(f, (uint32_t)entry_idx);

    for (int i = 0; i < prog->struct_count; i++) {
        struct ud_structdef *d = prog->structs[i];
        w_str(f, d->name);
        w_u32(f, (uint32_t)d->field_count);
        for (int j = 0; j < d->field_count; j++) {
            w_str(f, d->field_names[j]);
            w_u8(f, d->field_types[j]);
        }
    }

    for (int i = 0; i < prog->function_count; i++)
        write_function(f, prog->functions[i]);
}

/* Thin .ldx: nothing but the portable payload. */
void ud_serialize_write(struct ud_program *prog, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) ud_error(UDE_IO, 0, "could not open '%s' for writing", path);
    write_payload(f, prog);
    if (ferror(f)) { fclose(f); ud_error(UDE_IO, 0, "failed while writing '%s'", path); }
    fclose(f);
}

/* Standalone .ldx: a full copy of the `ud` runtime at `runtime_path`, then the
 * payload, then a 16-byte trailer (u64 payload offset + "UDLXFUSE"). The result
 * self-executes yet is still a valid .ldx. If `runtime_path` is itself a fused
 * .ldx we copy only its runtime half, so re-fusing never nests payloads. */
void ud_serialize_write_standalone(struct ud_program *prog, const char *path,
                                   const char *runtime_path) {
    FILE *rf = fopen(runtime_path, "rb");
    if (!rf) ud_error(UDE_IO, 0, "could not open the UD runtime '%s'", runtime_path);
    fseek(rf, 0, SEEK_END);
    long rsz = ftell(rf);
    fseek(rf, 0, SEEK_SET);
    if (rsz <= 0) { fclose(rf); ud_error(UDE_IO, 0, "the UD runtime '%s' is empty", runtime_path); }
    uint8_t *rbuf = (uint8_t *)malloc((size_t)rsz);
    if (!rbuf) { fclose(rf); ud_error(UDE_INTERNAL, 0, "out of memory"); }
    size_t rgot = fread(rbuf, 1, (size_t)rsz, rf);
    fclose(rf);

    /* Strip an existing fuse so we copy only the clean runtime bytes. */
    size_t runtime_len = rgot;
    if (rgot >= 16 && memcmp(rbuf + rgot - 8, LDX_FUSE_MAGIC, 8) == 0) {
        uint64_t off = 0;
        for (int i = 0; i < 8; i++) off |= (uint64_t)rbuf[rgot - 16 + i] << (8 * i);
        if (off <= (uint64_t)(rgot - 16)) runtime_len = (size_t)off;
    }

    FILE *f = fopen(path, "wb");
    if (!f) { free(rbuf); ud_error(UDE_IO, 0, "could not open '%s' for writing", path); }
    fwrite(rbuf, 1, runtime_len, f);
    free(rbuf);

    write_payload(f, prog);

    w_u64(f, (uint64_t)runtime_len);
    fwrite(LDX_FUSE_MAGIC, 1, 8, f);

    if (ferror(f)) { fclose(f); ud_error(UDE_IO, 0, "failed while writing '%s'", path); }
    fclose(f);

#ifndef _WIN32
    chmod(path, 0755); /* the payload alone isn't runnable; the whole file is */
#endif
}

/* ------------------------------------------------------------------ */
/* Reading                                                            */
/* ------------------------------------------------------------------ */

struct reader {
    const uint8_t *p;
    const uint8_t *end;
    uint8_t *owned; /* the malloc'd buffer, freed on error/finish */
};

static void r_fail(struct reader *r, const char *why) {
    free(r->owned);
    ud_error(UDE_BADBYTECODE, 0, "%s", why);
}

static uint8_t r_u8(struct reader *r) {
    if (r->p >= r->end) r_fail(r, "this .ldx file is truncated");
    return *r->p++;
}

static uint32_t r_u32(struct reader *r) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)r_u8(r) << (8 * i);
    return v;
}

static uint64_t r_u64(struct reader *r) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)r_u8(r) << (8 * i);
    return v;
}

static struct ud_string *r_str(struct reader *r) {
    uint32_t len = r_u32(r);
    if (r->p + len > r->end) r_fail(r, "this .ldx file is truncated");
    struct ud_string *s = ud_str_intern((const char *)r->p, (int)len);
    r->p += len;
    return s;
}

/* Read one function chunk written by write_function(). Recursive: a constant
 * tagged 5 is a nested function (a lambda). */
static struct ud_function *read_function(struct reader *r) {
    struct ud_string *name = r_str(r);
    struct ud_function *fn = ud_function_new(name);
    fn->arity = (int)r_u32(r);
    if (fn->arity > 0) {
        fn->param_types = (uint8_t *)ud_alloc((size_t)fn->arity);
        for (int j = 0; j < fn->arity; j++) fn->param_types[j] = r_u8(r);
    }
    fn->return_type = r_u8(r);
    fn->num_slots = (int)r_u32(r);
    fn->code_len = (int)r_u32(r);
    fn->code_cap = fn->code_len;
    if (fn->code_len > 0) {
        if (r->p + fn->code_len > r->end) r_fail(r, "this .ldx file is truncated");
        fn->code = (uint8_t *)ud_alloc((size_t)fn->code_len);
        memcpy(fn->code, r->p, (size_t)fn->code_len);
        r->p += fn->code_len;
        fn->lines = (int *)ud_alloc(sizeof(int) * (size_t)fn->code_len);
        for (int j = 0; j < fn->code_len; j++) fn->lines[j] = (int)r_u32(r);
    }
    fn->const_count = (int)r_u32(r);
    fn->const_cap = fn->const_count;
    if (fn->const_count > 0)
        fn->consts = (struct ud_value *)
            ud_alloc(sizeof(struct ud_value) * (size_t)fn->const_count);
    for (int j = 0; j < fn->const_count; j++) {
        uint8_t tag = r_u8(r);
        struct ud_value v;
        switch (tag) {
            case 0: v = ud_nil(); break;
            case 1: v = ud_bool(r_u8(r)); break;
            case 2: v = ud_int((long long)r_u64(r)); break;
            case 3: {
                uint64_t bits = r_u64(r);
                double d; memcpy(&d, &bits, 8);
                v = ud_float(d);
                break;
            }
            case 4: v = ud_obj_val((struct ud_obj *)r_str(r)); break;
            case 5: v = ud_obj_val((struct ud_obj *)read_function(r)); break;
            default: r_fail(r, "this .ldx file has a corrupt constant");
                     return NULL; /* unreachable: r_fail never returns */
        }
        fn->consts[j] = v;
    }
    return fn;
}

void ud_serialize_read(struct ud_program *prog, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) ud_error(UDE_IO, 0, "could not open '%s'", path);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); ud_error(UDE_IO, 0, "could not read '%s'", path); }

    uint8_t *buf = (uint8_t *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); ud_error(UDE_INTERNAL, 0, "out of memory"); }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    struct reader r;
    r.p = buf; r.end = buf + got; r.owned = buf;

    /* Standalone .ldx? Skip the prepended runtime and land on the payload. */
    if (got >= 16 && memcmp(buf + got - 8, LDX_FUSE_MAGIC, 8) == 0) {
        uint64_t off = 0;
        for (int i = 0; i < 8; i++) off |= (uint64_t)buf[got - 16 + i] << (8 * i);
        if (off > (uint64_t)(got - 16)) r_fail(&r, "this .ldx file has a corrupt fuse trailer");
        r.p   = buf + off;
        r.end = buf + got - 16;
    }

    if ((r.end - r.p) < 8 || memcmp(r.p, LDX_MAGIC, 4) != 0)
        r_fail(&r, "this is not a UD .ldx file");
    r.p += 4;
    if (r_u32(&r) != LDX_VERSION)
        r_fail(&r, "this .ldx was built by a different version of UD");

    uint32_t fcount = r_u32(&r);
    uint32_t scount = r_u32(&r);
    uint32_t entry_idx = r_u32(&r);

    for (uint32_t i = 0; i < scount; i++) {
        struct ud_string *name = r_str(&r);
        uint32_t fc = r_u32(&r);
        struct ud_structdef *d = ud_structdef_new(name, (int)fc);
        for (uint32_t j = 0; j < fc; j++) {
            d->field_names[j] = r_str(&r);
            d->field_types[j] = r_u8(&r);
        }
        ud_map_set(&prog->globals, name, ud_obj_val((struct ud_obj *)d));
        ud_program_add_struct(prog, d);
    }

    for (uint32_t i = 0; i < fcount; i++) {
        struct ud_function *fn = read_function(&r);
        ud_map_set(&prog->globals, fn->name, ud_obj_val((struct ud_obj *)fn));
        ud_program_add_function(prog, fn);
    }

    if (entry_idx >= fcount || prog->function_count == 0)
        r_fail(&r, "this .ldx file has no valid entry point");
    prog->entry = prog->functions[entry_idx];

    free(buf);
}

/* True if `path` is a standalone (fused) .ldx -- i.e. the running executable is
 * carrying its own embedded program. Any read failure just means "no". */
int ud_serialize_is_standalone(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, -8L, SEEK_END) != 0) { fclose(f); return 0; }
    char tail[8];
    size_t got = fread(tail, 1, 8, f);
    fclose(f);
    return got == 8 && memcmp(tail, LDX_FUSE_MAGIC, 8) == 0;
}
