// L^ (lhat) -- sample standard library: std.math.complex.
// Complex is an f32 host value with writable re/im fields. new(re, im),
// +, binary/unary -, * (complex product or number^ scaling, either order),
// conjugate, length, normalized and tostring follow Vector2's conventions.
// A zero value stays zero when normalized. Boxes are supplied by the language.
// Module functions zero(), one() (= 1 + 0i) and i() (= 0 + 1i) return values.
#ifndef LHATSTDLIB_MATHCOMPLEX_H
#define LHATSTDLIB_MATHCOMPLEX_H

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_mathcomplex_register(LhatProgram *program);

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_MATHCOMPLEX_H
