/* parser.c -- UD parser implementation. */
#include "parser.h"
#include "errors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct parser {
    struct ud_token *toks;
    int count;
    int pos;
    const char *base_dir; /* directory of the current file, for resolving require() */
};

/* -------- token helpers -------- */

static struct ud_token *cur(struct parser *p)  { return &p->toks[p->pos]; }
static struct ud_token *at(struct parser *p, int off) {
    int i = p->pos + off;
    if (i >= p->count) i = p->count - 1; /* clamp to EOF */
    return &p->toks[i];
}
static int check(struct parser *p, int type)   { return cur(p)->type == type; }
static struct ud_token *advance(struct parser *p) {
    struct ud_token *t = cur(p);
    if (p->pos < p->count - 1) p->pos++;
    return t;
}
static int match(struct parser *p, int type) {
    if (check(p, type)) { advance(p); return 1; }
    return 0;
}
static struct ud_token *expect(struct parser *p, int type, const char *what) {
    if (!check(p, type))
        ud_error(UDE_SYNTAX, cur(p)->line, "expected %s here", what);
    return advance(p);
}

/* After a `.`, any word-spelled token is a valid member name -- keywords like
 * `bool` or `end` become plain names in this position (e.g. random.bool). */
static struct ud_token *expect_member(struct parser *p) {
    struct ud_token *t = cur(p);
    if (t->length > 0) {
        char c = t->start[0];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')
            return advance(p);
    }
    ud_error(UDE_SYNTAX, t->line, "expected a field or method name here");
    return t; /* unreachable */
}

/* -------- node helpers -------- */

static struct ud_node *new_node(struct parser *p, int kind) {
    struct ud_node *n = (struct ud_node *)ud_alloc(sizeof(struct ud_node));
    memset(n, 0, sizeof(*n));
    n->kind = (uint8_t)kind;
    n->line = cur(p)->line;
    return n;
}

static void nl_init(struct ud_nodelist *l) { l->items = NULL; l->count = 0; l->cap = 0; }
static void nl_push(struct ud_nodelist *l, struct ud_node *n) {
    if (l->count + 1 > l->cap) {
        int nc = l->cap < 4 ? 4 : l->cap * 2;
        struct ud_node **it =
            (struct ud_node **)ud_alloc(sizeof(struct ud_node *) * (size_t)nc);
        if (l->count) memcpy(it, l->items, sizeof(struct ud_node *) * (size_t)l->count);
        l->items = it;
        l->cap = nc;
    }
    l->items[l->count++] = n;
}

static struct ud_string *tok_str(struct ud_token *t) {
    return ud_str_intern(t->start, t->length);
}

/* -------- decl type keyword -------- */

static int is_type_kw(int type) {
    return type == T_KW_INT || type == T_KW_FLOAT ||
           type == T_KW_BOOL || type == T_KW_STRING;
}
static int decltype_of(int type) {
    switch (type) {
        case T_KW_INT:    return DT_INT;
        case T_KW_FLOAT:  return DT_FLOAT;
        case T_KW_BOOL:   return DT_BOOL;
        case T_KW_STRING: return DT_STRING;
        default:          return DT_NONE;
    }
}
static const char *decltype_name(int dt) {
    switch (dt) {
        case DT_INT: return "int"; case DT_FLOAT: return "float";
        case DT_BOOL: return "bool"; case DT_STRING: return "string";
        default: return "";
    }
}

/* -------- forward decls -------- */
static struct ud_node *parse_expression(struct parser *p);
static struct ud_node *parse_statement(struct parser *p);
static struct ud_node *parse_block(struct parser *p);
static struct ud_node *parse_const(struct parser *p);
static struct ud_node *parse_lambda(struct parser *p);

/* -------- string escapes -------- */

static struct ud_string *make_string_literal(struct ud_token *t) {
    char *buf = (char *)ud_alloc((size_t)t->length + 1);
    int n = 0;
    for (int i = 0; i < t->length; i++) {
        char c = t->start[i];
        if (c == '\\' && i + 1 < t->length) {
            char e = t->start[++i];
            switch (e) {
                case 'n': buf[n++] = '\n'; break;
                case 't': buf[n++] = '\t'; break;
                case 'r': buf[n++] = '\r'; break;
                case '0': buf[n++] = '\0'; break;
                case '\\': buf[n++] = '\\'; break;
                case '"': buf[n++] = '"'; break;
                case '\'': buf[n++] = '\''; break;
                default: buf[n++] = e; break;
            }
        } else {
            buf[n++] = c;
        }
    }
    return ud_str_intern(buf, n);
}

