// L^ (lhat) -- sample standard library: std.debug.
//
// One function a host calls once, before lhat_program_check, the same as any
// of program.h's own registrations (05 の 8.7). Nothing here is required --
// a host that never calls this never sees a "debug" module.
//
// std.debug.log is 02 の 10.6's 脱法関数 -- an f^ that writes. 10.6 names it
// as the one exception the f^ constraints allow, and stdlib/debug.c says why
// it is one rather than a hole. A host that wants the guarantee back leaves
// this module out of its registrations; nothing else in stdlib/ depends on
// it.

#ifndef LHATSTDLIB_DEBUG_H
#define LHATSTDLIB_DEBUG_H

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_debug_register(LhatProgram *program);

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_DEBUG_H
