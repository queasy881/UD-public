/* errors.h -- UD's single, centralized error channel.
 *
 * Every compile-time and runtime failure funnels through ud_error(). It prints
 * a consistent, human-readable report:
 *
 *     UD Error <N>: <short description>
 *       Why: <one-line plain-language explanation>
 *       at line <L>   (when a line is known)
 *
 * and then longjmps back to the top-level handler installed by main.c, so a
 * broken UD program never surfaces a raw C crash or segfault. Error numbers are
 * stable and documented in the README so users can look them up.
 */
#ifndef UD_ERRORS_H
#define UD_ERRORS_H

#include <setjmp.h>

enum ud_errcode {
    UDE_NO_ENTRY        = 1,  /* no `int function entry()` defined            */
    UDE_SYNTAX          = 2,  /* lexer/parser could not understand the source */
    UDE_UNDEF_VAR       = 3,  /* read of a variable that was never assigned   */
    UDE_UNDEF_FUNC      = 4,  /* call to a function/global that doesn't exist */
    UDE_TYPE            = 5,  /* operation applied to the wrong value type    */
    UDE_ARGCOUNT        = 6,  /* function called with the wrong # of args     */
    UDE_DIVZERO         = 7,  /* division or modulo by zero                   */
    UDE_INDEX           = 8,  /* array/string index out of bounds            */
    UDE_NOT_INDEXABLE   = 9,  /* indexing something that isn't array/string   */
    UDE_CIN_INT         = 10, /* typed cin expected an integer               */
    UDE_CIN_FLOAT       = 11, /* typed cin expected a number                 */
    UDE_CIN_BOOL        = 12, /* typed cin expected a boolean                */
    UDE_FIELD           = 13, /* unknown struct field                         */
    UDE_STRUCT          = 14, /* unknown struct type / bad construction       */
    UDE_IO              = 15, /* could not read/write a file                  */
    UDE_BADBYTECODE     = 16, /* .ldx file is missing/corrupt/wrong version   */
    UDE_STACKOVERFLOW   = 17, /* call depth exceeded                          */
    UDE_SLICE           = 18, /* malformed slice bounds                       */
    UDE_ATTR            = 19, /* unknown method/attribute on a value          */
    UDE_NOT_CALLABLE    = 20, /* attempted to call a non-function value       */
    UDE_CONVERT         = 21, /* invalid explicit type conversion             */
    UDE_THROWN          = 22, /* a value raised by `throw` reached the top     */
    UDE_INTERNAL        = 99  /* should-never-happen guard                    */
};

/* The top-level recovery point. main.c does setjmp(ud_error_jmp) before running
 * anything; ud_error() longjmps here with the error code as the return value. */
extern jmp_buf ud_error_jmp;
extern int     ud_had_error;

/* try/catch plumbing. When the VM arms a `try`, it points ud_catch_current at
 * that handler's jmp_buf; ud_error()/ud_raise() then longjmp there (stashing the
 * details in ud_pending_error) instead of unwinding to the top level. NULL means
 * "no handler armed" -- errors are fatal and reported as before. */
struct ud_errinfo {
    int  code;
    int  line;
    int  has_value;         /* 1 when a `throw`n UD value is waiting in the VM */
    char message[480];
};
extern struct ud_errinfo ud_pending_error;
extern jmp_buf          *ud_catch_current;

/* Raise a pre-formatted, catchable error (used by `throw`). Never returns. */
#if defined(__GNUC__)
__attribute__((noreturn))
#endif
void ud_raise(enum ud_errcode code, int line, const char *message, int has_value);

/* Report an error and unwind. `line` may be 0 when no source location applies.
 * Never returns. */
#if defined(__GNUC__)
__attribute__((noreturn, format(printf, 3, 4)))
#endif
void ud_error(enum ud_errcode code, int line, const char *why_fmt, ...);

/* The stable short description associated with a code (used in reports and the
 * README's error table). */
const char *ud_error_title(enum ud_errcode code);

#endif /* UD_ERRORS_H */