/* -------- primary / postfix -------- */

static void parse_args(struct parser *p, struct ud_nodelist *args) {
    nl_init(args);
    expect(p, T_LPAREN, "'('");
    if (!check(p, T_RPAREN)) {
        do { nl_push(args, parse_expression(p)); } while (match(p, T_COMMA));
    }
    expect(p, T_RPAREN, "')' to close the argument list");
}

static struct ud_node *parse_primary(struct parser *p) {
    struct ud_token *t = cur(p);
    switch (t->type) {
        case T_INT: {
            struct ud_node *n = new_node(p, N_INT);
            n->as.ival = strtoll(t->start, NULL, 10);
            advance(p);
            return n;
        }
        case T_FLOAT: {
            struct ud_node *n = new_node(p, N_FLOAT);
            n->as.fval = strtod(t->start, NULL);
            advance(p);
            return n;
        }
        case T_STRING: {
            struct ud_node *n = new_node(p, N_STRING);
            n->as.sval = make_string_literal(t);
            advance(p);
            return n;
        }
        case T_TRUE: case T_FALSE: {
            struct ud_node *n = new_node(p, N_BOOL);
            n->as.ival = (t->type == T_TRUE);
            advance(p);
            return n;
        }
        case T_NIL: {
            struct ud_node *n = new_node(p, N_NIL);
            advance(p);
            return n;
        }
        case T_IDENT: {
            struct ud_node *n = new_node(p, N_IDENT);
            n->as.sval = tok_str(t);
            advance(p);
            return n;
        }
        /* type keywords used as conversion functions: int(x), float(x)... */
        case T_KW_INT: case T_KW_FLOAT: case T_KW_BOOL: case T_KW_STRING: {
            struct ud_node *n = new_node(p, N_IDENT);
            n->as.sval = tok_str(t);
            advance(p);
            return n;
        }
        case T_LPAREN: {
            advance(p);
            struct ud_node *e = parse_expression(p);
            expect(p, T_RPAREN, "')'");
            return e;
        }
        case T_LBRACK: { /* array literal */
            struct ud_node *n = new_node(p, N_ARRAY);
            nl_init(&n->as.array.elems);
            advance(p);
            if (!check(p, T_RBRACK)) {
                do {
                    if (check(p, T_RBRACK)) break; /* trailing comma */
                    nl_push(&n->as.array.elems, parse_expression(p));
                } while (match(p, T_COMMA));
            }
            expect(p, T_RBRACK, "']' to close the array");
            return n;
        }
        case T_LBRACE: { /* dict {k: v, ...} or set {v, ...}; empty {} is a dict */
            advance(p);
            if (check(p, T_RBRACE)) {           /* {} -> empty dict */
                struct ud_node *n = new_node(p, N_DICT);
                nl_init(&n->as.dict.keys);
                nl_init(&n->as.dict.vals);
                advance(p);
                return n;
            }
            struct ud_node *first = parse_expression(p);
            if (check(p, T_COLON)) {            /* { k: v, ... } -> dict */
                struct ud_node *n = new_node(p, N_DICT);
                nl_init(&n->as.dict.keys);
                nl_init(&n->as.dict.vals);
                advance(p); /* consume ':' */
                nl_push(&n->as.dict.keys, first);
                nl_push(&n->as.dict.vals, parse_expression(p));
                while (match(p, T_COMMA)) {
                    if (check(p, T_RBRACE)) break; /* trailing comma */
                    nl_push(&n->as.dict.keys, parse_expression(p));
                    expect(p, T_COLON, "':' between a dict key and its value");
                    nl_push(&n->as.dict.vals, parse_expression(p));
                }
                expect(p, T_RBRACE, "'}' to close the dict");
                return n;
            } else {                            /* { v, ... } -> set */
                struct ud_node *n = new_node(p, N_SET);
                nl_init(&n->as.set.elems);
                nl_push(&n->as.set.elems, first);
                while (match(p, T_COMMA)) {
                    if (check(p, T_RBRACE)) break; /* trailing comma */
                    nl_push(&n->as.set.elems, parse_expression(p));
                }
                expect(p, T_RBRACE, "'}' to close the set");
                return n;
            }
        }
        case T_FUNCTION: /* anonymous function expression (lambda) */
            return parse_lambda(p);
        default:
            ud_error(UDE_SYNTAX, t->line,
                     "expected a value (number, string, name, or '(') here");
    }
    return NULL; /* unreachable */
}

