/* lexer.c -- UD tokenizer implementation. */
#include "lexer.h"
#include "errors.h"

#include <stdlib.h>
#include <string.h>

struct lexer {
    const char *src;
    const char *cur;
    int line;
    struct ud_token *toks;
    int count;
    int cap;
};

static void push_tok(struct lexer *lx, int type, const char *start, int length) {
    if (lx->count + 1 > lx->cap) {
        lx->cap = lx->cap < 64 ? 64 : lx->cap * 2;
        lx->toks = (struct ud_token *)realloc(lx->toks,
            sizeof(struct ud_token) * (size_t)lx->cap);
        if (!lx->toks) ud_error(UDE_INTERNAL, 0, "out of memory while lexing");
    }
    struct ud_token *t = &lx->toks[lx->count++];
    t->type = type;
    t->start = start;
    t->length = length;
    t->line = lx->line;
}

static int is_ident_start(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int is_ident(int c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
static int is_digit(int c) { return c >= '0' && c <= '9'; }

/* keyword table: text -> token type */
static int keyword_type(const char *s, int len) {
    struct { const char *kw; int type; } kws[] = {
        {"function", T_FUNCTION}, {"end", T_END}, {"return", T_RETURN},
        {"if", T_IF}, {"then", T_THEN}, {"elseif", T_ELSEIF}, {"else", T_ELSE},
        {"while", T_WHILE}, {"do", T_DO}, {"unless", T_UNLESS},
        {"for", T_FOR}, {"in", T_IN},
        {"break", T_BREAK}, {"continue", T_CONTINUE},
        {"struct", T_STRUCT},
        {"and", T_AND}, {"or", T_OR}, {"not", T_NOT},
        {"nil", T_NIL}, {"true", T_TRUE}, {"false", T_FALSE},
        {"int", T_KW_INT}, {"float", T_KW_FLOAT},
        {"bool", T_KW_BOOL}, {"string", T_KW_STRING},
        {"const", T_CONST}, {"enum", T_ENUM},
        {"try", T_TRY}, {"catch", T_CATCH}, {"throw", T_THROW},
        {"require", T_REQUIRE},
        {NULL, 0}
    };
    for (int i = 0; kws[i].kw; i++) {
        if ((int)strlen(kws[i].kw) == len && memcmp(kws[i].kw, s, (size_t)len) == 0)
            return kws[i].type;
    }
    return T_IDENT;
}

static void scan_string(struct lexer *lx, char quote) {
    const char *start = lx->cur; /* opening quote already consumed by caller */
    while (*lx->cur && *lx->cur != quote) {
        if (*lx->cur == '\n') lx->line++;
        if (*lx->cur == '\\' && lx->cur[1]) lx->cur += 2;
        else lx->cur++;
    }
    if (*lx->cur != quote)
        ud_error(UDE_SYNTAX, lx->line, "a string was opened but never closed");
    int length = (int)(lx->cur - start);
    push_tok(lx, T_STRING, start, length); /* content without quotes */
    lx->cur++; /* closing quote */
}

static void scan_number(struct lexer *lx) {
    const char *start = lx->cur;
    int is_float = 0;
    while (is_digit(*lx->cur)) lx->cur++;
    if (*lx->cur == '.' && is_digit(lx->cur[1])) {
        is_float = 1;
        lx->cur++;
        while (is_digit(*lx->cur)) lx->cur++;
    }
    if (*lx->cur == 'e' || *lx->cur == 'E') {
        is_float = 1;
        lx->cur++;
        if (*lx->cur == '+' || *lx->cur == '-') lx->cur++;
        while (is_digit(*lx->cur)) lx->cur++;
    }
    push_tok(lx, is_float ? T_FLOAT : T_INT, start, (int)(lx->cur - start));
}

struct ud_token *ud_lex(const char *src, int *out_count) {
    struct lexer lx;
    lx.src = src;
    lx.cur = src;
    lx.line = 1;
    lx.toks = NULL;
    lx.count = 0;
    lx.cap = 0;

    for (;;) {
        char c = *lx.cur;

        /* whitespace */
        if (c == ' ' || c == '\t' || c == '\r') { lx.cur++; continue; }
        if (c == '\n') { lx.line++; lx.cur++; continue; }

        /* comments: -- ... or // ... to end of line */
        if ((c == '-' && lx.cur[1] == '-') || (c == '/' && lx.cur[1] == '/')) {
            while (*lx.cur && *lx.cur != '\n') lx.cur++;
            continue;
        }
        /* block comments: /* ... *​/  (nestable) */
        if (c == '/' && lx.cur[1] == '*') {
            lx.cur += 2;
            int depth = 1;
            while (*lx.cur && depth > 0) {
                if (lx.cur[0] == '/' && lx.cur[1] == '*') { depth++; lx.cur += 2; }
                else if (lx.cur[0] == '*' && lx.cur[1] == '/') { depth--; lx.cur += 2; }
                else { if (*lx.cur == '\n') lx.line++; lx.cur++; }
            }
            if (depth > 0)
                ud_error(UDE_SYNTAX, lx.line, "a /* block comment was never closed");
            continue;
        }

        if (c == '\0') { push_tok(&lx, T_EOF, lx.cur, 0); break; }

        /* identifiers / keywords */
        if (is_ident_start(c)) {
            const char *start = lx.cur;
            while (is_ident(*lx.cur)) lx.cur++;
            int len = (int)(lx.cur - start);
            push_tok(&lx, keyword_type(start, len), start, len);
            continue;
        }

        /* numbers */
        if (is_digit(c)) { scan_number(&lx); continue; }

        /* strings */
        if (c == '"' || c == '\'') { lx.cur++; scan_string(&lx, c); continue; }

        /* operators and punctuation */
        const char *s = lx.cur;
        switch (c) {
            case '(' : push_tok(&lx, T_LPAREN, s, 1); lx.cur++; break;
            case ')' : push_tok(&lx, T_RPAREN, s, 1); lx.cur++; break;
            case '[' : push_tok(&lx, T_LBRACK, s, 1); lx.cur++; break;
            case ']' : push_tok(&lx, T_RBRACK, s, 1); lx.cur++; break;
            case '{' : push_tok(&lx, T_LBRACE, s, 1); lx.cur++; break;
            case '}' : push_tok(&lx, T_RBRACE, s, 1); lx.cur++; break;
            case ',' : push_tok(&lx, T_COMMA, s, 1); lx.cur++; break;
            case ':' : push_tok(&lx, T_COLON, s, 1); lx.cur++; break;
            case '?' : push_tok(&lx, T_QUESTION, s, 1); lx.cur++; break;
            case '~' : push_tok(&lx, T_TILDE, s, 1); lx.cur++; break;
            case '^' : push_tok(&lx, T_CARET, s, 1); lx.cur++; break;
            case '.' :
                if (lx.cur[1] == '.') { push_tok(&lx, T_CONCAT, s, 2); lx.cur += 2; }
                else { push_tok(&lx, T_DOT, s, 1); lx.cur++; }
                break;
            case '+' :
                if (lx.cur[1] == '=') { push_tok(&lx, T_PLUSEQ, s, 2); lx.cur += 2; }
                else if (lx.cur[1] == '+') { push_tok(&lx, T_PLUSPLUS, s, 2); lx.cur += 2; }
                else { push_tok(&lx, T_PLUS, s, 1); lx.cur++; }
                break;
            case '-' :
                if (lx.cur[1] == '=') { push_tok(&lx, T_MINUSEQ, s, 2); lx.cur += 2; }
                else { push_tok(&lx, T_MINUS, s, 1); lx.cur++; }
                break;
            case '*' :
                if (lx.cur[1] == '*') { push_tok(&lx, T_STARSTAR, s, 2); lx.cur += 2; }
                else if (lx.cur[1] == '=') { push_tok(&lx, T_STAREQ, s, 2); lx.cur += 2; }
                else { push_tok(&lx, T_STAR, s, 1); lx.cur++; }
                break;
            case '/' :
                if (lx.cur[1] == '=') { push_tok(&lx, T_SLASHEQ, s, 2); lx.cur += 2; }
                else { push_tok(&lx, T_SLASH, s, 1); lx.cur++; }
                break;
            case '%' :
                if (lx.cur[1] == '=') { push_tok(&lx, T_PERCENTEQ, s, 2); lx.cur += 2; }
                else { push_tok(&lx, T_PERCENT, s, 1); lx.cur++; }
                break;
            case '=' :
                if (lx.cur[1] == '=') { push_tok(&lx, T_EQ, s, 2); lx.cur += 2; }
                else { push_tok(&lx, T_ASSIGN, s, 1); lx.cur++; }
                break;
            case '!' :
                if (lx.cur[1] == '=') { push_tok(&lx, T_NE, s, 2); lx.cur += 2; }
                else { push_tok(&lx, T_NOT, s, 1); lx.cur++; } /* ! alias for not */
                break;
            case '<' :
                if (lx.cur[1] == '=') { push_tok(&lx, T_LE, s, 2); lx.cur += 2; }
                else if (lx.cur[1] == '<') { push_tok(&lx, T_SHL, s, 2); lx.cur += 2; }
                else { push_tok(&lx, T_LT, s, 1); lx.cur++; }
                break;
            case '>' :
                if (lx.cur[1] == '=') { push_tok(&lx, T_GE, s, 2); lx.cur += 2; }
                else if (lx.cur[1] == '>') { push_tok(&lx, T_SHR, s, 2); lx.cur += 2; }
                else { push_tok(&lx, T_GT, s, 1); lx.cur++; }
                break;
            case '&' :
                if (lx.cur[1] == '&') { push_tok(&lx, T_AND, s, 2); lx.cur += 2; }
                else { push_tok(&lx, T_AMP, s, 1); lx.cur++; }
                break;
            case '|' :
                if (lx.cur[1] == '|') { push_tok(&lx, T_OR, s, 2); lx.cur += 2; }
                else { push_tok(&lx, T_PIPE, s, 1); lx.cur++; }
                break;
            default:
                ud_error(UDE_SYNTAX, lx.line,
                         "unexpected character '%c' in the source", c);
        }
    }

    *out_count = lx.count;
    return lx.toks;
}
