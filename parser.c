/* parser.c -- UD parser implementation. */
#include "parser.h"
#include "errors.h"

#include <stdlib.h>
#include <string.h>

struct parser {
    struct ud_token *toks;
    int count;
    int pos;
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
            struct ud_token *name = expect(p, T_IDENT, "a field or method name");
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
         check(p,T_GT)||check(p,T_LE)||check(p,T_GE))

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

static struct ud_node *parse_expression(struct parser *p) {
    return parse_or(p);
}

/* -------- statements -------- */

static int is_assign_op(int type) {
    return type == T_ASSIGN || type == T_PLUSEQ || type == T_MINUSEQ ||
           type == T_STAREQ || type == T_SLASHEQ || type == T_PERCENTEQ;
}

static int is_block_end(struct parser *p) {
    int t = cur(p)->type;
    return t == T_END || t == T_ELSE || t == T_ELSEIF ||
           t == T_UNLESS || t == T_EOF;
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
        case T_IF:    return parse_if(p);
        case T_WHILE: return parse_while(p);
        case T_FOR:   return parse_for(p);
        case T_RETURN: {
            struct ud_node *n = new_node(p, N_RETURN);
            advance(p);
            n->as.expr = is_block_end(p) ? NULL : parse_expression(p);
            return n;
        }
        case T_BREAK:    { struct ud_node *n = new_node(p, N_BREAK);    advance(p); return n; }
        case T_CONTINUE: { struct ud_node *n = new_node(p, N_CONTINUE); advance(p); return n; }
        default: break;
    }

    /* expression statement or assignment */
    struct ud_node *expr = parse_expression(p);
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

static struct ud_node *parse_function(struct parser *p, int has_ret, int rtype) {
    expect(p, T_FUNCTION, "'function'");
    struct ud_token *name = expect(p, T_IDENT, "a function name");
    struct ud_node *n = new_node(p, N_FUNC);
    n->as.func.name = tok_str(name);
    n->as.func.has_ret = has_ret;
    n->as.func.rtype = (uint8_t)rtype;
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

static struct ud_node *parse_toplevel(struct parser *p) {
    int t = cur(p)->type;
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

struct ud_node *ud_parse(struct ud_token *toks, int count) {
    struct parser p;
    p.toks = toks;
    p.count = count;
    p.pos = 0;

    struct ud_node *program = (struct ud_node *)ud_alloc(sizeof(struct ud_node));
    memset(program, 0, sizeof(*program));
    program->kind = N_BLOCK;
    program->line = 1;
    nl_init(&program->as.block);
    while (!check(&p, T_EOF))
        nl_push(&program->as.block, parse_toplevel(&p));
    return program;
}
