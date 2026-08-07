// L^ (lhat) -- sample standard library: std.io.
//
// One function a host calls once, before lhat_program_check, the same as any
// of program.h's own registrations (05 の 8.7). Nothing here is required --
// a host that never calls this never sees an "io" module.
//
// std.io.open answers std.io.File|std.io.IOError.NotFound|std.io.IOError.
// Denied|std.error.OutOfMemory -- read it with try^/catch^, or narrow with
// isa^ against std.io.File or an error kind (both supported). Naming
// std.error.OutOfMemory in an isa^ needs its own `import^ std.error` --
// 8.1's "the language hands out no names" applies to a name reached
// through another module's registration too.

#ifndef LHATSTDLIB_IO_H
#define LHATSTDLIB_IO_H

#include "lhat.h"

bool lhatstdlib_io_register(LhatProgram *program);

#endif  // LHATSTDLIB_IO_H
