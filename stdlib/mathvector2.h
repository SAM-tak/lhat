// L^ (lhat) -- sample standard library: std.math.vector2.
//
// One function a host calls once, before lhat_program_check, the same as any
// of program.h's own registrations (05 の 8.7). Nothing here is required --
// a host that never calls this never sees a "math.vector2" module. The
// two-component sibling of std.math.vector3 (mathvector3.h), kept as a
// module of its own under std.math.
//
// One type, on 05 の 8.9's value side:
//
// - std.math.vector2.Vector2 -- a host value (8.9), made by
//   std.math.vector2.new(x, y). Two f32 components in stack slots, no heap,
//   no lifetime. Fields x/y read and write directly; +, -, * (by number^),
//   dot, cross (the scalar z of the 3D cross), length, normalized and
//   tostring are registered members. Keeping one is the language's
//   Vector2.Box^ (box^ / get / set) -- nothing for a library to provide.

#ifndef LHATSTDLIB_MATHVECTOR2_H
#define LHATSTDLIB_MATHVECTOR2_H

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_mathvector2_register(LhatProgram *program);

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_MATHVECTOR2_H
