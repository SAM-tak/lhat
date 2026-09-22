// L^ (lhat) -- sample standard library: std.math.
//
// The scalar functions -- what a number^ cannot answer about itself alone
// (02 の 14.21改 gives it abs, sign and clamp beside the three roundings,
// and 14.8改2 the constants number^.inf / nan). This is the rest: two
// numbers or more, the transcendental functions, and the mathematical
// constants they are written with. Not Lua's math table copied over: what
// std.random owns is not here, and what number^ carries is not here either.
//
// Angles are radians throughout, as Lua's math and C's <math.h> -- sin of
// pi / 2 is 1, and asin(1) is pi / 2. The answers are libm's as they stand,
// so cos(pi / 2) is 6e-17 rather than 0; '=' reads a real with 14.8's
// tolerance, which is the comparison that wants. A written angle in degrees
// converts with rad, and deg turns an answer back.
//
//   pi tau e             number^                   constants (tau = 2 * pi)
//   sin cos tan          f^number^ -> number^;     radians in
//   asin acos atan       f^number^ -> number^;     radians out
//   atan2                f^number^, number^ -> number^;   (y, x), radians out
//   deg rad              f^number^ -> number^;     radians -> degrees, and back
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
