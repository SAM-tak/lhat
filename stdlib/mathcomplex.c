// L^ (lhat) -- complex numbers as two f32 components (re, im).
#include "mathcomplex.h"

#define MATH_MODULE "std.math.complex"
#define MATH_TYPE "Complex"
#define MATH_DIM 2
#define MATH_NAMES { "re", "im" }
#define MATH_COMPLEX
#include "mathvalue_impl.h"

bool lhatstdlib_mathcomplex_register(LhatProgram *program)
{
    static MathModule shared;
    return math_register(program, &shared,
                         "f^number^, number^ -> std.math.complex.Complex;") &&
           math_register_constant(program, &shared, 0, "zero", (MathValue){{0, 0}}) &&
           math_register_constant(program, &shared, 1, "one", (MathValue){{1, 0}}) &&
           math_register_constant(program, &shared, 2, "i", (MathValue){{0, 1}});
}
