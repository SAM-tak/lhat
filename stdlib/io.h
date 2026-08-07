// L^ (lhat) -- sample standard library: std.io.
//
// One function a host calls once, before lhat_program_check, the same as any
// of program.h's own registrations (05 の 8.7). Nothing here is required --
// a host that never calls this never sees an "io" module.
//
// std.io.open answers std.io.File|std.io.IOError.NotFound|std.io.IOError.
// Denied -- read it with try^/catch^, not isa^: compile_isa (vm.c) only
// resolves an error kind (04 の 6.1), not a hostdata type like File, so
// `x isa^ std.io.File` fails to compile (LHAT_COMPILE_UNSUPPORTED).

#ifndef LHATSTDLIB_IO_H
#define LHATSTDLIB_IO_H

#include "lhat.h"

bool lhatstdlib_io_register(LhatProgram *program);

#endif  // LHATSTDLIB_IO_H
