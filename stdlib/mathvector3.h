// L^ (lhat) -- sample standard library: std.math.vector3.
//
// One function a host calls once, before lhat_program_check, the same as any
// of program.h's own registrations (05 の 8.7). Nothing here is required --
// a host that never calls this never sees a "math.vector3" module. The scalar
// functions are std.math (math.h); this is its proving ground for 8.9,
// kept as a module of its own under it.
//
// One type, on 05 の 8.9's value side:
//
// - std.math.vector3.Vector3 -- a host value (8.9), made by
//   std.math.vector3.new(x, y, z). Three f32 components in stack slots, no
//   heap, no lifetime. Fields x/y/z read and write directly;
//   +, -, * (by number^), dot, cross, length, normalized and tostring are
//   registered members. This is the type arithmetic runs on. Keeping one
//   is the language's Vector3.Box^ (box^ / get / set) -- nothing for a
//   library to provide.

#ifndef LHATSTDLIB_MATHVECTOR3_H
#define LHATSTDLIB_MATHVECTOR3_H

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_mathvector3_register(LhatProgram *program);

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_MATHVECTOR3_H
