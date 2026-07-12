/* serialize.h -- read and write compiled UD programs as .ldx bytecode.
 *
 * `ud build file.ud -o file.ldx` compiles the source and calls
 * ud_serialize_write(). `ud run file.ldx` calls ud_serialize_read() and hands
 * the reconstructed program to the exact same VM that `ud file.ud` uses.
 *
 * A .ldx holds only bytecode, constants and type tags -- never the original
 * source -- so it cannot be turned back into readable UD (or C). The payload is
 * a portable, little-endian container: the same bytes run on any platform.
 *
 * A .ldx may also be "standalone": the portable payload with a full copy of the
 * native `ud` runtime fused in front of it, so the file self-executes without
 * `ud` installed while `ud run` still accepts it. See serialize.c for the exact
 * on-disk layout.
 */
#ifndef UD_SERIALIZE_H
#define UD_SERIALIZE_H

#include "vm.h"

/* Serialize a compiled program to `path` as a thin, portable .ldx. Reports
 * failures via ud_error(). */
void ud_serialize_write(struct ud_program *prog, const char *path);

/* Serialize a compiled program to `path` as a standalone .ldx: a copy of the
 * `ud` runtime at `runtime_path` with the payload fused on. The result is a
 * runnable executable that is still a valid .ldx. Reports failures via
 * ud_error(). */
void ud_serialize_write_standalone(struct ud_program *prog, const char *path,
                                   const char *runtime_path);

/* Load a .ldx (thin or standalone) into `prog`, which must already be
 * ud_program_init()'d with ud_register_builtins() applied. Sets prog->entry.
 * Reports failures via ud_error() (UDE_BADBYTECODE / UDE_IO). */
void ud_serialize_read(struct ud_program *prog, const char *path);

/* True if `path` is a standalone (fused) .ldx / executable carrying a payload.
 * Never raises; any read failure reports "no". */
int ud_serialize_is_standalone(const char *path);

#endif /* UD_SERIALIZE_H */
