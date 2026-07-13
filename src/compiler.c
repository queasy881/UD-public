/* compiler.c -- AST -> bytecode.
 *
 * Stack-based codegen. Locals live in the VM frame's slot window; a scope
 * stack tracks which names occupy which slots (block scoping). Globals are the
 * functions/structs registered up front. Loops keep break/continue patch lists
 * so both work inside while, while...unless, and for.
 */
#include "compiler.h"
#include "errors.h"
#include "lexer.h"

#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Compiler state                                                     */
/* ------------------------------------------------------------------ */

struct local {
    struct ud_string *name;
    int depth;
    uint8_t is_const;
};

/* Compile-time constant environment shared across a whole file: named
 * top-level constants and enum members are folded to literal values, so a
 * reference to one emits OP_CONST and no runtime global is needed. Keys are
 * interned strings ("MAX" for a const, "Color.RED" for an enum member). */
struct cconst_entry { struct ud_string *key; struct ud_value val; };
struct cconst {
    struct cconst_entry *items;
    int count, cap;
    struct ud_string **enums;  /* names known to be enums (for good errors) */
    int enum_count, enum_cap;
};

struct loop {
    int start;            /* ip to jump back to for the next iteration      */
    int base_local;       /* local_count when the body scope began          */
    int base_try;         /* try nesting depth when the loop began          */
    int *breaks;          /* patch list of break jump operand positions     */
    int break_count, break_cap;
    int *continues;       /* patch list of continue jump operand positions  */
    int continue_count, continue_cap;
    struct loop *prev;
};

struct compiler {
    struct ud_program *prog;
    struct ud_function *fn;   /* function currently being compiled */
    struct local locals[256];
    int local_count;
    int scope_depth;
    int try_depth;            /* number of `try` handlers armed at this point */
    int ret_dtype;            /* declared return type (DT_*) or DT_NONE */
    struct loop *loop;        /* innermost enclosing loop */
    struct cconst *cc;        /* file-wide const/enum environment */
};

/* ------------------------------------------------------------------ */
/* Chunk emission                                                     */
/* ------------------------------------------------------------------ */

static void emit_byte(struct compiler *c, uint8_t b, int line) {
    struct ud_function *f = c->fn;
    if (f->code_len + 1 > f->code_cap) {
        int nc = f->code_cap < 64 ? 64 : f->code_cap * 2;
        uint8_t *code = (uint8_t *)ud_alloc((size_t)nc);
        int *lines = (int *)ud_alloc(sizeof(int) * (size_t)nc);
        if (f->code_len) {
            memcpy(code, f->code, (size_t)f->code_len);
            memcpy(lines, f->lines, sizeof(int) * (size_t)f->code_len);
        }
        f->code = code;
        f->lines = lines;
        f->code_cap = nc;
    }
    f->lines[f->code_len] = line;
    f->code[f->code_len++] = b;
}

static void emit_u16(struct compiler *c, int v, int line) {
    emit_byte(c, (uint8_t)(v & 0xFF), line);
    emit_byte(c, (uint8_t)((v >> 8) & 0xFF), line);
}

static int add_const(struct compiler *c, struct ud_value v) {
    struct ud_function *f = c->fn;
    /* de-dup identical constants to keep chunks small */
    for (int i = 0; i < f->const_count; i++)
        if (ud_value_equal(f->consts[i], v) &&
            f->consts[i].type == v.type)
            return i;
    if (f->const_count + 1 > f->const_cap) {
        int nc = f->const_cap < 8 ? 8 : f->const_cap * 2;
        struct ud_value *cs =
            (struct ud_value *)ud_alloc(sizeof(struct ud_value) * (size_t)nc);
        if (f->const_count)
            memcpy(cs, f->consts, sizeof(struct ud_value) * (size_t)f->const_count);
        f->consts = cs;
        f->const_cap = nc;
    }
    f->consts[f->const_count] = v;
    return f->const_count++;
}

static void emit_const(struct compiler *c, struct ud_value v, int line) {
    emit_byte(c, OP_CONST, line);
    emit_u16(c, add_const(c, v), line);
}

/* emit a jump with a placeholder operand; return the operand position */
static int emit_jump(struct compiler *c, uint8_t op, int line) {
    emit_byte(c, op, line);
    emit_byte(c, 0xFF, line);
    emit_byte(c, 0xFF, line);
    return c->fn->code_len - 2;
}

static void patch_jump(struct compiler *c, int operand_pos) {
    int target = c->fn->code_len;
    int offset = target - (operand_pos + 2); /* forward distance */
    c->fn->code[operand_pos]     = (uint8_t)(offset & 0xFF);
    c->fn->code[operand_pos + 1] = (uint8_t)((offset >> 8) & 0xFF);
}

static void emit_loop(struct compiler *c, int loop_start, int line) {
    emit_byte(c, OP_LOOP, line);
    int offset = (c->fn->code_len + 2) - loop_start; /* backward distance */
    emit_u16(c, offset, line);
}

/* ------------------------------------------------------------------ */
/* Scopes and locals                                                  */
/* ------------------------------------------------------------------ */

static void begin_scope(struct compiler *c) { c->scope_depth++; }

static void end_scope(struct compiler *c) {
    c->scope_depth--;
    int popped = 0;
    while (c->local_count > 0 &&
           c->locals[c->local_count - 1].depth > c->scope_depth) {
        c->local_count--;
        popped++;
    }
    for (int i = 0; i < popped; i++) emit_byte(c, OP_POP, 0);
}

static int add_local(struct compiler *c, struct ud_string *name) {
    if (c->local_count >= 256)
        ud_error(UDE_INTERNAL, 0, "too many local variables in one function");
    int slot = c->local_count;
    c->locals[slot].name = name;
    c->locals[slot].depth = c->scope_depth;
    c->locals[slot].is_const = 0;
    c->local_count++;
    if (c->local_count > c->fn->num_slots) c->fn->num_slots = c->local_count;
    return slot;
}

/* reserve a nameless slot (for-loop bookkeeping) */
static int add_anon_local(struct compiler *c) {
    return add_local(c, NULL);
}

