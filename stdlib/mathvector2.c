// L^ (lhat) -- two f32 components, with scalar signed-area cross product.
#include "mathvector2.h"

#define MATH_MODULE "std.math.vector2"
#define MATH_TYPE "Vector2"
#define MATH_DIM 2
#define MATH_NAMES { "x", "y" }
#define MATH_VECTOR2
#include "mathvalue_impl.h"

bool lhatstdlib_mathvector2_register(LhatProgram *program)
{
    // One host-value identity per process, shared by every program.
    static MathModule shared;
    return math_register(program, &shared,
                         "f^number^, number^ -> std.math.vector2.Vector2;") &&
           math_register_constant(program, &shared, 0, "zero", (MathValue){{0, 0}}) &&
           math_register_constant(program, &shared, 1, "one", (MathValue){{1, 1}}) &&
           math_register_constant(program, &shared, 2, "right", (MathValue){{1, 0}}) &&
           math_register_constant(program, &shared, 3, "left", (MathValue){{-1, 0}}) &&
           math_register_constant(program, &shared, 4, "up", (MathValue){{0, 1}}) &&
           math_register_constant(program, &shared, 5, "down", (MathValue){{0, -1}});
}