static struct ud_node *parse_postfix(struct parser *p) {
    struct ud_node *node = parse_primary(p);
    for (;;) {
        if (check(p, T_LPAREN)) {
            struct ud_node *n = new_node(p, N_CALL);
            n->as.call.callee = node;
            parse_args(p, &n->as.call.args);
            node = n;
        } else if (check(p, T_DOT)) {
            advance(p);
            struct ud_token *name = expect_member(p);
            if (check(p, T_LPAREN)) {
                struct ud_node *n = new_node(p, N_METHOD);
                n->as.method.target = node;
                n->as.method.name = tok_str(name);
                parse_args(p, &n->as.method.args);
                node = n;
            } else {
                struct ud_node *n = new_node(p, N_FIELD);
                n->as.field.target = node;
                n->as.field.name = tok_str(name);
                node = n;
            }
        } else if (check(p, T_PLUSPLUS)) { /* postfix x++ */
            advance(p);
            struct ud_node *n = new_node(p, N_INCR);
            n->as.incr.target = node;
            n->as.incr.is_prefix = 0;
            node = n;
        } else if (check(p, T_LBRACK)) {
            advance(p);
            struct ud_node *start = NULL, *stop = NULL;
            int is_slice = 0;
            if (check(p, T_COLON)) {
                is_slice = 1; advance(p);
                if (!check(p, T_RBRACK)) stop = parse_expression(p);
            } else {
                start = parse_expression(p);
                if (check(p, T_COLON)) {
                    is_slice = 1; advance(p);
                    if (!check(p, T_RBRACK)) stop = parse_expression(p);
                }
            }
            expect(p, T_RBRACK, "']'");
            if (is_slice) {
                struct ud_node *n = new_node(p, N_SLICE);
                n->as.slice.target = node;
                n->as.slice.start = start;
                n->as.slice.stop = stop;
                node = n;
            } else {
                struct ud_node *n = new_node(p, N_INDEX);
                n->as.index.target = node;
                n->as.index.index = start;
                node = n;
            }
        } else {
            break;
        }
    }
    return node;
}

/* -------- precedence ladder -------- */

static struct ud_node *parse_unary(struct parser *p); /* fwd */

static struct ud_node *parse_power(struct parser *p) {
    struct ud_node *left = parse_postfix(p);
    if (check(p, T_STARSTAR)) {
        int op = advance(p)->type;
        struct ud_node *right = parse_unary(p); /* right-assoc, allows -exp */
        struct ud_node *n = new_node(p, N_BINARY);
        n->as.binary.op = op;
        n->as.binary.left = left;
        n->as.binary.right = right;
        return n;
    }
    return left;
}

static struct ud_node *parse_unary(struct parser *p) {
    if (check(p, T_PLUSPLUS)) { /* prefix ++x */
        advance(p);
        struct ud_node *n = new_node(p, N_INCR);
        n->as.incr.target = parse_unary(p);
        n->as.incr.is_prefix = 1;
        return n;
    }
    if (check(p, T_MINUS) || check(p, T_TILDE) || check(p, T_PLUS)) {
        int op = advance(p)->type;
        struct ud_node *operand = parse_unary(p);
        if (op == T_PLUS) return operand; /* unary plus is a no-op */
        struct ud_node *n = new_node(p, N_UNARY);
        n->as.unary.op = op;
        n->as.unary.operand = operand;
        return n;
    }
    return parse_power(p);
}

/* helper to build a left-assoc binary chain for a set of operators */
#define BINCHAIN(fname, nextfn, cond)                                 \
    static struct ud_node *fname(struct parser *p) {                  \
        struct ud_node *left = nextfn(p);                             \
        while (cond) {                                                \
            int op = advance(p)->type;                                \
            struct ud_node *right = nextfn(p);                        \
            struct ud_node *n = new_node(p, N_BINARY);                \
            n->as.binary.op = op; n->as.binary.left = left;           \
            n->as.binary.right = right; left = n;                     \
        }                                                             \
        return left;                                                  \
    }

BINCHAIN(parse_mult,     parse_unary,  check(p,T_STAR)||check(p,T_SLASH)||check(p,T_PERCENT))
BINCHAIN(parse_additive, parse_mult,   check(p,T_PLUS)||check(p,T_MINUS))
BINCHAIN(parse_concat,   parse_additive, check(p,T_CONCAT))
BINCHAIN(parse_shift,    parse_concat, check(p,T_SHL)||check(p,T_SHR))
BINCHAIN(parse_bitand,   parse_shift,  check(p,T_AMP))
BINCHAIN(parse_bitxor,   parse_bitand, check(p,T_CARET))
BINCHAIN(parse_bitor,    parse_bitxor, check(p,T_PIPE))
BINCHAIN(parse_comparison, parse_bitor,
         check(p,T_EQ)||check(p,T_NE)||check(p,T_LT)||
         check(p,T_GT)||check(p,T_LE)||check(p,T_GE)||check(p,T_IN))

