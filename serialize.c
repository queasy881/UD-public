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
 */
#include "serialize.h"
#include "errors.h"
#include "ast.h" /* DT_NONE */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LDX_MAGIC   "UDLX"
#define LDX_VERSION 1u

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

void ud_serialize_write(struct ud_program *prog, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) ud_error(UDE_IO, 0, "could not open '%s' for writing", path);

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

    for (int i = 0; i < prog->function_count; i++) {
        struct ud_function *fn = prog->functions[i];
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
                    else ud_error(UDE_INTERNAL, 0,
                                  "internal: cannot serialize this constant");
                    break;
                default:
                    ud_error(UDE_INTERNAL, 0, "internal: bad constant type");
            }
        }
    }

    if (ferror(f)) { fclose(f); ud_error(UDE_IO, 0, "failed while writing '%s'", path); }
    fclose(f);
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

    if (got < 8 || memcmp(r.p, LDX_MAGIC, 4) != 0)
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
        struct ud_string *name = r_str(&r);
        struct ud_function *fn = ud_function_new(name);
        fn->arity = (int)r_u32(&r);
        if (fn->arity > 0) {
            fn->param_types = (uint8_t *)ud_alloc((size_t)fn->arity);
            for (int j = 0; j < fn->arity; j++) fn->param_types[j] = r_u8(&r);
        }
        fn->return_type = r_u8(&r);
        fn->num_slots = (int)r_u32(&r);
        fn->code_len = (int)r_u32(&r);
        fn->code_cap = fn->code_len;
        if (fn->code_len > 0) {
            if (r.p + fn->code_len > r.end) r_fail(&r, "this .ldx file is truncated");
            fn->code = (uint8_t *)ud_alloc((size_t)fn->code_len);
            memcpy(fn->code, r.p, (size_t)fn->code_len);
            r.p += fn->code_len;
            fn->lines = (int *)ud_alloc(sizeof(int) * (size_t)fn->code_len);
            for (int j = 0; j < fn->code_len; j++) fn->lines[j] = (int)r_u32(&r);
        }
        fn->const_count = (int)r_u32(&r);
        fn->const_cap = fn->const_count;
        if (fn->const_count > 0)
            fn->consts = (struct ud_value *)
                ud_alloc(sizeof(struct ud_value) * (size_t)fn->const_count);
        for (int j = 0; j < fn->const_count; j++) {
            uint8_t tag = r_u8(&r);
            struct ud_value v;
            switch (tag) {
                case 0: v = ud_nil(); break;
                case 1: v = ud_bool(r_u8(&r)); break;
                case 2: v = ud_int((long long)r_u64(&r)); break;
                case 3: {
                    uint64_t bits = r_u64(&r);
                    double d; memcpy(&d, &bits, 8);
                    v = ud_float(d);
                    break;
                }
                case 4: v = ud_obj_val((struct ud_obj *)r_str(&r)); break;
                default: r_fail(&r, "this .ldx file has a corrupt constant");
                         return; /* unreachable: r_fail never returns */
            }
            fn->consts[j] = v;
        }
        ud_map_set(&prog->globals, name, ud_obj_val((struct ud_obj *)fn));
        ud_program_add_function(prog, fn);
    }

    if (entry_idx >= fcount || prog->function_count == 0)
        r_fail(&r, "this .ldx file has no valid entry point");
    prog->entry = prog->functions[entry_idx];

    free(buf);
}
