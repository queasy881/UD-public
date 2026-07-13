/* parser.h -- UD recursive-descent + Pratt parser.
 *
 * Consumes the token array from the lexer and produces an AST. The top-level
 * result is an N_BLOCK whose items are the file's function and struct
 * declarations. All syntax problems are reported via ud_error(UDE_SYNTAX, ...).
 */
#ifndef UD_PARSER_H
#define UD_PARSER_H

#include "ast.h"
#include "lexer.h"

/* `path` is the source file's path (or NULL), used to resolve require() imports
 * relative to it; pass NULL when parsing source that has no file of origin. */
struct ud_node *ud_parse(struct ud_token *toks, int count, const char *path);

#endif /* UD_PARSER_H */
