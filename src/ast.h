/* ast.h -- UD abstract syntax tree.
 *
 * One tagged node type (`struct ud_node`) with a `kind` discriminator and a
 * union of per-kind payloads. Nodes and node lists are arena-allocated, so the
 * whole tree is thrown away for free after the compiler has lowered it to
 * bytecode. No typedefs: everything is spelled `struct ud_node`.
 */
#ifndef UD_AST_H
#define UD_AST_H

#include <stdint.h>
#include "speed.h"

/* Declared types on parameters, returns, fields and typed variable decls.
 * DT_NONE means "untyped" (dynamic). */
enum ud_decltype {
    DT_INT = 0,
    DT_FLOAT,
    DT_BOOL,
    DT_STRING,
    DT_NONE
};

enum ud_nodekind {
    /* expressions */
    N_INT, N_FLOAT, N_STRING, N_BOOL, N_NIL,
    N_IDENT, N_ARRAY, N_DICT, N_SET,
    N_BINARY, N_UNARY, N_LOGICAL, N_TERNARY, N_INCR,
    N_CALL, N_INDEX, N_SLICE, N_FIELD, N_METHOD, N_LAMBDA,
    /* statements */
    N_EXPRSTMT, N_VARDECL, N_ASSIGN, N_MULTIVARDECL,
    N_IF, N_WHILE, N_FORRANGE, N_FORIN,
    N_RETURN, N_BREAK, N_CONTINUE, N_BLOCK,
    N_TRY, N_THROW,
    /* declarations */
    N_FUNC, N_STRUCT, N_ENUM
};

struct ud_node;

struct ud_nodelist {
    struct ud_node **items;
    int count;
    int cap;
};

struct ud_node {
    uint8_t kind;
    int line;
    union {
        long long          ival;   /* N_INT   */
        double             fval;   /* N_FLOAT */
        struct ud_string  *sval;   /* N_STRING literal, or N_IDENT name */

        struct { struct ud_nodelist elems; } array;                 /* N_ARRAY */
        struct { struct ud_nodelist keys, vals; } dict;             /* N_DICT   */
        struct { struct ud_nodelist elems; } set;                   /* N_SET    */
        struct { int op; struct ud_node *left, *right; } binary;    /* N_BINARY: op is a token type */
        struct { int op; struct ud_node *operand; } unary;          /* N_UNARY  */
        struct { int op; struct ud_node *left, *right; } logical;   /* N_LOGICAL: T_AND / T_OR */
        struct { struct ud_node *cond, *then, *els; } ternary;      /* N_TERNARY */
        struct { struct ud_node *target; uint8_t is_prefix; } incr; /* N_INCR: ++target */

        struct { struct ud_node *callee; struct ud_nodelist args; } call;   /* N_CALL */
        struct { struct ud_node *target, *index; } index;                   /* N_INDEX */
        struct { struct ud_node *target, *start, *stop; } slice;            /* N_SLICE */
        struct { struct ud_node *target; struct ud_string *name; } field;   /* N_FIELD */
        struct { struct ud_node *target; struct ud_string *name;
                 struct ud_nodelist args; } method;                         /* N_METHOD */

        struct ud_node *expr;      /* N_EXPRSTMT value, N_RETURN value (may be NULL) */

        struct { uint8_t dtype; struct ud_node *init; struct ud_string *name;
                 int is_typed; uint8_t is_const; } vardecl;                 /* N_VARDECL */
        struct { int op; struct ud_node *target, *value; } assign;          /* N_ASSIGN: op token */

        struct { struct ud_nodelist conds; struct ud_nodelist bodies;
                 struct ud_node *elsebody; } ifs;                           /* N_IF */
        struct { struct ud_node *cond, *body, *unless; } whiles;            /* N_WHILE (+optional unless) */
        struct { struct ud_string *var; struct ud_node *start, *stop, *step, *body; } forr; /* N_FORRANGE */
        struct { struct ud_string *var; struct ud_node *iter, *body; } forin;              /* N_FORIN */

        struct ud_nodelist block;  /* N_BLOCK */

        struct { struct ud_string *name; struct ud_nodelist params; uint8_t *ptypes;
                 uint8_t rtype; int has_ret; struct ud_node *body; } func;  /* N_FUNC, N_LAMBDA */
        struct { struct ud_string *name; struct ud_nodelist fields;
                 uint8_t *ftypes; } strct;                                  /* N_STRUCT */

        struct { struct ud_nodelist names; struct ud_node *init;
                 int is_typed; uint8_t dtype; } multi;                      /* N_MULTIVARDECL: a, b = ... */
        struct { struct ud_node *body; struct ud_string *errname;
                 struct ud_node *handler; } trycatch;                       /* N_TRY */
        struct { struct ud_string *name; struct ud_nodelist names;
                 struct ud_nodelist vals; } enumdef;                        /* N_ENUM */
    } as;
};

#endif /* UD_AST_H */
