// L^ (lhat) -- sample standard library: std.random.
//
// One function a host calls once, before lhat_program_check, the same as any
// of program.h's own registrations (05 の 8.7). Nothing here is required --
// a host that never calls this never sees a "random" module, the same way a
// host that leaves port/ out never sees the default allocator.

#ifndef LHATSTDLIB_RANDOM_H
#define LHATSTDLIB_RANDOM_H

#include "lhat.h"

bool lhatstdlib_random_register(LhatProgram *program);

#endif  // LHATSTDLIB_RANDOM_H
