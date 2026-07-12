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

struct ud_node *ud_parse(struct ud_token *toks, int count);

#endif /* UD_PARSER_H */