static int resolve_local(struct compiler *c, struct ud_string *name) {
    for (int i = c->local_count - 1; i >= 0; i--)
        if (c->locals[i].name == name) return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Loop bookkeeping                                                   */
/* ------------------------------------------------------------------ */

static void loop_add_break(struct compiler *c, int pos) {
    struct loop *lp = c->loop;
    if (lp->break_count + 1 > lp->break_cap) {
        int nc = lp->break_cap < 4 ? 4 : lp->break_cap * 2;
        int *b = (int *)ud_alloc(sizeof(int) * (size_t)nc);
        if (lp->break_count) memcpy(b, lp->breaks, sizeof(int) * (size_t)lp->break_count);
        lp->breaks = b; lp->break_cap = nc;
    }
    lp->breaks[lp->break_count++] = pos;
}

static void loop_add_continue(struct compiler *c, int pos) {
    struct loop *lp = c->loop;
    if (lp->continue_count + 1 > lp->continue_cap) {
        int nc = lp->continue_cap < 4 ? 4 : lp->continue_cap * 2;
        int *b = (int *)ud_alloc(sizeof(int) * (size_t)nc);
        if (lp->continue_count) memcpy(b, lp->continues, sizeof(int) * (size_t)lp->continue_count);
        lp->continues = b; lp->continue_cap = nc;
    }
    lp->continues[lp->continue_count++] = pos;
}

/* ------------------------------------------------------------------ */
/* Forward decls                                                      */
/* ------------------------------------------------------------------ */

static void compile_expr(struct compiler *c, struct ud_node *n);
static void compile_stmt(struct compiler *c, struct ud_node *n);
static void compile_block(struct compiler *c, struct ud_node *block);
static void compile_incr(struct compiler *c, struct ud_node *n);
static void compile_function_body(struct compiler *parent, struct ud_node *fnode,
                                  struct ud_function *fn);

/* ------------------------------------------------------------------ */
/* Compile-time constants (const + enum)                              */
/* ------------------------------------------------------------------ */

static struct ud_string *enum_key(struct ud_string *ename, struct ud_string *member) {
    return ud_str_concat(ud_str_concat(ename, ud_str_intern(".", 1)), member);
}

static int cconst_get(struct cconst *cc, struct ud_string *key, struct ud_value *out) {
    if (!cc) return 0;
    for (int i = 0; i < cc->count; i++)
        if (cc->items[i].key == key) { if (out) *out = cc->items[i].val; return 1; }
    return 0;
}

static void cconst_put(struct cconst *cc, struct ud_string *key,
                       struct ud_value v, int line) {
    if (cconst_get(cc, key, NULL))
        ud_error(UDE_SYNTAX, line, "'%s' is already defined", key->chars);
    if (cc->count + 1 > cc->cap) {
        int nc = cc->cap < 8 ? 8 : cc->cap * 2;
        struct cconst_entry *it =
            (struct cconst_entry *)ud_alloc(sizeof(*it) * (size_t)nc);
        if (cc->count) memcpy(it, cc->items, sizeof(*it) * (size_t)cc->count);
        cc->items = it; cc->cap = nc;
    }
    cc->items[cc->count].key = key;
    cc->items[cc->count].val = v;
    cc->count++;
}

static int cconst_is_enum(struct cconst *cc, struct ud_string *name) {
    if (!cc) return 0;
    for (int i = 0; i < cc->enum_count; i++)
        if (cc->enums[i] == name) return 1;
    return 0;
}

static void cconst_add_enum(struct cconst *cc, struct ud_string *name) {
    if (cc->enum_count + 1 > cc->enum_cap) {
        int nc = cc->enum_cap < 8 ? 8 : cc->enum_cap * 2;
        struct ud_string **e =
            (struct ud_string **)ud_alloc(sizeof(*e) * (size_t)nc);
        if (cc->enum_count) memcpy(e, cc->enums, sizeof(*e) * (size_t)cc->enum_count);
        cc->enums = e; cc->enum_cap = nc;
    }
    cc->enums[cc->enum_count++] = name;
}

/* Fold a binary op over two already-folded constant operands (mirrors the VM's
 * arithmetic exactly). Returns 0 if the combination isn't foldable. */
static int fold_binary(int op, struct ud_value a, struct ud_value b,
                       struct ud_value *out) {
    switch (op) {
        case T_PLUS: case T_MINUS: case T_STAR:
            if (!UD_IS_NUM(a) || !UD_IS_NUM(b)) return 0;
            if (UD_IS_INT(a) && UD_IS_INT(b)) {
                long long x = a.as.i, y = b.as.i;
                *out = ud_int(op == T_PLUS ? x + y : op == T_MINUS ? x - y : x * y);
            } else {
                double x = ud_as_number(a), y = ud_as_number(b);
                *out = ud_float(op == T_PLUS ? x + y : op == T_MINUS ? x - y : x * y);
            }
            return 1;
        case T_SLASH:
            if (!UD_IS_NUM(a) || !UD_IS_NUM(b)) return 0;
            if (UD_IS_INT(a) && UD_IS_INT(b)) {
                if (b.as.i == 0) return 0;
                *out = ud_int(a.as.i / b.as.i);
            } else {
                double y = ud_as_number(b); if (y == 0.0) return 0;
                *out = ud_float(ud_as_number(a) / y);
            }
            return 1;
        case T_PERCENT:
            if (!UD_IS_NUM(a) || !UD_IS_NUM(b)) return 0;
            if (UD_IS_INT(a) && UD_IS_INT(b)) {
                if (b.as.i == 0) return 0;
                *out = ud_int(a.as.i % b.as.i);
            } else {
                double y = ud_as_number(b); if (y == 0.0) return 0;
                *out = ud_float(fmod(ud_as_number(a), y));
            }
            return 1;
        case T_STARSTAR:
            if (!UD_IS_NUM(a) || !UD_IS_NUM(b)) return 0;
            if (UD_IS_INT(a) && UD_IS_INT(b) && b.as.i >= 0) {
                long long r = 1, base = a.as.i, e = b.as.i;
                while (e > 0) { if (e & 1) r *= base; base *= base; e >>= 1; }
                *out = ud_int(r);
            } else {
                *out = ud_float(pow(ud_as_number(a), ud_as_number(b)));
            }
            return 1;
        case T_AMP: case T_PIPE: case T_CARET: case T_SHL: case T_SHR: {
            if (!UD_IS_INT(a) || !UD_IS_INT(b)) return 0;
            long long x = a.as.i, y = b.as.i, r = 0;
            switch (op) {
                case T_AMP:   r = x & y;  break; case T_PIPE:  r = x | y; break;
                case T_CARET: r = x ^ y;  break; case T_SHL:   r = x << y; break;
                case T_SHR:   r = x >> y; break;
            }
            *out = ud_int(r);
            return 1;
        }
        case T_CONCAT:
            if (!UD_IS_STRING(a) || !UD_IS_STRING(b)) return 0;
            *out = ud_obj_val((struct ud_obj *)
                ud_str_concat(UD_AS_STRING(a), UD_AS_STRING(b)));
            return 1;
        case T_EQ: *out = ud_bool(ud_value_equal(a, b));  return 1;
        case T_NE: *out = ud_bool(!ud_value_equal(a, b)); return 1;
        case T_LT: case T_GT: case T_LE: case T_GE: {
            if (!UD_IS_NUM(a) || !UD_IS_NUM(b)) return 0;
            double x = ud_as_number(a), y = ud_as_number(b); int r = 0;
            switch (op) {
                case T_LT: r = x <  y; break; case T_GT: r = x >  y; break;
                case T_LE: r = x <= y; break; case T_GE: r = x >= y; break;
            }
            *out = ud_bool(r);
            return 1;
        }
        default: return 0;
    }
}

/* Evaluate a constant expression at compile time. Returns 0 if the expression
 * refers to anything not known at compile time. */
static int fold_const(struct cconst *cc, struct ud_node *n, struct ud_value *out) {
    switch (n->kind) {
        case N_INT:    *out = ud_int(n->as.ival);   return 1;
        case N_FLOAT:  *out = ud_float(n->as.fval);  return 1;
        case N_BOOL:   *out = ud_bool(n->as.ival ? 1 : 0); return 1;
        case N_NIL:    *out = ud_nil();              return 1;
        case N_STRING: *out = ud_obj_val((struct ud_obj *)n->as.sval); return 1;
        case N_IDENT:  return cconst_get(cc, n->as.sval, out);
        case N_FIELD: {
            struct ud_node *t = n->as.field.target;
            if (t->kind != N_IDENT) return 0;
            return cconst_get(cc, enum_key(t->as.sval, n->as.field.name), out);
        }
        case N_UNARY: {
            struct ud_value v;
            if (!fold_const(cc, n->as.unary.operand, &v)) return 0;
            if (n->as.unary.op == T_MINUS) {
                if (UD_IS_INT(v))   { *out = ud_int(-v.as.i);   return 1; }
                if (UD_IS_FLOAT(v)) { *out = ud_float(-v.as.d); return 1; }
                return 0;
            }
            if (n->as.unary.op == T_NOT) { *out = ud_bool(!ud_value_truthy(v)); return 1; }
            if (n->as.unary.op == T_TILDE) {
                if (!UD_IS_INT(v)) return 0;
                *out = ud_int(~v.as.i); return 1;
            }
            return 0;
        }
        case N_BINARY: {
            struct ud_value a, b;
            if (!fold_const(cc, n->as.binary.left, &a))  return 0;
            if (!fold_const(cc, n->as.binary.right, &b)) return 0;
            return fold_binary(n->as.binary.op, a, b, out);
        }
        default: return 0;
    }
}

/* map a binary token op to its arithmetic/comparison opcode */
static int binop_opcode(int tok) {
    switch (tok) {
        case T_PLUS: return OP_ADD;   case T_MINUS: return OP_SUB;
        case T_STAR: return OP_MUL;   case T_SLASH: return OP_DIV;
        case T_PERCENT: return OP_MOD; case T_STARSTAR: return OP_POW;
        case T_EQ: return OP_EQ;      case T_NE: return OP_NE;
        case T_LT: return OP_LT;      case T_GT: return OP_GT;
        case T_LE: return OP_LE;      case T_GE: return OP_GE;
        case T_AMP: return OP_BAND;   case T_PIPE: return OP_BOR;
        case T_CARET: return OP_BXOR; case T_SHL: return OP_SHL;
        case T_SHR: return OP_SHR;    case T_CONCAT: return OP_CONCAT;
        case T_IN: return OP_IN;
        default: return OP_HALT;
    }
}

static int coerce_opcode(int dtype) {
    switch (dtype) {
        case DT_INT:    return OP_TO_INT;
        case DT_FLOAT:  return OP_TO_FLOAT;
        case DT_BOOL:   return OP_TO_BOOL;
        case DT_STRING: return OP_TO_STRING;
        default:        return -1;
    }
}

static int cin_opcode(int dtype) {
    switch (dtype) {
        case DT_INT:   return OP_CIN_INT;
        case DT_FLOAT: return OP_CIN_FLOAT;
        case DT_BOOL:  return OP_CIN_BOOL;
        default:       return OP_CIN; /* string / none */
    }
}

/* Is this AST node a direct call to the builtin `cin`? */
static int is_cin_call(struct ud_node *n) {
    return n && n->kind == N_CALL &&
           n->as.call.callee->kind == N_IDENT &&
           strcmp(n->as.call.callee->as.sval->chars, "cin") == 0;
}

/* Compile the prompt argument of a cin call (or push nil if none). */
static void compile_cin_prompt(struct compiler *c, struct ud_node *call) {
    if (call->as.call.args.count > 0)
        compile_expr(c, call->as.call.args.items[0]);
    else
        emit_byte(c, OP_NIL, call->line);
}

/* ------------------------------------------------------------------ */
/* Expression compilation                                             */
/* ------------------------------------------------------------------ */

/* Names that map to conversion opcodes rather than real calls. */
static int conversion_opcode_for(const char *name) {
    if (strcmp(name, "int") == 0)    return OP_TO_INT;
    if (strcmp(name, "float") == 0)  return OP_TO_FLOAT;
    if (strcmp(name, "bool") == 0)   return OP_TO_BOOL;
    if (strcmp(name, "string") == 0) return OP_TO_STRING;
    return -1;
}

static void compile_variable_get(struct compiler *c, struct ud_string *name, int line) {
    int slot = resolve_local(c, name);
    if (slot >= 0) {
        emit_byte(c, OP_GET_LOCAL, line);
        emit_byte(c, (uint8_t)slot, line);
        return;
    }
    struct ud_value cv;
    if (cconst_get(c->cc, name, &cv)) { /* file-level named constant */
        emit_const(c, cv, line);
        return;
    }
    emit_byte(c, OP_GET_GLOBAL, line);
    emit_u16(c, add_const(c, ud_obj_val((struct ud_obj *)name)), line);
}

static void compile_call(struct compiler *c, struct ud_node *n) {
    struct ud_node *callee = n->as.call.callee;

    /* cin(...) used as a plain expression reads a line and yields a string. */
    if (is_cin_call(n)) {
        compile_cin_prompt(c, n);
        emit_byte(c, OP_CIN, n->line);
        return;
    }

    /* int()/float()/string()/bool() are conversion opcodes. */
    if (callee->kind == N_IDENT) {
        int conv = conversion_opcode_for(callee->as.sval->chars);
        if (conv >= 0) {
            if (n->as.call.args.count != 1)
                ud_error(UDE_ARGCOUNT, n->line,
                         "%s(...) takes exactly one value to convert",
                         callee->as.sval->chars);
            compile_expr(c, n->as.call.args.items[0]);
            emit_byte(c, (uint8_t)conv, n->line);
            return;
        }
    }

    compile_expr(c, callee);
    for (int i = 0; i < n->as.call.args.count; i++)
        compile_expr(c, n->as.call.args.items[i]);
    emit_byte(c, OP_CALL, n->line);
    emit_byte(c, (uint8_t)n->as.call.args.count, n->line);
}

static void compile_expr(struct compiler *c, struct ud_node *n) {
    switch (n->kind) {
        case N_INT:   emit_const(c, ud_int(n->as.ival), n->line); break;
        case N_FLOAT: emit_const(c, ud_float(n->as.fval), n->line); break;
        case N_BOOL:  emit_byte(c, n->as.ival ? OP_TRUE : OP_FALSE, n->line); break;
        case N_NIL:   emit_byte(c, OP_NIL, n->line); break;
        case N_STRING:
            emit_const(c, ud_obj_val((struct ud_obj *)n->as.sval), n->line);
            break;
        case N_IDENT:
            compile_variable_get(c, n->as.sval, n->line);
            break;

        case N_ARRAY:
            for (int i = 0; i < n->as.array.elems.count; i++)
                compile_expr(c, n->as.array.elems.items[i]);
            emit_byte(c, OP_ARRAY, n->line);
            emit_u16(c, n->as.array.elems.count, n->line);
            break;

        case N_DICT:
            for (int i = 0; i < n->as.dict.keys.count; i++) {
                compile_expr(c, n->as.dict.keys.items[i]);
                compile_expr(c, n->as.dict.vals.items[i]);
            }
            emit_byte(c, OP_DICT, n->line);
            emit_u16(c, n->as.dict.keys.count, n->line);
            break;

        case N_SET:
            for (int i = 0; i < n->as.set.elems.count; i++)
                compile_expr(c, n->as.set.elems.items[i]);
            emit_byte(c, OP_SET, n->line);
            emit_u16(c, n->as.set.elems.count, n->line);
            break;

        case N_UNARY:
            compile_expr(c, n->as.unary.operand);
            if (n->as.unary.op == T_MINUS)      emit_byte(c, OP_NEG, n->line);
            else if (n->as.unary.op == T_NOT)   emit_byte(c, OP_NOT, n->line);
            else if (n->as.unary.op == T_TILDE) emit_byte(c, OP_BNOT, n->line);
            break;

        case N_BINARY:
            compile_expr(c, n->as.binary.left);
            compile_expr(c, n->as.binary.right);
            emit_byte(c, (uint8_t)binop_opcode(n->as.binary.op), n->line);
            break;

        case N_LOGICAL: {
            /* Short-circuit while leaving the deciding value on the stack.
             * DUP the left operand, test the copy (the jump pops it); the
             * surviving copy is the result on the short-circuit path. */
            compile_expr(c, n->as.logical.left);
            emit_byte(c, OP_DUP, n->line);
            int end = emit_jump(c,
                n->as.logical.op == T_AND ? OP_JUMP_IF_FALSE : OP_JUMP_IF_TRUE,
                n->line);
            emit_byte(c, OP_POP, n->line);           /* discard surviving left */
            compile_expr(c, n->as.logical.right);
            patch_jump(c, end);
            break;
        }

        case N_TERNARY: {
            compile_expr(c, n->as.ternary.cond);
            int to_else = emit_jump(c, OP_JUMP_IF_FALSE, n->line);
            compile_expr(c, n->as.ternary.then);
            int to_end = emit_jump(c, OP_JUMP, n->line);
            patch_jump(c, to_else);
            compile_expr(c, n->as.ternary.els);
            patch_jump(c, to_end);
            break;
        }

        case N_INCR: compile_incr(c, n); break;

        case N_CALL: compile_call(c, n); break;

        case N_INDEX:
            compile_expr(c, n->as.index.target);
            compile_expr(c, n->as.index.index);
            emit_byte(c, OP_INDEX_GET, n->line);
            break;

        case N_SLICE:
            compile_expr(c, n->as.slice.target);
            if (n->as.slice.start) compile_expr(c, n->as.slice.start);
            if (n->as.slice.stop)  compile_expr(c, n->as.slice.stop);
            emit_byte(c, OP_SLICE, n->line);
            emit_byte(c, (uint8_t)((n->as.slice.start ? 1 : 0) |
                                   (n->as.slice.stop ? 2 : 0)), n->line);
            break;

        case N_FIELD: {
            struct ud_node *t = n->as.field.target;
            /* Enum member access `Color.RED` folds to its constant value. */
            if (t->kind == N_IDENT && resolve_local(c, t->as.sval) < 0 &&
                cconst_is_enum(c->cc, t->as.sval)) {
                struct ud_value ev;
                if (!cconst_get(c->cc, enum_key(t->as.sval, n->as.field.name), &ev))
                    ud_error(UDE_FIELD, n->line, "enum %s has no member '%s'",
                             t->as.sval->chars, n->as.field.name->chars);
                emit_const(c, ev, n->line);
                break;
            }
            compile_expr(c, t);
            emit_byte(c, OP_FIELD_GET, n->line);
            emit_u16(c, add_const(c, ud_obj_val((struct ud_obj *)n->as.field.name)), n->line);
            break;
        }

        case N_METHOD:
            compile_expr(c, n->as.method.target);
            for (int i = 0; i < n->as.method.args.count; i++)
                compile_expr(c, n->as.method.args.items[i]);
            emit_byte(c, OP_INVOKE, n->line);
            emit_u16(c, add_const(c, ud_obj_val((struct ud_obj *)n->as.method.name)), n->line);
            emit_byte(c, (uint8_t)n->as.method.args.count, n->line);
            break;

        case N_LAMBDA: {
            /* Compile the anonymous function into its own chunk and push it as a
             * constant. The VM's ordinary OP_CALL path invokes function values
             * on the stack, so a lambda is a first-class value for free. */
            struct ud_string *lname = n->as.func.name
                                    ? n->as.func.name : ud_str_intern("lambda", 6);
            struct ud_function *fn = ud_function_new(lname);
            fn->arity = n->as.func.params.count;
            fn->param_types = n->as.func.ptypes;
            fn->return_type = n->as.func.has_ret ? n->as.func.rtype : 0xFF;
            compile_function_body(c, n, fn);
            emit_const(c, ud_obj_val((struct ud_obj *)fn), n->line);
            break;
        }

        default:
            ud_error(UDE_INTERNAL, n->line, "cannot compile this expression");
    }
}

/* ------------------------------------------------------------------ */
/* Assignment                                                         */
/* ------------------------------------------------------------------ */

static int compound_binop(int assign_op) {
    switch (assign_op) {
        case T_PLUSEQ:    return OP_ADD;
        case T_MINUSEQ:   return OP_SUB;
        case T_STAREQ:    return OP_MUL;
        case T_SLASHEQ:   return OP_DIV;
        case T_PERCENTEQ: return OP_MOD;
        default:          return OP_HALT;
    }
}

static void compile_assign(struct compiler *c, struct ud_node *n) {
    struct ud_node *target = n->as.assign.target;
    int op = n->as.assign.op;
    int line = n->line;

    if (target->kind == N_IDENT) {
        struct ud_string *name = target->as.sval;
        int slot = resolve_local(c, name);
        if (slot >= 0 && c->locals[slot].is_const)
            ud_error(UDE_SYNTAX, line, "cannot assign to constant '%s'", name->chars);
        if (op == T_ASSIGN) {
            if (slot >= 0) {
                compile_expr(c, n->as.assign.value);
                emit_byte(c, OP_SET_LOCAL, line);
                emit_byte(c, (uint8_t)slot, line);
                emit_byte(c, OP_POP, line);
            } else {
                /* first assignment declares a new block-scoped local */
                compile_expr(c, n->as.assign.value);
                add_local(c, name); /* value stays on the stack as the slot */
            }
        } else {
            if (slot < 0)
                ud_error(UDE_UNDEF_VAR, line,
                         "'%s' is used with '%s' before it was ever assigned",
                         name->chars, "a compound operator");
            emit_byte(c, OP_GET_LOCAL, line);
            emit_byte(c, (uint8_t)slot, line);
            compile_expr(c, n->as.assign.value);
            emit_byte(c, (uint8_t)compound_binop(op), line);
            emit_byte(c, OP_SET_LOCAL, line);
            emit_byte(c, (uint8_t)slot, line);
            emit_byte(c, OP_POP, line);
        }
    } else if (target->kind == N_INDEX) {
        if (op == T_ASSIGN) {
            compile_expr(c, target->as.index.target);
            compile_expr(c, target->as.index.index);
            compile_expr(c, n->as.assign.value);
            emit_byte(c, OP_INDEX_SET, line);
            emit_byte(c, OP_POP, line);
        } else {
            compile_expr(c, target->as.index.target);
            compile_expr(c, target->as.index.index);
            compile_expr(c, target->as.index.target);
            compile_expr(c, target->as.index.index);
            emit_byte(c, OP_INDEX_GET, line);
            compile_expr(c, n->as.assign.value);
            emit_byte(c, (uint8_t)compound_binop(op), line);
            emit_byte(c, OP_INDEX_SET, line);
            emit_byte(c, OP_POP, line);
        }
    } else { /* N_FIELD */
        int fc = add_const(c, ud_obj_val((struct ud_obj *)target->as.field.name));
        if (op == T_ASSIGN) {
            compile_expr(c, target->as.field.target);
            compile_expr(c, n->as.assign.value);
            emit_byte(c, OP_FIELD_SET, line);
            emit_u16(c, fc, line);
            emit_byte(c, OP_POP, line);
        } else {
            compile_expr(c, target->as.field.target);
            compile_expr(c, target->as.field.target);
            emit_byte(c, OP_FIELD_GET, line);
            emit_u16(c, fc, line);
            compile_expr(c, n->as.assign.value);
            emit_byte(c, (uint8_t)compound_binop(op), line);
            emit_byte(c, OP_FIELD_SET, line);
            emit_u16(c, fc, line);
            emit_byte(c, OP_POP, line);
        }
    }
}

/* ++target (prefix leaves the new value, postfix leaves the old value). */
static void compile_incr(struct compiler *c, struct ud_node *n) {
    struct ud_node *t = n->as.incr.target;
    int prefix = n->as.incr.is_prefix, line = n->line;
    if (t->kind == N_IDENT) {
        int slot = resolve_local(c, t->as.sval);
        if (slot < 0)
            ud_error(UDE_UNDEF_VAR, line,
                     "'%s' must be a local variable to use '++'", t->as.sval->chars);
        if (c->locals[slot].is_const)
            ud_error(UDE_SYNTAX, line, "cannot modify constant '%s' with '++'", t->as.sval->chars);
        if (!prefix) { emit_byte(c, OP_GET_LOCAL, line); emit_byte(c, (uint8_t)slot, line); }
        emit_byte(c, OP_GET_LOCAL, line); emit_byte(c, (uint8_t)slot, line);
        emit_const(c, ud_int(1), line);
        emit_byte(c, OP_ADD, line);
        emit_byte(c, OP_SET_LOCAL, line); emit_byte(c, (uint8_t)slot, line);
        if (!prefix) emit_byte(c, OP_POP, line); /* drop new, leave old */
    } else if (t->kind == N_INDEX) {
        if (!prefix) {
            compile_expr(c, t->as.index.target);
            compile_expr(c, t->as.index.index);
            emit_byte(c, OP_INDEX_GET, line); /* old, kept as result */
        }
        compile_expr(c, t->as.index.target);
        compile_expr(c, t->as.index.index);
        compile_expr(c, t->as.index.target);
        compile_expr(c, t->as.index.index);
        emit_byte(c, OP_INDEX_GET, line);
        emit_const(c, ud_int(1), line);
        emit_byte(c, OP_ADD, line);
        emit_byte(c, OP_INDEX_SET, line);
        if (!prefix) emit_byte(c, OP_POP, line);
    } else if (t->kind == N_FIELD) {
        int fc = add_const(c, ud_obj_val((struct ud_obj *)t->as.field.name));
        if (!prefix) {
            compile_expr(c, t->as.field.target);
            emit_byte(c, OP_FIELD_GET, line); emit_u16(c, fc, line);
        }
        compile_expr(c, t->as.field.target);
        compile_expr(c, t->as.field.target);
        emit_byte(c, OP_FIELD_GET, line); emit_u16(c, fc, line);
        emit_const(c, ud_int(1), line);
        emit_byte(c, OP_ADD, line);
        emit_byte(c, OP_FIELD_SET, line); emit_u16(c, fc, line);
        if (!prefix) emit_byte(c, OP_POP, line);
    } else {
        ud_error(UDE_SYNTAX, line, "'++' needs a variable, index, or field");
    }
}

/* ------------------------------------------------------------------ */
/* break / continue helpers                                           */
/* ------------------------------------------------------------------ */

static void emit_loop_exit_pops(struct compiler *c, int line) {
    /* disarm any try-handlers opened inside the loop before jumping out of it */
    int t = c->try_depth - c->loop->base_try;
    for (int i = 0; i < t; i++) emit_byte(c, OP_POP_TRY, line);
    int n = c->local_count - c->loop->base_local;
    for (int i = 0; i < n; i++) emit_byte(c, OP_POP, line);
}

/* ------------------------------------------------------------------ */
/* Statement compilation                                              */
/* ------------------------------------------------------------------ */

static void default_for_type(struct compiler *c, int dtype, int line) {
    switch (dtype) {
        case DT_INT:    emit_const(c, ud_int(0), line); break;
        case DT_FLOAT:  emit_const(c, ud_float(0.0), line); break;
        case DT_BOOL:   emit_byte(c, OP_FALSE, line); break;
        case DT_STRING: emit_const(c, ud_obj_val((struct ud_obj *)ud_str_intern("", 0)), line); break;
        default:        emit_byte(c, OP_NIL, line); break;
    }
}

static void compile_vardecl(struct compiler *c, struct ud_node *n) {
    int dtype = n->as.vardecl.dtype;
    struct ud_node *init = n->as.vardecl.init;
    if (init) {
        if (is_cin_call(init)) {
            compile_cin_prompt(c, init);
            emit_byte(c, (uint8_t)cin_opcode(dtype), n->line);
        } else {
            compile_expr(c, init);
            int co = coerce_opcode(dtype);
            if (co >= 0) emit_byte(c, (uint8_t)co, n->line);
        }
    } else {
        default_for_type(c, dtype, n->line);
    }
    int slot = add_local(c, n->as.vardecl.name); /* value stays on the stack as the slot */
    if (n->as.vardecl.is_const) c->locals[slot].is_const = 1;
}

static void compile_if(struct compiler *c, struct ud_node *n) {
    int end_jumps[64];
    int end_count = 0;
    int count = n->as.ifs.conds.count;
    for (int i = 0; i < count; i++) {
        compile_expr(c, n->as.ifs.conds.items[i]);
        int next = emit_jump(c, OP_JUMP_IF_FALSE, n->line);
        begin_scope(c);
        compile_block(c, n->as.ifs.bodies.items[i]);
        end_scope(c);
        if (end_count < 64) end_jumps[end_count++] = emit_jump(c, OP_JUMP, n->line);
        patch_jump(c, next);
    }
    if (n->as.ifs.elsebody) {
        begin_scope(c);
        compile_block(c, n->as.ifs.elsebody);
        end_scope(c);
    }
    for (int i = 0; i < end_count; i++) patch_jump(c, end_jumps[i]);
}

static void begin_loop(struct compiler *c, struct loop *lp, int start) {
    lp->start = start;
    lp->base_local = c->local_count;
    lp->base_try = c->try_depth;
    lp->breaks = NULL; lp->break_count = lp->break_cap = 0;
    lp->continues = NULL; lp->continue_count = lp->continue_cap = 0;
    lp->prev = c->loop;
    c->loop = lp;
}
static void patch_continues(struct compiler *c) {
    for (int i = 0; i < c->loop->continue_count; i++)
        patch_jump(c, c->loop->continues[i]);
}
static void patch_breaks(struct compiler *c) {
    for (int i = 0; i < c->loop->break_count; i++)
        patch_jump(c, c->loop->breaks[i]);
}
static void end_loop(struct compiler *c) { c->loop = c->loop->prev; }

static void compile_while(struct compiler *c, struct ud_node *n) {
    int loop_start = c->fn->code_len;
    compile_expr(c, n->as.whiles.cond);
    int exit_jump = emit_jump(c, OP_JUMP_IF_FALSE, n->line);

    struct loop lp;
    begin_scope(c);
    begin_loop(c, &lp, loop_start);
    compile_block(c, n->as.whiles.body);
    end_scope(c);

    patch_continues(c);              /* continue lands here */
    if (n->as.whiles.unless) {
        compile_expr(c, n->as.whiles.unless);
        int u = emit_jump(c, OP_JUMP_IF_TRUE, n->line); /* unless true -> exit */
        loop_add_break(c, u);
    }
    emit_loop(c, loop_start, n->line);

    patch_jump(c, exit_jump);
    patch_breaks(c);
    end_loop(c);
}

/* emit the "keep looping?" test for a numeric for. */
static void emit_for_test(struct compiler *c, int var_slot, int stop_slot,
                          int step_slot, struct ud_node *step, int line) {
    int const_dir = 0; /* 1 ascending, -1 descending, 0 unknown at compile time */
    if (!step) const_dir = 1;
    else if (step->kind == N_INT)   const_dir = step->as.ival > 0 ? 1 : (step->as.ival < 0 ? -1 : 0);
    else if (step->kind == N_FLOAT) const_dir = step->as.fval > 0 ? 1 : (step->as.fval < 0 ? -1 : 0);

    if (const_dir != 0) {
        emit_byte(c, OP_GET_LOCAL, line); emit_byte(c, (uint8_t)var_slot, line);
        emit_byte(c, OP_GET_LOCAL, line); emit_byte(c, (uint8_t)stop_slot, line);
        emit_byte(c, const_dir > 0 ? OP_LE : OP_GE, line);
        return;
    }
    /* runtime direction: (step>0) ? var<=stop : var>=stop */
    emit_byte(c, OP_GET_LOCAL, line); emit_byte(c, (uint8_t)step_slot, line);
    emit_const(c, ud_int(0), line);
    emit_byte(c, OP_GT, line);
    int to_desc = emit_jump(c, OP_JUMP_IF_FALSE, line);
    emit_byte(c, OP_GET_LOCAL, line); emit_byte(c, (uint8_t)var_slot, line);
    emit_byte(c, OP_GET_LOCAL, line); emit_byte(c, (uint8_t)stop_slot, line);
    emit_byte(c, OP_LE, line);
    int have = emit_jump(c, OP_JUMP, line);
    patch_jump(c, to_desc);
    emit_byte(c, OP_GET_LOCAL, line); emit_byte(c, (uint8_t)var_slot, line);
    emit_byte(c, OP_GET_LOCAL, line); emit_byte(c, (uint8_t)stop_slot, line);
    emit_byte(c, OP_GE, line);
    patch_jump(c, have);
}

static void compile_forrange(struct compiler *c, struct ud_node *n) {
    int line = n->line;
    begin_scope(c);
    compile_expr(c, n->as.forr.start);
    int var_slot = add_local(c, n->as.forr.var);
    compile_expr(c, n->as.forr.stop);
    int stop_slot = add_anon_local(c);
    if (n->as.forr.step) compile_expr(c, n->as.forr.step);
    else emit_const(c, ud_int(1), line);
    int step_slot = add_anon_local(c);

    int loop_start = c->fn->code_len;
    emit_for_test(c, var_slot, stop_slot, step_slot, n->as.forr.step, line);
    int exit_jump = emit_jump(c, OP_JUMP_IF_FALSE, line);

    struct loop lp;
    begin_scope(c);
    begin_loop(c, &lp, loop_start);
    compile_block(c, n->as.forr.body);
    end_scope(c);

    patch_continues(c);
    /* var = var + step */
    emit_byte(c, OP_GET_LOCAL, line); emit_byte(c, (uint8_t)var_slot, line);
    emit_byte(c, OP_GET_LOCAL, line); emit_byte(c, (uint8_t)step_slot, line);
    emit_byte(c, OP_ADD, line);
    emit_byte(c, OP_SET_LOCAL, line); emit_byte(c, (uint8_t)var_slot, line);
    emit_byte(c, OP_POP, line);
    emit_loop(c, loop_start, line);

    patch_jump(c, exit_jump);
    patch_breaks(c);
    end_loop(c);
    end_scope(c); /* pops var, stop, step */
}

static void compile_forin(struct compiler *c, struct ud_node *n) {
    int line = n->line;
    begin_scope(c);
    compile_expr(c, n->as.forin.iter);
    emit_byte(c, OP_ITER_KEYS, line); /* normalize dict/set to an element array */
    int iter_slot = add_anon_local(c);
    emit_const(c, ud_int(0), line);
    int idx_slot = add_anon_local(c);
    emit_byte(c, OP_NIL, line);
    int var_slot = add_local(c, n->as.forin.var);

    int loop_start = c->fn->code_len;
    emit_byte(c, OP_GET_LOCAL, line); emit_byte(c, (uint8_t)idx_slot, line);
    emit_byte(c, OP_GET_LOCAL, line); emit_byte(c, (uint8_t)iter_slot, line);
    emit_byte(c, OP_LEN, line);
    emit_byte(c, OP_LT, line);
    int exit_jump = emit_jump(c, OP_JUMP_IF_FALSE, line);

    /* var = iter[idx] */
    emit_byte(c, OP_GET_LOCAL, line); emit_byte(c, (uint8_t)iter_slot, line);
    emit_byte(c, OP_GET_LOCAL, line); emit_byte(c, (uint8_t)idx_slot, line);
    emit_byte(c, OP_INDEX_GET, line);
    emit_byte(c, OP_SET_LOCAL, line); emit_byte(c, (uint8_t)var_slot, line);
    emit_byte(c, OP_POP, line);

    struct loop lp;
    begin_scope(c);
    begin_loop(c, &lp, loop_start);
    compile_block(c, n->as.forin.body);
    end_scope(c);

    patch_continues(c);
    /* idx = idx + 1 */
    emit_byte(c, OP_GET_LOCAL, line); emit_byte(c, (uint8_t)idx_slot, line);
    emit_const(c, ud_int(1), line);
    emit_byte(c, OP_ADD, line);
    emit_byte(c, OP_SET_LOCAL, line); emit_byte(c, (uint8_t)idx_slot, line);
    emit_byte(c, OP_POP, line);
    emit_loop(c, loop_start, line);

    patch_jump(c, exit_jump);
    patch_breaks(c);
    end_loop(c);
    end_scope(c); /* pops iter, idx, var */
}

/* a, b, c = <array>  -- destructure an array (from a multi-return call or a
 * literal) into several targets. The source array is stashed in an anonymous
 * local; each target pulls its element by index. */
static void compile_multivardecl(struct compiler *c, struct ud_node *n) {
    int line = n->line;
    int ntargets = n->as.multi.names.count;
    compile_expr(c, n->as.multi.init);   /* source array stays on the stack */
    int src = add_anon_local(c);
    for (int i = 0; i < ntargets; i++) {
        struct ud_node *tgt = n->as.multi.names.items[i];
        if (tgt->kind == N_IDENT) {
            int slot = resolve_local(c, tgt->as.sval);
            if (slot >= 0 && c->locals[slot].is_const)
                ud_error(UDE_SYNTAX, line, "cannot assign to constant '%s'",
                         tgt->as.sval->chars);
            emit_byte(c, OP_GET_LOCAL, line); emit_byte(c, (uint8_t)src, line);
            emit_const(c, ud_int(i), line);
            emit_byte(c, OP_INDEX_GET, line);
            if (slot >= 0) {
                emit_byte(c, OP_SET_LOCAL, line); emit_byte(c, (uint8_t)slot, line);
                emit_byte(c, OP_POP, line);
            } else {
                add_local(c, tgt->as.sval); /* the element becomes the new local */
            }
        } else if (tgt->kind == N_INDEX) {
            compile_expr(c, tgt->as.index.target);
            compile_expr(c, tgt->as.index.index);
            emit_byte(c, OP_GET_LOCAL, line); emit_byte(c, (uint8_t)src, line);
            emit_const(c, ud_int(i), line);
            emit_byte(c, OP_INDEX_GET, line);
            emit_byte(c, OP_INDEX_SET, line);
            emit_byte(c, OP_POP, line);
        } else if (tgt->kind == N_FIELD) {
            int fc = add_const(c, ud_obj_val((struct ud_obj *)tgt->as.field.name));
            compile_expr(c, tgt->as.field.target);
            emit_byte(c, OP_GET_LOCAL, line); emit_byte(c, (uint8_t)src, line);
            emit_const(c, ud_int(i), line);
            emit_byte(c, OP_INDEX_GET, line);
            emit_byte(c, OP_FIELD_SET, line); emit_u16(c, fc, line);
            emit_byte(c, OP_POP, line);
        } else {
            ud_error(UDE_SYNTAX, line,
                     "each target on the left of '=' must be a name, index, or field");
        }
    }
}

/* try/catch. OP_TRY arms a handler pointing at the catch clause; on the normal
 * path OP_POP_TRY disarms it and a jump skips the handler. When an error unwinds
 * into the handler the VM has already pushed the error value, so a bound catch
 * variable simply *is* that fresh local. */
static void compile_try(struct compiler *c, struct ud_node *n) {
    int line = n->line;
    int baseline = c->local_count;         /* the catch var (if any) lands here */
    int has_var = n->as.trycatch.errname != NULL;
    if (has_var && baseline >= 0xFF)
        ud_error(UDE_INTERNAL, line, "too many locals surrounding a try/catch");

    emit_byte(c, OP_TRY, line);
    emit_byte(c, (uint8_t)(has_var ? baseline : 0xFF), line);
    int catch_off_pos = c->fn->code_len;   /* u16 patched to the catch clause */
    emit_byte(c, 0xFF, line);
    emit_byte(c, 0xFF, line);

    c->try_depth++;
    begin_scope(c);
    compile_block(c, n->as.trycatch.body);
    end_scope(c);
    c->try_depth--;

    emit_byte(c, OP_POP_TRY, line);        /* normal completion disarms it */
    int over = emit_jump(c, OP_JUMP, line);/* and skips the catch clause      */

    patch_jump(c, catch_off_pos);          /* OP_TRY's offset -> here          */
    begin_scope(c);
    if (has_var)
        add_local(c, n->as.trycatch.errname); /* pushed error becomes the local */
    else
        emit_byte(c, OP_POP, line);           /* no variable: drop the error    */
    compile_block(c, n->as.trycatch.handler);
    end_scope(c);

    patch_jump(c, over);                    /* normal path resumes after catch */
}

static void compile_throw(struct compiler *c, struct ud_node *n) {
    compile_expr(c, n->as.expr);
    emit_byte(c, OP_THROW, n->line);
}

static void compile_stmt(struct compiler *c, struct ud_node *n) {
    switch (n->kind) {
        case N_EXPRSTMT:
            compile_expr(c, n->as.expr);
            emit_byte(c, OP_POP, n->line);
            break;
        case N_VARDECL: compile_vardecl(c, n); break;
        case N_MULTIVARDECL: compile_multivardecl(c, n); break;
        case N_ASSIGN:  compile_assign(c, n); break;
        case N_IF:      compile_if(c, n); break;
        case N_WHILE:   compile_while(c, n); break;
        case N_FORRANGE:compile_forrange(c, n); break;
        case N_FORIN:   compile_forin(c, n); break;
        case N_RETURN:
            if (n->as.expr) {
                compile_expr(c, n->as.expr);
                int co = coerce_opcode(c->ret_dtype);
                if (co >= 0) emit_byte(c, (uint8_t)co, n->line);
            } else if (c->ret_dtype != DT_NONE) {
                default_for_type(c, c->ret_dtype, n->line);
            } else {
                emit_byte(c, OP_NIL, n->line);
            }
            emit_byte(c, OP_RETURN, n->line);
            break;
        case N_BREAK:
            if (!c->loop) ud_error(UDE_SYNTAX, n->line, "'break' is only allowed inside a loop");
            emit_loop_exit_pops(c, n->line);
            loop_add_break(c, emit_jump(c, OP_JUMP, n->line));
            break;
        case N_CONTINUE:
            if (!c->loop) ud_error(UDE_SYNTAX, n->line, "'continue' is only allowed inside a loop");
            emit_loop_exit_pops(c, n->line);
            loop_add_continue(c, emit_jump(c, OP_JUMP, n->line));
            break;
        case N_BLOCK:
            begin_scope(c);
            compile_block(c, n);
            end_scope(c);
            break;
        case N_TRY:   compile_try(c, n); break;
        case N_THROW: compile_throw(c, n); break;
        default:
            ud_error(UDE_INTERNAL, n->line, "cannot compile this statement");
    }
}

static void compile_block(struct compiler *c, struct ud_node *block) {
    for (int i = 0; i < block->as.block.count; i++)
        compile_stmt(c, block->as.block.items[i]);
}

/* ------------------------------------------------------------------ */
/* Functions and top level                                            */
/* ------------------------------------------------------------------ */

static void compile_function_body(struct compiler *parent, struct ud_node *fnode,
                                  struct ud_function *fn) {
    struct compiler c;
    c.prog = parent->prog;
    c.fn = fn;
    c.local_count = 0;
    c.scope_depth = 0;
    c.try_depth = 0;
    c.ret_dtype = fnode->as.func.has_ret ? fnode->as.func.rtype : DT_NONE;
    c.loop = NULL;
    c.cc = parent->cc;

    /* parameters occupy the first slots */
    for (int i = 0; i < fnode->as.func.params.count; i++)
        add_local(&c, fnode->as.func.params.items[i]->as.sval);

    compile_block(&c, fnode->as.func.body);

    /* implicit return when control falls off the end */
    if (c.ret_dtype != DT_NONE) default_for_type(&c, c.ret_dtype, fnode->line);
    else emit_byte(&c, OP_NIL, fnode->line);
    emit_byte(&c, OP_RETURN, fnode->line);
}

void ud_compile(struct ud_node *program, struct ud_program *prog) {
    struct cconst cc;
    memset(&cc, 0, sizeof(cc));

    struct compiler top;
    top.prog = prog;
    top.fn = NULL;
    top.local_count = 0;
    top.scope_depth = 0;
    top.try_depth = 0;
    top.ret_dtype = DT_NONE;
    top.loop = NULL;
    top.cc = &cc;

    /* pass 1: register every function and struct as a global so calls resolve
     * regardless of source order; fold enums and file-level consts to values. */
    for (int i = 0; i < program->as.block.count; i++) {
        struct ud_node *d = program->as.block.items[i];
        if (d->kind == N_ENUM) {
            cconst_add_enum(&cc, d->as.enumdef.name);
            long long next = 0;
            for (int j = 0; j < d->as.enumdef.names.count; j++) {
                struct ud_string *mem = d->as.enumdef.names.items[j]->as.sval;
                struct ud_node *ve = d->as.enumdef.vals.items[j];
                long long value;
                if (ve) {
                    struct ud_value v;
                    if (!fold_const(&cc, ve, &v) || !UD_IS_INT(v))
                        ud_error(UDE_SYNTAX, d->line,
                                 "enum %s member '%s' must be a constant integer",
                                 d->as.enumdef.name->chars, mem->chars);
                    value = v.as.i;
                } else {
                    value = next;
                }
                cconst_put(&cc, enum_key(d->as.enumdef.name, mem), ud_int(value), d->line);
                next = value + 1;
            }
            continue;
        }
        if (d->kind == N_VARDECL && d->as.vardecl.is_const) {
            struct ud_value v;
            if (!fold_const(&cc, d->as.vardecl.init, &v))
                ud_error(UDE_SYNTAX, d->line,
                         "the value of constant '%s' must be a constant expression",
                         d->as.vardecl.name->chars);
            cconst_put(&cc, d->as.vardecl.name, v, d->line);
            continue;
        }
        if (d->kind == N_FUNC) {
            struct ud_function *fn = ud_function_new(d->as.func.name);
            fn->arity = d->as.func.params.count;
            fn->param_types = d->as.func.ptypes;
            fn->return_type = d->as.func.has_ret ? d->as.func.rtype : 0xFF;
            ud_map_set(&prog->globals, d->as.func.name,
                       ud_obj_val((struct ud_obj *)fn));
            ud_program_add_function(prog, fn);
        } else if (d->kind == N_STRUCT) {
            int fc = d->as.strct.fields.count;
            struct ud_structdef *sd = ud_structdef_new(d->as.strct.name, fc);
            for (int j = 0; j < fc; j++) {
                sd->field_names[j] = d->as.strct.fields.items[j]->as.sval;
                sd->field_types[j] = d->as.strct.ftypes[j];
            }
            ud_map_set(&prog->globals, d->as.strct.name,
                       ud_obj_val((struct ud_obj *)sd));
            ud_program_add_struct(prog, sd);
        }
    }

    /* pass 2: compile each function body into its chunk. */
    int fidx = 0;
    for (int i = 0; i < program->as.block.count; i++) {
        struct ud_node *d = program->as.block.items[i];
        if (d->kind != N_FUNC) continue;
        struct ud_function *fn = prog->functions[fidx++];
        compile_function_body(&top, d, fn);
    }

    /* the mandatory entry point */
    struct ud_string *entry_name = ud_str_intern("entry", 5);
    struct ud_value ev;
    if (!ud_map_get(&prog->globals, entry_name, &ev) || !UD_IS_FUNCTION(ev)) {
        ud_error(UDE_NO_ENTRY, 0,
                 "every UD program needs an 'int function entry()' as its "
                 "starting point, like main() in C");
    }
    struct ud_function *entry = UD_AS_FUNCTION(ev);
    if (entry->arity != 0)
        ud_error(UDE_NO_ENTRY, 0,
                 "entry() must take no parameters (found %d)", entry->arity);
    prog->entry = entry;
}