static struct ud_node *parse_not(struct parser *p) {
    if (check(p, T_NOT)) {
        advance(p);
        struct ud_node *operand = parse_not(p);
        struct ud_node *n = new_node(p, N_UNARY);
        n->as.unary.op = T_NOT;
        n->as.unary.operand = operand;
        return n;
    }
    return parse_comparison(p);
}

static struct ud_node *parse_and(struct parser *p) {
    struct ud_node *left = parse_not(p);
    while (check(p, T_AND)) {
        advance(p);
        struct ud_node *right = parse_not(p);
        struct ud_node *n = new_node(p, N_LOGICAL);
        n->as.logical.op = T_AND;
        n->as.logical.left = left;
        n->as.logical.right = right;
        left = n;
    }
    return left;
}

static struct ud_node *parse_or(struct parser *p) {
    struct ud_node *left = parse_and(p);
    while (check(p, T_OR)) {
        advance(p);
        struct ud_node *right = parse_and(p);
        struct ud_node *n = new_node(p, N_LOGICAL);
        n->as.logical.op = T_OR;
        n->as.logical.left = left;
        n->as.logical.right = right;
        left = n;
    }
    return left;
}

/* ternary: cond ? then : else  (lower than `or`, right-associative) */
static struct ud_node *parse_ternary(struct parser *p) {
    struct ud_node *cond = parse_or(p);
    if (check(p, T_QUESTION)) {
        advance(p);
        struct ud_node *n = new_node(p, N_TERNARY);
        n->as.ternary.cond = cond;
        n->as.ternary.then = parse_ternary(p);
        expect(p, T_COLON, "':' in a ternary '?:' expression");
        n->as.ternary.els = parse_ternary(p);
        return n;
    }
    return cond;
}

static struct ud_node *parse_expression(struct parser *p) {
    return parse_ternary(p);
}

/* -------- statements -------- */

static int is_assign_op(int type) {
    return type == T_ASSIGN || type == T_PLUSEQ || type == T_MINUSEQ ||
           type == T_STAREQ || type == T_SLASHEQ || type == T_PERCENTEQ;
}

static int is_block_end(struct parser *p) {
    int t = cur(p)->type;
    return t == T_END || t == T_ELSE || t == T_ELSEIF ||
           t == T_UNLESS || t == T_CATCH || t == T_EOF;
}

static struct ud_node *parse_if(struct parser *p) {
    struct ud_node *n = new_node(p, N_IF);
    nl_init(&n->as.ifs.conds);
    nl_init(&n->as.ifs.bodies);
    n->as.ifs.elsebody = NULL;
    advance(p); /* if */
    nl_push(&n->as.ifs.conds, parse_expression(p));
    expect(p, T_THEN, "'then'");
    nl_push(&n->as.ifs.bodies, parse_block(p));
    while (check(p, T_ELSEIF)) {
        advance(p);
        nl_push(&n->as.ifs.conds, parse_expression(p));
        expect(p, T_THEN, "'then'");
        nl_push(&n->as.ifs.bodies, parse_block(p));
    }
    if (match(p, T_ELSE))
        n->as.ifs.elsebody = parse_block(p);
    expect(p, T_END, "'end' to close the if");
    return n;
}

static struct ud_node *parse_while(struct parser *p) {
    struct ud_node *n = new_node(p, N_WHILE);
    advance(p); /* while */
    n->as.whiles.cond = parse_expression(p);
    expect(p, T_DO, "'do'");
    n->as.whiles.body = parse_block(p);
    n->as.whiles.unless = NULL;
    if (match(p, T_UNLESS))
        n->as.whiles.unless = parse_expression(p);
    expect(p, T_END, "'end' to close the while");
    return n;
}

static struct ud_node *parse_for(struct parser *p) {
    advance(p); /* for */
    struct ud_token *var = expect(p, T_IDENT, "a loop variable name");
    if (match(p, T_ASSIGN)) {
        struct ud_node *n = new_node(p, N_FORRANGE);
        n->as.forr.var = tok_str(var);
        n->as.forr.start = parse_expression(p);
        expect(p, T_COMMA, "',' (for i = start, stop[, step])");
        n->as.forr.stop = parse_expression(p);
        n->as.forr.step = NULL;
        if (match(p, T_COMMA)) n->as.forr.step = parse_expression(p);
        expect(p, T_DO, "'do'");
        n->as.forr.body = parse_block(p);
        expect(p, T_END, "'end' to close the for");
        return n;
    } else if (match(p, T_IN)) {
        struct ud_node *n = new_node(p, N_FORIN);
        n->as.forin.var = tok_str(var);
        n->as.forin.iter = parse_expression(p);
        expect(p, T_DO, "'do'");
        n->as.forin.body = parse_block(p);
        expect(p, T_END, "'end' to close the for");
        return n;
    }
    ud_error(UDE_SYNTAX, cur(p)->line,
             "a for loop needs '=' (range) or 'in' (iterator) after the name");
    return NULL;
}

