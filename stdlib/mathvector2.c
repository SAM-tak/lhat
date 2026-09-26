// L^ (lhat) -- sample standard library: std.math.vector2.
//
// std.math.vector3's two-component sibling (mathvector3.c says why the value
// is a host value and the box is the language's). Two f32 components are
// eight bytes -- a narrower value than Vector3 on the same machinery.

#include "mathvector2.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Threaded through as every registration's `context` (05 の 8.7) -- see
// stdlib/io.c's IoModule comment for why this is not a file-scope static.
typedef struct {
    const LhatHostValueTag *vec2;   // std.math.vector2.Vector2 (the value)
} MathModule;

typedef struct {
    float x;
    float y;
} Vec2;

static double arg_as_real(LhatValue value)
{
    return lhat_is_integer(value) ? (double)lhat_as_integer(value)
                                  : lhat_as_real(value);
}

// The value back out of an argument, or false when the argument is not a
// Vector2.
static bool vec2_arg(const MathModule *module, LhatValue argument,
                     Vec2 *out)
{
    const void *bytes = lhat_hostvalue_data(argument, module->vec2);
    if (bytes == NULL) {
        return false;
    }
    memcpy(out, bytes, sizeof *out);
    return true;
}

static LhatValue vec2_value(LhatMachine *machine, const MathModule *module,
                            Vec2 v)
{
    LhatValue out = lhat_nil();
    return lhat_make_hostvalue(machine, module->vec2, &v, &out) ? out
                                                                 : lhat_nil();
}

// f^number^, number^ -> std.math.vector2.Vector2;
static void vec2_new(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count,
                     LhatValue *answers, int *answer_count)
{
    const MathModule *module = (const MathModule *)context;
    if (count < 2) {
        return;
    }
    Vec2 v;
    v.x = (float)arg_as_real(arguments[0]);
    v.y = (float)arg_as_real(arguments[1]);
    answers[0] = vec2_value(machine, module, v);
    *answer_count = 1;
}

// op "+": f^self^, std.math.vector2.Vector2 -> std.math.vector2.Vector2;
static void vec2_add(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count,
                     LhatValue *answers, int *answer_count)
{
    const MathModule *module = (const MathModule *)context;
    Vec2 left, right;
    if (count < 2 || !vec2_arg(module, arguments[0], &left) ||
        !vec2_arg(module, arguments[1], &right)) {
        return;
    }
    Vec2 sum = { left.x + right.x, left.y + right.y };
    answers[0] = vec2_value(machine, module, sum);
    *answer_count = 1;
}

// op "-": f^self^, std.math.vector2.Vector2 -> std.math.vector2.Vector2;
static void vec2_sub(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count,
                     LhatValue *answers, int *answer_count)
{
    const MathModule *module = (const MathModule *)context;
    Vec2 left, right;
    if (count < 2 || !vec2_arg(module, arguments[0], &left) ||
        !vec2_arg(module, arguments[1], &right)) {
        return;
    }
    Vec2 difference = { left.x - right.x, left.y - right.y };
    answers[0] = vec2_value(machine, module, difference);
    *answer_count = 1;
}

// op "-": f^self^ -> std.math.vector2.Vector2;  (02 の 11.8改's unary arm)
static void vec2_neg(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count,
                     LhatValue *answers, int *answer_count)
{
    const MathModule *module = (const MathModule *)context;
    Vec2 v;
    if (count < 1 || !vec2_arg(module, arguments[0], &v)) {
        return;
    }
    Vec2 negated = { -v.x, -v.y };
    answers[0] = vec2_value(machine, module, negated);
    *answer_count = 1;
}

// op "*": both orders (02 の 11.3改) --
//   f^self^, number^ -> std.math.vector2.Vector2;
//   f^number^, self^ -> std.math.vector2.Vector2;
static void vec2_scale(LhatMachine *machine, void *context,
                       const LhatValue *arguments, size_t count,
                       LhatValue *answers, int *answer_count)
{
    const MathModule *module = (const MathModule *)context;
    if (count < 2) {
        return;
    }
    size_t vector_at = lhat_is_number(arguments[0]) ? 1 : 0;
    size_t number_at = vector_at == 0 ? 1 : 0;
    Vec2 v;
    if (!vec2_arg(module, arguments[vector_at], &v) ||
        !lhat_is_number(arguments[number_at])) {
        return;
    }
    float by = (float)arg_as_real(arguments[number_at]);
    Vec2 scaled = { v.x * by, v.y * by };
    answers[0] = vec2_value(machine, module, scaled);
    *answer_count = 1;
}

// f^self^, std.math.vector2.Vector2 -> number^;
static void vec2_dot(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count,
                     LhatValue *answers, int *answer_count)
{
    (void)machine;
    const MathModule *module = (const MathModule *)context;
    Vec2 left, right;
    if (count < 2 || !vec2_arg(module, arguments[0], &left) ||
        !vec2_arg(module, arguments[1], &right)) {
        return;
    }
    answers[0] = lhat_real((double)left.x * right.x + (double)left.y * right.y);
    *answer_count = 1;
}

// f^self^, std.math.vector2.Vector2 -> number^;
//
// The plane has no perpendicular vector to answer, so this is the z of the
// 3D cross of the two lifted to z = 0 -- positive when b turns
// counter-clockwise from a.
static void vec2_cross(LhatMachine *machine, void *context,
                       const LhatValue *arguments, size_t count,
                       LhatValue *answers, int *answer_count)
{
    (void)machine;
    const MathModule *module = (const MathModule *)context;
    Vec2 a, b;
    if (count < 2 || !vec2_arg(module, arguments[0], &a) ||
        !vec2_arg(module, arguments[1], &b)) {
        return;
    }
    answers[0] = lhat_real((double)a.x * b.y - (double)a.y * b.x);
    *answer_count = 1;
}

