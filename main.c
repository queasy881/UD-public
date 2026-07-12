/* main.c -- the `ud` command line.
 *
 * Three ways to run UD, all sharing one compiler and one VM:
 *
 *   ud file.ud               lex -> parse -> compile -> run (in memory)
 *   ud build file.ud -o x    lex -> parse -> compile -> serialize to .ldx
 *   ud run file.ldx          load .ldx -> run
 *
 * A single setjmp() here is the landing pad for every ud_error(): any failure,
 * compile-time or runtime, prints its numbered report and unwinds to main,
 * which exits with that error number. UD never shows a raw C crash.
 */
#include "speed.h"
#include "errors.h"
#include "lexer.h"
#include "parser.h"
#include "compiler.h"
#include "vm.h"
#include "serialize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UD_VERSION_STRING "UD 1.0.0"

static int usage(FILE *out) {
    fprintf(out,
        "UD -- a small, fast scripting language.\n"
        "\n"
        "Usage:\n"
        "  ud <file.ud>                    compile and run a UD program\n"
        "  ud build <file.ud> [-o out.ldx] compile to portable .ldx bytecode\n"
        "  ud run <file.ldx>               run a compiled .ldx program\n"
        "  ud --version                    print the version\n"
        "  ud --help                       show this help\n"
        "\n"
        "Every program needs an entry point:\n"
        "  int function entry()\n"
        "      return 0\n"
        "  end\n");
    return out == stderr ? 64 : 0;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) ud_error(UDE_IO, 0, "could not open '%s'", path);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); ud_error(UDE_IO, 0, "could not read '%s'", path); }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); ud_error(UDE_INTERNAL, 0, "out of memory"); }
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* lex + parse + compile a source file into an already-initialized program. */
static void compile_file(const char *path, struct ud_program *prog) {
    char *src = read_file(path);
    int ntok = 0;
    struct ud_token *toks = ud_lex(src, &ntok);
    struct ud_node *ast = ud_parse(toks, ntok);
    ud_compile(ast, prog);
    /* The AST/bytecode keeps only interned strings, so the raw source and token
     * buffers are safe to release now. */
    free(toks);
    free(src);
}

/* Derive "foo.ldx" from "foo.ud" (replace the final extension, else append). */
static char *default_out_path(const char *src) {
    size_t n = strlen(src);
    size_t sep = 0;
    for (size_t i = 0; i < n; i++)
        if (src[i] == '/' || src[i] == '\\') sep = i + 1;
    size_t base = n;
    for (size_t i = sep; i < n; i++)
        if (src[i] == '.') base = i;
    char *out = (char *)malloc(base + 5);
    if (!out) ud_error(UDE_INTERNAL, 0, "out of memory");
    memcpy(out, src, base);
    memcpy(out + base, ".ldx", 4);
    out[base + 4] = '\0';
    return out;
}

int main(int argc, char **argv) {
    ud_heap_init();

    int err = setjmp(ud_error_jmp);
    if (err != 0) {
        /* ud_error() already printed the numbered report to stderr. */
        ud_heap_shutdown();
        return err;
    }

    if (argc < 2) { int c = usage(stderr); ud_heap_shutdown(); return c; }

    const char *cmd = argv[1];
    int exit_code = 0;

    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        printf("%s\n", UD_VERSION_STRING);
    } else if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        usage(stdout);
    } else if (strcmp(cmd, "build") == 0) {
        if (argc < 3) { int c = usage(stderr); ud_heap_shutdown(); return c; }
        const char *src_path = argv[2];
        const char *out_path = NULL;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out_path = argv[++i];
            else { int c = usage(stderr); ud_heap_shutdown(); return c; }
        }
        char *derived = NULL;
        if (!out_path) { derived = default_out_path(src_path); out_path = derived; }
        struct ud_program prog;
        ud_program_init(&prog);
        ud_register_builtins(&prog);
        compile_file(src_path, &prog);
        ud_serialize_write(&prog, out_path);
        printf("Built %s\n", out_path);
        free(derived);
    } else if (strcmp(cmd, "run") == 0) {
        if (argc < 3) { int c = usage(stderr); ud_heap_shutdown(); return c; }
        struct ud_program prog;
        ud_program_init(&prog);
        ud_register_builtins(&prog);
        ud_serialize_read(&prog, argv[2]);
        exit_code = ud_vm_run(&prog);
    } else {
        struct ud_program prog;
        ud_program_init(&prog);
        ud_register_builtins(&prog);
        compile_file(cmd, &prog);
        exit_code = ud_vm_run(&prog);
    }

    ud_heap_shutdown();
    return exit_code;
}
