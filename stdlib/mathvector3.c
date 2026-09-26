// L^ (lhat) -- three f32 components, with vector cross product.
#include "mathvector3.h"
#include "mathvector3_internal.h"

#define MATH_MODULE "std.math.vector3"
#define MATH_TYPE "Vector3"
#define MATH_DIM 3
#define MATH_NAMES { "x", "y", "z" }
#define MATH_VECTOR3
#include "mathvalue_impl.h"

static MathModule shared;

const LhatHostValueTag *lhatstdlib_mathvector3_tag(void)
{
    return shared.tag;
}

bool lhatstdlib_mathvector3_register(LhatProgram *program)
{
    // One host-value identity per process, shared by every program.
    return math_register(program, &shared,
                         "f^number^, number^, number^ -> std.math.vector3.Vector3;") &&
           math_register_constant(program, &shared, 0, "zero", (MathValue){{0, 0, 0}}) &&
           math_register_constant(program, &shared, 1, "one", (MathValue){{1, 1, 1}}) &&
           math_register_constant(program, &shared, 2, "right", (MathValue){{1, 0, 0}}) &&
           math_register_constant(program, &shared, 3, "left", (MathValue){{-1, 0, 0}}) &&
           math_register_constant(program, &shared, 4, "up", (MathValue){{0, 1, 0}}) &&
           math_register_constant(program, &shared, 5, "down", (MathValue){{0, -1, 0}}) &&
           // +Z forward is a convention, not a property of the coordinate system.
           math_register_constant(program, &shared, 6, "forward", (MathValue){{0, 0, 1}}) &&
           math_register_constant(program, &shared, 7, "back", (MathValue){{0, 0, -1}});
}