/* try <body> catch [(name)] <handler> end
 * The catch variable is optional and, when present, written in parentheses:
 *   catch (e)  -> e receives the thrown value (or a string for a runtime error)
 *   catch      -> the error is not bound to a name
 * Parentheses are required around the name so a handler that simply starts with
 * a call (e.g. `cout(...)`) is never mistaken for the error variable. */
static struct ud_node *parse_try(struct parser *p) {
    struct ud_node *n = new_node(p, N_TRY);
    advance(p); /* try */
    n->as.trycatch.body = parse_block(p);
    expect(p, T_CATCH, "'catch' to handle errors from the 'try' block");

    n->as.trycatch.errname = NULL;
    if (match(p, T_LPAREN)) {
        if (check(p, T_IDENT))
            n->as.trycatch.errname = tok_str(advance(p));
        expect(p, T_RPAREN, "')' after the catch variable");
    }

    n->as.trycatch.handler = parse_block(p);
    expect(p, T_END, "'end' to close the try/catch");
    return n;
}

static struct ud_node *parse_throw(struct parser *p) {
    struct ud_node *n = new_node(p, N_THROW);
    advance(p); /* throw */
    n->as.expr = parse_expression(p);
    return n;
}

static struct ud_node *parse_vardecl(struct parser *p) {
    struct ud_node *n = new_node(p, N_VARDECL);
    n->as.vardecl.dtype = (uint8_t)decltype_of(advance(p)->type);
    n->as.vardecl.is_typed = 1;
    struct ud_token *name = expect(p, T_IDENT, "a variable name");
    n->as.vardecl.name = tok_str(name);
    n->as.vardecl.init = NULL;
    if (match(p, T_ASSIGN))
        n->as.vardecl.init = parse_expression(p);
    return n;
}

static struct ud_node *parse_statement(struct parser *p) {
    int t = cur(p)->type;

    /* typed variable declaration: `int a = ...`  (but `int(x)` is a call) */
    if (is_type_kw(t) && at(p, 1)->type == T_IDENT)
        return parse_vardecl(p);

    switch (t) {
        case T_CONST: return parse_const(p);
        case T_IF:    return parse_if(p);
        case T_WHILE: return parse_while(p);
        case T_FOR:   return parse_for(p);
        case T_RETURN: {
            struct ud_node *n = new_node(p, N_RETURN);
            advance(p);
            if (is_block_end(p)) { n->as.expr = NULL; return n; }
            struct ud_node *first = parse_expression(p);
            if (check(p, T_COMMA)) {   /* return a, b, c -> packs an array */
                struct ud_node *arr = new_node(p, N_ARRAY);
                nl_init(&arr->as.array.elems);
                nl_push(&arr->as.array.elems, first);
                while (match(p, T_COMMA))
                    nl_push(&arr->as.array.elems, parse_expression(p));
                n->as.expr = arr;
            } else {
                n->as.expr = first;
            }
            return n;
        }
        case T_BREAK:    { struct ud_node *n = new_node(p, N_BREAK);    advance(p); return n; }
        case T_CONTINUE: { struct ud_node *n = new_node(p, N_CONTINUE); advance(p); return n; }
        case T_TRY:   return parse_try(p);
        case T_THROW: return parse_throw(p);
        case T_REQUIRE:
            ud_error(UDE_SYNTAX, cur(p)->line,
                     "require may only appear at the top level of a file, "
                     "not inside a function");
            return NULL; /* unreachable */
        default: break;
    }

    /* expression statement or assignment */
    struct ud_node *expr = parse_expression(p);

