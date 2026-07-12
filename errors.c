/* errors.c -- implementation of UD's centralized error channel. */
#include "errors.h"

#include <stdio.h>
#include <stdarg.h>

jmp_buf ud_error_jmp;
int     ud_had_error = 0;

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
        case UDE_INTERNAL:      return "Internal error";
        default:                return "Error";
    }
}

void ud_error(enum ud_errcode code, int line, const char *why_fmt, ...) {
    va_list ap;
    fflush(stdout);
    fprintf(stderr, "UD Error %d: %s\n", (int)code, ud_error_title(code));
    fprintf(stderr, "  Why: ");
    va_start(ap, why_fmt);
    vfprintf(stderr, why_fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    if (line > 0)
        fprintf(stderr, "  at line %d\n", line);
    fflush(stderr);

    ud_had_error = 1;
    longjmp(ud_error_jmp, (int)code);
}
