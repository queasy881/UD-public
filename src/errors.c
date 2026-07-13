/* errors.c -- implementation of UD's centralized error channel. */
#include "errors.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

jmp_buf ud_error_jmp;
int     ud_had_error = 0;

/* try/catch routing state (see errors.h). */
struct ud_errinfo ud_pending_error;
jmp_buf          *ud_catch_current = NULL;

const char *ud_error_title(enum ud_errcode code) {
    switch (code) {
        case UDE_NO_ENTRY:      return "No entry point";
        case UDE_SYNTAX:        return "Syntax error";
        case UDE_UNDEF_VAR:     return "Undefined variable";
        case UDE_UNDEF_FUNC:    return "Undefined function";
        case UDE_TYPE:          return "Type mismatch";
        case UDE_ARGCOUNT:      return "Wrong number of arguments";
        case UDE_DIVZERO:       return "Division by zero";
        case UDE_INDEX:         return "Index out of bounds";
        case UDE_NOT_INDEXABLE: return "Value is not indexable";
        case UDE_CIN_INT:       return "Input is not an integer";
        case UDE_CIN_FLOAT:     return "Input is not a number";
        case UDE_CIN_BOOL:      return "Input is not a boolean";
        case UDE_FIELD:         return "Unknown struct field";
        case UDE_STRUCT:        return "Unknown struct type";
        case UDE_IO:            return "File I/O error";
        case UDE_BADBYTECODE:   return "Invalid bytecode file";
        case UDE_STACKOVERFLOW: return "Call stack overflow";
        case UDE_SLICE:         return "Bad slice bounds";
        case UDE_ATTR:          return "Unknown attribute or method";
        case UDE_NOT_CALLABLE:  return "Value is not callable";
        case UDE_CONVERT:       return "Invalid conversion";
        case UDE_THROWN:        return "Uncaught error";
        case UDE_INTERNAL:      return "Internal error";
        default:                return "Error";
    }
}

/* Common tail: either hand the error to an armed try-handler, or print the
 * report and unwind to the top level. `has_value` records whether a `throw`n UD
 * value is parked in the VM for the catch clause to pick up. */
static void ud_deliver(int code, int line, const char *msg, int has_value) {
    if (ud_catch_current) {
        ud_pending_error.code      = code;
        ud_pending_error.line      = line;
        ud_pending_error.has_value = has_value;
        size_t n = strlen(msg);
        if (n >= sizeof(ud_pending_error.message))
            n = sizeof(ud_pending_error.message) - 1;
        memcpy(ud_pending_error.message, msg, n);
        ud_pending_error.message[n] = '\0';
        longjmp(*ud_catch_current, 1);
    }

    fflush(stdout);
    fprintf(stderr, "UD Error %d: %s\n", code, ud_error_title((enum ud_errcode)code));
    fprintf(stderr, "  Why: %s\n", msg);
    if (line > 0)
        fprintf(stderr, "  at line %d\n", line);
    fflush(stderr);

    ud_had_error = 1;
    longjmp(ud_error_jmp, code);
}

void ud_error(enum ud_errcode code, int line, const char *why_fmt, ...) {
    char buf[480];
    va_list ap;
    va_start(ap, why_fmt);
    vsnprintf(buf, sizeof buf, why_fmt, ap);
    va_end(ap);
    ud_deliver((int)code, line, buf, 0);
    for (;;) { } /* unreachable: ud_deliver always longjmps */
}

void ud_raise(enum ud_errcode code, int line, const char *message, int has_value) {
    ud_deliver((int)code, line, message, has_value);
    for (;;) { } /* unreachable */
}
