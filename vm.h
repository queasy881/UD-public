/* vm.h -- UD bytecode VM: opcodes, program container, and the run entry point.
 *
 * There is exactly ONE VM. `ud file.ud` compiles to bytecode and runs it in
 * memory; `ud build` compiles to the same bytecode and serializes it to a
 * .ldx file; `ud run` loads a .ldx and executes it here. Compiling and
 * interpreting therefore share this identical execution core.
 */
#ifndef UD_VM_H
#define UD_VM_H

#include "speed.h"

/* Bytecode. Operand widths are noted per op. u16 operands are little-endian
 * pairs of bytes; u8 operands are single bytes. */
enum ud_op {
    OP_CONST,          /* u16 const   : push constant                        */
    OP_NIL, OP_TRUE, OP_FALSE,
    OP_POP,
    OP_DUP,

    OP_GET_LOCAL,      /* u8 slot                                            */
    OP_SET_LOCAL,      /* u8 slot   : leaves value on stack                  */
    OP_GET_GLOBAL,     /* u16 name-const                                     */
    OP_SET_GLOBAL,     /* u16 name-const                                     */

    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW, OP_NEG,
    OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_NOT,
    OP_BAND, OP_BOR, OP_BXOR, OP_BNOT, OP_SHL, OP_SHR,
    OP_CONCAT,
    OP_LEN,            /* replace top with its length (int)                  */

    OP_JUMP,           /* u16 : ip += operand                                */
    OP_JUMP_IF_FALSE,  /* u16 : pop; if falsey ip += operand                 */
    OP_JUMP_IF_TRUE,   /* u16                                                */
    OP_LOOP,           /* u16 : ip -= operand                                */

    OP_CALL,           /* u8 argc                                            */
    OP_RETURN,         /* return top of stack from current function          */

    OP_ARRAY,          /* u16 count : build array from top `count` values    */
    OP_INDEX_GET,      /* pop index, container -> push element               */
    OP_INDEX_SET,      /* stack: container index value -> leaves value       */
    OP_SLICE,          /* u8 flags (bit0 has-start, bit1 has-stop)           */

    OP_INVOKE,         /* u16 method-const, u8 argc : method call            */
    OP_STRUCT_NEW,     /* u16 name-const, u8 argc : construct a struct        */
    OP_FIELD_GET,      /* u16 field-const                                    */
    OP_FIELD_SET,      /* u16 field-const : leaves value                     */

    OP_CIN,            /* pop prompt(or nil), read line -> push string       */
    OP_CIN_INT,        /* typed cin: strict integer                          */
    OP_CIN_FLOAT,      /* typed cin: number (int auto-promotes)              */
    OP_CIN_BOOL,       /* typed cin: boolean                                 */

    OP_TO_INT, OP_TO_FLOAT, OP_TO_BOOL, OP_TO_STRING, /* explicit conversions */

    OP_PRINT,          /* debug helper (unused by codegen)                   */
    OP_HALT
};

/* A whole compiled program: the global table (functions + natives + struct
 * defs), the entry() function, and flat lists of every function/struct so the
 * serializer can walk them. */
struct ud_program {
    struct ud_map globals;
    struct ud_function *entry;

    struct ud_function **functions;
    int function_count;
    int function_cap;

    struct ud_structdef **structs;
    int struct_count;
    int struct_cap;
};

void ud_program_init(struct ud_program *prog);
void ud_program_add_function(struct ud_program *prog, struct ud_function *fn);
void ud_program_add_struct(struct ud_program *prog, struct ud_structdef *sd);

/* Install cout / len / type / etc. into the program's global table. */
void ud_register_builtins(struct ud_program *prog);

/* Run entry() and return its int result (used as the process exit code). */
int ud_vm_run(struct ud_program *prog);

#endif /* UD_VM_H */
