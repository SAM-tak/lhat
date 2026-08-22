// L^ (lhat) -- sample standard library: std.math.
//
// The scalar functions -- what a number^ cannot answer about itself alone
// (02 の 14.21改 gives it abs, sign and clamp beside the three roundings,
// and 14.8改2 the constants number^.pi / tau / e / inf / nan). This is the
// rest: two numbers or more, and the transcendental functions. Not Lua's
// math table copied over: what std.random owns is not here, and what
// number^ carries is not here either.
//
// Angles are degrees throughout, as Unity's Mathf -- sin(90) is 1, and
// asin(1) is 90. The multiples of 90 are answered exactly (cos(90) is 0,
// not 6e-17). A host whose own API takes radians (LÖVE, Box2D) converts at
// its boundary with rad and deg.
//
//   sin cos tan          f^number^ -> number^;     degrees in
//   asin acos atan       f^number^ -> number^;     degrees out
//   atan2                f^number^, number^ -> number^;   (y, x), degrees out
//   deg rad              f^number^ -> number^;     radians <-> degrees
//   sqrt cbrt exp        f^number^ -> number^;
//   log                  f^number^ -> number^;  and  f^number^, number^ -> number^;
//                        (natural, and to a written base -- one name, two arms)
//   log2 log10           f^number^ -> number^;
//   hypot                f^number^, number^ -> number^;
//   fmod                 f^number^, number^ -> number^;   C's fmod: the sign of
//                        the dividend, where '%' floors toward the divisor
//   min max              f^number^, ...:number^ -> number^;   one or more;
//                        an integer answers as an integer
//   lerp                 f^number^, number^, number^ -> number^;   (a, b, t)
//
// '**' already takes any exponent (14.8改), so there is no pow here.
// std.math.vector3 (mathvector3.h) is a module of its own under this one.

#ifndef LHATSTDLIB_MATH_H
#define LHATSTDLIB_MATH_H

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_math_register(LhatProgram *program);

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_MATH_H
