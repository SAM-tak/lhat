// L^ (lhat) -- sample standard library: std.math.quaternion.
// Quaternion is an f32 host value with writable x/y/z/w fields; w is the
// scalar part. new(x, y, z, w), +, binary/unary -, * (Hamilton product or
// number^ scaling, either order), dot^ / ⋅, conjugate, length, normalized and
// tostring and slerp(other, t) are registered. slerp normalizes nonzero
// inputs, takes the shortest arc, and does not clamp t; a zero input uses a
// linear blend. A zero value stays zero when normalized.
// Register std.math.vector3 first. fromTo(from: Vector3, to: Vector3) and
// fromAxisAngle(axis: Vector3, radians) return Quaternion|nil^ (nil^ for a
// zero direction); q.rotate(v: Vector3) returns a Vector3. Opposite from/to
// directions choose a deterministic perpendicular axis. Rotation normalizes
// nonzero quaternions and treats a zero quaternion as the identity.
// The module's identity() function returns a fresh (0, 0, 0, 1) value.
#ifndef LHATSTDLIB_MATHQUATERNION_H
#define LHATSTDLIB_MATHQUATERNION_H

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_mathquaternion_register(LhatProgram *program);

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_MATHQUATERNION_H