    /* destructuring / multiple assignment: a, b, ... = ... */
    if (check(p, T_COMMA)) {
        struct ud_node *n = new_node(p, N_MULTIVARDECL);
        nl_init(&n->as.multi.names);
        nl_push(&n->as.multi.names, expr);
        while (match(p, T_COMMA))
            nl_push(&n->as.multi.names, parse_expression(p));
        expect(p, T_ASSIGN, "'=' to give the names their values");
        struct ud_node *first = parse_expression(p);
        if (check(p, T_COMMA)) {   /* several values -> pack into an array */
            struct ud_node *arr = new_node(p, N_ARRAY);
            nl_init(&arr->as.array.elems);
            nl_push(&arr->as.array.elems, first);
            while (match(p, T_COMMA))
                nl_push(&arr->as.array.elems, parse_expression(p));
            n->as.multi.init = arr;
        } else {
            n->as.multi.init = first;
        }
        n->as.multi.is_typed = 0;
        n->as.multi.dtype = DT_NONE;
        return n;
    }

    if (is_assign_op(cur(p)->type)) {
        if (expr->kind != N_IDENT && expr->kind != N_INDEX && expr->kind != N_FIELD)
            ud_error(UDE_SYNTAX, cur(p)->line,
                     "the left side of '=' must be a variable, index, or field");
        struct ud_node *n = new_node(p, N_ASSIGN);
        n->as.assign.op = advance(p)->type;
        n->as.assign.target = expr;
        n->as.assign.value = parse_expression(p);
        return n;
    }
    struct ud_node *n = new_node(p, N_EXPRSTMT);
    n->as.expr = expr;
    return n;
}

static struct ud_node *parse_block(struct parser *p) {
    struct ud_node *n = new_node(p, N_BLOCK);
    nl_init(&n->as.block);
    while (!is_block_end(p))
        nl_push(&n->as.block, parse_statement(p));
    return n;
}

/* -------- declarations -------- */

/* Shared tail of every function form: "(params) body end", filling an
 * already-created N_FUNC/N_LAMBDA node whose name/return fields are set. */
static void parse_params_and_body(struct parser *p, struct ud_node *n) {
    nl_init(&n->as.func.params);

    expect(p, T_LPAREN, "'(' for the parameter list");
    /* collect params + their declared types */
    int tmpcap = 8, pcount = 0;
    uint8_t *ptypes = (uint8_t *)ud_alloc((size_t)tmpcap);
    if (!check(p, T_RPAREN)) {
        do {
            int pt = DT_NONE;
            if (is_type_kw(cur(p)->type)) pt = decltype_of(advance(p)->type);
            struct ud_token *pname = expect(p, T_IDENT, "a parameter name");
            struct ud_node *id = new_node(p, N_IDENT);
            id->as.sval = tok_str(pname);
            nl_push(&n->as.func.params, id);
            if (pcount >= tmpcap) {
                int nc = tmpcap * 2;
                uint8_t *np = (uint8_t *)ud_alloc((size_t)nc);
                memcpy(np, ptypes, (size_t)pcount);
                ptypes = np; tmpcap = nc;
            }
            ptypes[pcount++] = (uint8_t)pt;
        } while (match(p, T_COMMA));
    }
    expect(p, T_RPAREN, "')' to close the parameter list");
    n->as.func.ptypes = ptypes;
    n->as.func.body = parse_block(p);
    expect(p, T_END, "'end' to close the function");
}

static struct ud_node *parse_function(struct parser *p, int has_ret, int rtype) {
    expect(p, T_FUNCTION, "'function'");
    struct ud_token *name = expect(p, T_IDENT, "a function name");
    struct ud_node *n = new_node(p, N_FUNC);
    n->as.func.name = tok_str(name);
    n->as.func.has_ret = has_ret;
    n->as.func.rtype = (uint8_t)rtype;
    parse_params_and_body(p, n);
    return n;
}

/* Anonymous function expression `function(params) ... end`: same machinery as a
 * named function, but nameless and with a dynamic (untyped) return. Lambdas are
 * first-class values -- store them in a variable, pass them, return them, call
 * them. They do not capture enclosing locals (pass what they need as args). */
static struct ud_node *parse_lambda(struct parser *p) {
    expect(p, T_FUNCTION, "'function'");
    struct ud_node *n = new_node(p, N_LAMBDA);
    n->as.func.name = NULL;
    n->as.func.has_ret = 0;
    n->as.func.rtype = (uint8_t)DT_NONE;
    parse_params_and_body(p, n);
    return n;
}

