// L^ (lhat) -- sample standard library: std.math.
//
// One function a host calls once, before lhat_program_check, the same as any
// of program.h's own registrations (05 の 8.7). Nothing here is required --
// a host that never calls this never sees a "math" module.
//
// Two types, one value and one reference, which is 05 の 8.9's whole axis:
//
// - std.math.LVector3 -- a host value (8.9). Three f32 components in stack
//   slots, no heap, no lifetime. Fields x/y/z read and write directly;
//   +, -, * (by number^), dot, cross, length and normalized are registered
//   members. This is the type arithmetic runs on.
// - std.math.Vector3 -- a hostdata container (8.8) for keeping one: a table
//   can hold it, and get()/set() are the unboxing/boxing between the two.
//   Made with std.math.Vector3.new(x, y, z), owned (dispose is registered).

#ifndef LHATSTDLIB_MATH_H
#define LHATSTDLIB_MATH_H

#include "lhat.h"

bool lhatstdlib_math_register(LhatProgram *program);

#endif  // LHATSTDLIB_MATH_H
