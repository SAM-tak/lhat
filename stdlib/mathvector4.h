// L^ (lhat) -- sample standard library: std.math.vector4.
// Vector4 is an f32 host value with writable x/y/z/w fields. new(x, y, z, w),
// +, binary/unary -, * (number^ scaling in either order, or component-wise
// multiplication with another Vector4), / (number^ or component-wise by
// Vector4), dot^ / ⋅, length, normalized, lerp(other, t) (unclamped) and
// tostring follow Vector2/Vector3. Zero stays zero when
// normalized; there is no 4D cross product. The language supplies Box^.
// Module functions zero() and one() return fresh Vector4 values.
#ifndef LHATSTDLIB_MATHVECTOR4_H
#define LHATSTDLIB_MATHVECTOR4_H

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_mathvector4_register(LhatProgram *program);

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_MATHVECTOR4_H
