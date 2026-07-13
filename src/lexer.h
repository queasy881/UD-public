/* lexer.h -- UD tokenizer.
 *
 * Turns UD source into a flat array of tokens up front (the parser then walks
 * that array, which makes lookahead trivial). Comments are `--` or `//` to end
 * of line. String concatenation is `..`; `+` stays numeric-only.
 */
#ifndef UD_LEXER_H
#define UD_LEXER_H

enum ud_tok {
    T_EOF = 0,

    /* literals / names */
    T_INT, T_FLOAT, T_STRING, T_IDENT,

    /* keywords */
    T_FUNCTION, T_END, T_RETURN,
    T_IF, T_THEN, T_ELSEIF, T_ELSE,
    T_WHILE, T_DO, T_UNLESS,
    T_FOR, T_IN,
    T_BREAK, T_CONTINUE,
    T_STRUCT,
    T_AND, T_OR, T_NOT,
    T_NIL, T_TRUE, T_FALSE,
    T_KW_INT, T_KW_FLOAT, T_KW_BOOL, T_KW_STRING, /* type keywords */
    T_CONST, T_ENUM,
    T_TRY, T_CATCH, T_THROW, T_REQUIRE,

    /* punctuation / operators */
    T_LPAREN, T_RPAREN, T_LBRACK, T_RBRACK, T_LBRACE, T_RBRACE,
    T_COMMA, T_DOT, T_COLON, T_QUESTION,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_STARSTAR,
    T_PLUSPLUS,                  /* ++ */
    T_CONCAT,                    /* .. */
    T_ASSIGN,                    /* =  */
    T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PERCENTEQ,
    T_EQ, T_NE, T_LT, T_GT, T_LE, T_GE,
    T_AMP, T_PIPE, T_CARET, T_TILDE, T_SHL, T_SHR
};

struct ud_token {
    int type;
    const char *start; /* points into the source buffer */
    int length;
    int line;
};

/* Tokenize `src` into an arena-allocated array. Returns the array and writes
 * the count to *out_count. A trailing T_EOF token is always present. Reports
 * lexical problems through ud_error(). */
struct ud_token *ud_lex(const char *src, int *out_count);

#endif /* UD_LEXER_H */
