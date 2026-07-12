/* compiler.h -- lowers the AST into bytecode inside ud_function chunks.
 *
 * Two things happen here: every function/struct is registered as a global so
 * calls resolve regardless of definition order, then each function body is
 * compiled. The mandatory entry() check lives here so a broken program is
 * never compiled into a .ldx with no starting point.
 */
#ifndef UD_COMPILER_H
#define UD_COMPILER_H

#include "ast.h"
#include "vm.h"

/* Compile a parsed program into `prog`. Reports problems via ud_error(). On
 * success prog->entry is set. */
void ud_compile(struct ud_node *program, struct ud_program *prog);

#endif /* UD_COMPILER_H */
