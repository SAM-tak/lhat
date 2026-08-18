// L^ (lhat) -- sample standard library: std.math.
//
// One function a host calls once, before lhat_program_check, the same as any
// of program.h's own registrations (05 の 8.7). Nothing here is required --
// a host that never calls this never sees a "math" module.
//
// One type, on 05 の 8.9's value side:
//
// - std.math.Vector3 -- a host value (8.9). Three f32 components in stack
//   slots, no heap, no lifetime. Fields x/y/z read and write directly;
//   +, -, * (by number^), dot, cross, length, normalized and tostring are
//   registered members. This is the type arithmetic runs on. Keeping one
//   is the language's Vector3.Box^ (box^ / get / set) -- nothing for a
//   library to provide.

#ifndef LHATSTDLIB_MATH_H
#define LHATSTDLIB_MATH_H

#include "lhat.h"

bool lhatstdlib_math_register(LhatProgram *program);

#endif  // LHATSTDLIB_MATH_H
