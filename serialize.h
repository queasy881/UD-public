/* serialize.h -- read and write compiled UD programs as .ldx bytecode.
 *
 * `ud build file.ud -o file.ldx` compiles the source and calls
 * ud_serialize_write(). `ud run file.ldx` calls ud_serialize_read() and hands
 * the reconstructed program to the exact same VM that `ud file.ud` uses.
 *
 * A .ldx holds only bytecode, constants and type tags -- never the original
 * source -- so it cannot be turned back into readable UD (or C). It is a
 * portable, little-endian container: the same file runs on any platform.
 */
#ifndef UD_SERIALIZE_H
#define UD_SERIALIZE_H

#include "vm.h"

/* Serialize a compiled program to `path`. Reports failures via ud_error(). */
void ud_serialize_write(struct ud_program *prog, const char *path);

/* Load a .ldx into `prog`, which must already be ud_program_init()'d with
 * ud_register_builtins() applied. Sets prog->entry. Reports failures via
 * ud_error() (UDE_BADBYTECODE / UDE_IO). */
void ud_serialize_read(struct ud_program *prog, const char *path);

#endif /* UD_SERIALIZE_H */
