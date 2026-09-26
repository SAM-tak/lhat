// L^ (lhat) -- four f32 components, with value semantics like Vector2/Vector3.
#include "mathvector4.h"

#define MATH_MODULE "std.math.vector4"
#define MATH_TYPE "Vector4"
#define MATH_DIM 4
#define MATH_NAMES { "x", "y", "z", "w" }
#define MATH_VECTOR4
#include "mathvalue_impl.h"

bool lhatstdlib_mathvector4_register(LhatProgram *program)
{
    static MathModule shared;
    return math_register(program, &shared,
                         "f^number^, number^, number^, number^ -> std.math.vector4.Vector4;") &&
           math_register_constant(program, &shared, 0, "zero", (MathValue){{0, 0, 0, 0}}) &&
           math_register_constant(program, &shared, 1, "one", (MathValue){{1, 1, 1, 1}});
}