// f^self^ -> number^;
static void vec2_length(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count,
                        LhatValue *answers, int *answer_count)
{
    (void)machine;
    const MathModule *module = (const MathModule *)context;
    Vec2 v;
    if (count < 1 || !vec2_arg(module, arguments[0], &v)) {
        return;
    }
    answers[0] = lhat_real(sqrt((double)v.x * v.x + (double)v.y * v.y));
    *answer_count = 1;
}

// f^self^ -> std.math.vector2.Vector2;
static void vec2_normalized(LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count,
                            LhatValue *answers, int *answer_count)
{
    const MathModule *module = (const MathModule *)context;
    Vec2 v;
    if (count < 1 || !vec2_arg(module, arguments[0], &v)) {
        return;
    }
    double length = sqrt((double)v.x * v.x + (double)v.y * v.y);
    if (length == 0.0) {
        answers[0] = vec2_value(machine, module, v);
        *answer_count = 1;
        return;  // the zero vector stays
    }
    Vec2 unit = { (float)(v.x / length), (float)(v.y / length) };
    answers[0] = vec2_value(machine, module, unit);
    *answer_count = 1;
}

// tostring: f^self^ -> string^;  (02 の 14.17, spelled as vec3_tostring does)
static void vec2_tostring(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count,
                          LhatValue *answers, int *answer_count)
{
    const MathModule *module = (const MathModule *)context;
    Vec2 v;
    if (count < 1 || !vec2_arg(module, arguments[0], &v)) {
        return;
    }
    const double held[2] = { (double)v.x, (double)v.y };
    static const char *const names[2] = { "x", "y" };

    char text[96];
    size_t used = 0;
    text[used++] = '{';
    for (size_t i = 0; i < 2; i++) {
        char spelt[64];
        size_t length = lhat_value_text(lhat_real(held[i]), spelt, sizeof spelt);
        if (length >= sizeof spelt) {
            return;  // no number^ spells this long
        }
        int written = snprintf(text + used, sizeof text - used, "%s%s:%s",
                               i == 0 ? "" : " ", names[i], spelt);
        if (written < 0 || (size_t)written >= sizeof text - used) {
            return;
        }
        used += (size_t)written;
    }
    if (used + 1 >= sizeof text) {
        return;
    }
    text[used++] = '}';

    LhatValue out = lhat_nil();
    answers[0] = lhat_machine_make_string(machine, text, used, &out) ? out
                                                              : lhat_nil();
    *answer_count = 1;
}

bool lhatstdlib_mathvector2_register(LhatProgram *program)
{
    // One identity per process, as lhatstdlib_mathvector3_register says.
    static MathModule shared;
    MathModule *module = &shared;

    module->vec2 = lhat_register_hostvalue_type(program, "std.math.vector2",
                                                 "Vector2", sizeof(Vec2));
    if (module->vec2 == NULL) {
        return false;
    }

    return lhat_register_hostvalue_field(program, "std.math.vector2", "Vector2", "x",
                                         offsetof(Vec2, x),
                                         LHAT_HVFIELD_F32) &&
           lhat_register_hostvalue_field(program, "std.math.vector2", "Vector2", "y",
                                         offsetof(Vec2, y),
                                         LHAT_HVFIELD_F32) &&
           lhat_register_hostvalue_member(
               program, "std.math.vector2", "Vector2", "+",
               "f^self^, std.math.vector2.Vector2 -> std.math.vector2.Vector2;", vec2_add,
               module) &&
           lhat_register_hostvalue_member(
               program, "std.math.vector2", "Vector2", "-",
               "f^self^, std.math.vector2.Vector2 -> std.math.vector2.Vector2;", vec2_sub,
               module) &&
           lhat_register_hostvalue_member(
               program, "std.math.vector2", "Vector2", "-",
               "f^self^ -> std.math.vector2.Vector2;", vec2_neg, module) &&
           lhat_register_hostvalue_member(
               program, "std.math.vector2", "Vector2", "*",
               "f^self^, number^ -> std.math.vector2.Vector2;", vec2_scale,
               module) &&
           lhat_register_hostvalue_member(
               program, "std.math.vector2", "Vector2", "*",
               "f^number^, self^ -> std.math.vector2.Vector2;", vec2_scale,
               module) &&
           lhat_register_hostvalue_member(
               program, "std.math.vector2", "Vector2", "dot",
               "f^self^, std.math.vector2.Vector2 -> number^;", vec2_dot, module) &&
           lhat_register_hostvalue_member(
               program, "std.math.vector2", "Vector2", "cross",
               "f^self^, std.math.vector2.Vector2 -> number^;", vec2_cross, module) &&
           lhat_register_hostvalue_member(program, "std.math.vector2", "Vector2",
                                          "length", "f^self^ -> number^;",
                                          vec2_length, module) &&
           lhat_register_hostvalue_member(
               program, "std.math.vector2", "Vector2", "normalized",
               "f^self^ -> std.math.vector2.Vector2;", vec2_normalized, module) &&
           lhat_register_hostvalue_member(program, "std.math.vector2", "Vector2",
                                          "tostring", "f^self^ -> string^;",
                                          vec2_tostring, module) &&
           lhat_register_func(
               program, "std.math.vector2", "new",
               "f^number^, number^ -> std.math.vector2.Vector2;",
               vec2_new, module);
}
