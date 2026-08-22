// L^ (lhat) -- sample standard library: std.random.
//
// One function a host calls once, before lhat_program_check, the same as any
// of program.h's own registrations (05 の 8.7). Nothing here is required --
// a host that never calls this never sees a "random" module, the same way a
// host that leaves port/ out never sees the default allocator.
//
// std.random.make answers std.random.Random|std.error.OutOfMemory -- read
// it with try^/catch^, or narrow with isa^ against std.random.Random or
// std.error.OutOfMemory (05 の 8.8's hostdata path and 04 の 2.4's error
// kind path, both supported). Naming std.error.OutOfMemory in an isa^
// (rather than just letting try^/catch^ read it) needs its own
// `import^ std.error` -- 8.1's "the language hands out no names" applies
// to a name reached through another module's registration too.

#ifndef LHATSTDLIB_RANDOM_H
#define LHATSTDLIB_RANDOM_H

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_random_register(LhatProgram *program);

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_RANDOM_H