static struct ud_node *parse_struct(struct parser *p) {
    advance(p); /* struct */
    struct ud_token *name = expect(p, T_IDENT, "a struct name");
    struct ud_node *n = new_node(p, N_STRUCT);
    n->as.strct.name = tok_str(name);
    nl_init(&n->as.strct.fields);
    int tmpcap = 8, fcount = 0;
    uint8_t *ftypes = (uint8_t *)ud_alloc((size_t)tmpcap);
    while (!check(p, T_END) && !check(p, T_EOF)) {
        if (!is_type_kw(cur(p)->type))
            ud_error(UDE_SYNTAX, cur(p)->line,
                     "struct fields must be declared as 'type name' (e.g. float x)");
        int ft = decltype_of(advance(p)->type);
        struct ud_token *fname = expect(p, T_IDENT, "a field name");
        struct ud_node *id = new_node(p, N_IDENT);
        id->as.sval = tok_str(fname);
        nl_push(&n->as.strct.fields, id);
        if (fcount >= tmpcap) {
            int nc = tmpcap * 2;
            uint8_t *nf = (uint8_t *)ud_alloc((size_t)nc);
            memcpy(nf, ftypes, (size_t)fcount);
            ftypes = nf; tmpcap = nc;
        }
        ftypes[fcount++] = (uint8_t)ft;
    }
    n->as.strct.ftypes = ftypes;
    expect(p, T_END, "'end' to close the struct");
    return n;
}

static struct ud_node *parse_const(struct parser *p) {
    struct ud_node *n = new_node(p, N_VARDECL);
    advance(p); /* const */
    struct ud_token *name = expect(p, T_IDENT, "a name for the constant");
    n->as.vardecl.name = tok_str(name);
    n->as.vardecl.dtype = DT_NONE;
    n->as.vardecl.is_typed = 0;
    n->as.vardecl.is_const = 1;
    expect(p, T_ASSIGN, "'=' (a constant must be given a value)");
    n->as.vardecl.init = parse_expression(p);
    return n;
}

static struct ud_node *parse_enum(struct parser *p) {
    advance(p); /* enum */
    struct ud_token *name = expect(p, T_IDENT, "an enum name");
    struct ud_node *n = new_node(p, N_ENUM);
    n->as.enumdef.name = tok_str(name);
    nl_init(&n->as.enumdef.names);
    nl_init(&n->as.enumdef.vals);
    while (!check(p, T_END) && !check(p, T_EOF)) {
        struct ud_token *m = expect(p, T_IDENT, "an enum member name");
        struct ud_node *id = new_node(p, N_IDENT);
        id->as.sval = tok_str(m);
        nl_push(&n->as.enumdef.names, id);
        struct ud_node *val = NULL;
        if (match(p, T_ASSIGN)) val = parse_expression(p);
        nl_push(&n->as.enumdef.vals, val); /* NULL => auto-increment */
        match(p, T_COMMA); /* comma between members is optional */
    }
    expect(p, T_END, "'end' to close the enum");
    return n;
}

/* -------- require(): compile-time module include --------
 *
 * `require("lib.ud")` at the top level lexes and parses that file's top-level
 * declarations and splices them straight into the current program, so a built
 * .ldx stays self-contained. Paths resolve relative to the requiring file's
 * directory; a registry of canonical paths dedupes repeats and breaks cycles. */

#define UD_REQUIRE_MAX 512
static char *g_required[UD_REQUIRE_MAX];
static int   g_required_count;

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (!d) ud_error(UDE_INTERNAL, 0, "out of memory");
    memcpy(d, s, n);
    return d;
}

/* Directory prefix of `path`, including the trailing separator, or "" if none. */
static char *dir_prefix(const char *path) {
    if (!path) return xstrdup("");
    size_t n = strlen(path);
    long sep = -1;
    for (size_t i = 0; i < n; i++)
        if (path[i] == '/' || path[i] == '\\') sep = (long)i;
    if (sep < 0) return xstrdup("");
    char *d = (char *)malloc((size_t)sep + 2);
    if (!d) ud_error(UDE_INTERNAL, 0, "out of memory");
    memcpy(d, path, (size_t)sep + 1);
    d[sep + 1] = '\0';
    return d;
}

static int path_is_absolute(const char *p) {
    if (!p || !p[0]) return 0;
    if (p[0] == '/' || p[0] == '\\') return 1;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':')
        return 1;
    return 0;
}

/* Join base_dir + rel (or just rel if absolute). Caller frees. */
static char *resolve_path(const char *base_dir, const char *rel) {
    if (path_is_absolute(rel) || !base_dir || !base_dir[0]) return xstrdup(rel);
    size_t a = strlen(base_dir), b = strlen(rel);
    char *out = (char *)malloc(a + b + 1);
    if (!out) ud_error(UDE_INTERNAL, 0, "out of memory");
    memcpy(out, base_dir, a);
    memcpy(out + a, rel, b + 1);
    return out;
}

/* A normalized absolute path used as the dedup key. Caller frees. */
static char *canonical_path(const char *path) {
#ifdef _WIN32
    char *full = _fullpath(NULL, path, 0);
#else
    char *full = realpath(path, NULL);
#endif
    if (full) return full;      /* malloc'd by the C library */
    return xstrdup(path);       /* fall back to the joined path as-is */
}

static int require_seen(const char *key) {
    for (int i = 0; i < g_required_count; i++)
        if (strcmp(g_required[i], key) == 0) return 1;
    return 0;
}

static char *require_read_file(const char *path, int line) {
    FILE *f = fopen(path, "rb");
    if (!f) ud_error(UDE_IO, line, "could not open required file '%s'", path);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); ud_error(UDE_IO, line, "could not read required file '%s'", path); }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); ud_error(UDE_INTERNAL, line, "out of memory"); }
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static struct ud_node *parse_toplevel(struct parser *p);
static void parse_into_program(struct parser *p, struct ud_node *program);

/* Handle a top-level `require("file")`: splice the module's declarations in. */
static void parse_require(struct parser *p, struct ud_node *program) {
    int line = cur(p)->line;
    advance(p); /* 'require' */
    int paren = match(p, T_LPAREN);
    if (!check(p, T_STRING))
        ud_error(UDE_SYNTAX, cur(p)->line,
                 "require expects a quoted file name, e.g. require(\"lib.ud\")");
    struct ud_string *rel = make_string_literal(cur(p));
    advance(p);
    if (paren) expect(p, T_RPAREN, "')' after the required file name");

    char *full = resolve_path(p->base_dir, rel->chars);
    char *key  = canonical_path(full);
    free(full);
    if (require_seen(key)) { free(key); return; }   /* already included / cycle */
    if (g_required_count >= UD_REQUIRE_MAX)
        ud_error(UDE_INTERNAL, line, "too many required files (limit %d)", UD_REQUIRE_MAX);
    g_required[g_required_count++] = key;            /* registry owns key */

    char *src = require_read_file(key, line);
    int ntok = 0;
    struct ud_token *toks = ud_lex(src, &ntok);
    char *subdir = dir_prefix(key);

    struct parser sub;
    sub.toks = toks; sub.count = ntok; sub.pos = 0; sub.base_dir = subdir;
    parse_into_program(&sub, program);

    free(subdir);
    free(toks);
    free(src);
    /* `key` stays alive in the registry; freed when the top-level parse ends. */
}

static struct ud_node *parse_toplevel(struct parser *p) {
    int t = cur(p)->type;
    if (t == T_CONST) return parse_const(p);
    if (t == T_ENUM)  return parse_enum(p);
    if (is_type_kw(t)) {
        /* `int function name(...)`  -- typed return */
        int rtype = decltype_of(advance(p)->type);
        if (!check(p, T_FUNCTION))
            ud_error(UDE_SYNTAX, cur(p)->line,
                     "at the top level, '%s' must begin a typed function "
                     "(e.g. %s function name())", decltype_name(rtype),
                     decltype_name(rtype));
        return parse_function(p, 1, rtype);
    }
    if (t == T_FUNCTION) return parse_function(p, 0, DT_NONE);
    if (t == T_STRUCT)   return parse_struct(p);
    ud_error(UDE_SYNTAX, cur(p)->line,
             "the top level of a UD file may only contain function and struct "
             "definitions; put runnable code inside 'int function entry()'");
    return NULL;
}

/* Drain a parser's top-level items into `program`, expanding require() inline. */
static void parse_into_program(struct parser *p, struct ud_node *program) {
    while (!check(p, T_EOF)) {
        if (check(p, T_REQUIRE)) { parse_require(p, program); continue; }
        nl_push(&program->as.block, parse_toplevel(p));
    }
}

struct ud_node *ud_parse(struct ud_token *toks, int count, const char *path) {
    g_required_count = 0; /* fresh registry for this top-level parse */

    struct parser p;
    p.toks = toks;
    p.count = count;
    p.pos = 0;
    char *base = dir_prefix(path);
    p.base_dir = base;

    /* Record the main file so a module that requires it back is skipped. */
    if (path && g_required_count < UD_REQUIRE_MAX)
        g_required[g_required_count++] = canonical_path(path);

    struct ud_node *program = (struct ud_node *)ud_alloc(sizeof(struct ud_node));
    memset(program, 0, sizeof(*program));
    program->kind = N_BLOCK;
    program->line = 1;
    nl_init(&program->as.block);
    parse_into_program(&p, program);

    free(base);
    for (int i = 0; i < g_required_count; i++) free(g_required[i]);
    g_required_count = 0;
    return program;
}
