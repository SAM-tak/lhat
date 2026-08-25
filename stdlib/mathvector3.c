// L^ (lhat) -- sample standard library: std.math.vector3.
//
// The proving library for 05 の 8.9's host values. Vector3 is the value --
// three f32 components living in stack slots, moved by copy, compared by
// bytes, with no lifetime for anyone to manage. Keeping one is the
// language's Vector3.Box^ (box^ / get / set): arithmetic never allocates,
// and holding a value is spelled out rather than happening by accident.

#include "error.h"
#include "mathvector3.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Threaded through as every registration's `context` (05 の 8.7) -- see
// stdlib/io.c's IoModule comment for why this is not a file-scope static.
// Holding one is what the language asks: 8.9's box (`Vector3.Box^`) is the
// machine's own, so the library registers the value type alone.
typedef struct {
    const LhatHostValueTag *vec3;   // std.math.vector3.Vector3 (the value)
} MathModule;

typedef struct {
    float x;
    float y;
    float z;
} Vec3;

static double arg_as_real(LhatValue value)
{
    return lhat_is_integer(value) ? (double)lhat_as_integer(value)
                                  : lhat_as_real(value);
}

// The value back out of an argument, or false when the argument is not a
// Vector3 -- the double check 8.9 shares with 8.8's pointer path.
static bool vec3_arg(const MathModule *module, LhatValue argument,
                      Vec3 *out)
{
    const void *bytes = lhat_hostvalue_data(argument, module->vec3);
    if (bytes == NULL) {
        return false;
    }
    memcpy(out, bytes, sizeof *out);
    return true;
}

static LhatValue vec3_value(LhatMachine *machine, const MathModule *module,
                             Vec3 v)
{
    LhatValue out = lhat_nil();
    return lhat_make_hostvalue(machine, module->vec3, &v, &out) ? out
                                                                 : lhat_nil();
}

// f^number^, number^, number^ -> std.math.vector3.Vector3;
static LhatValue vec3_new(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count)
{
    const MathModule *module = (const MathModule *)context;
    if (count < 3) {
        return lhat_nil();
    }
    Vec3 v;
    v.x = (float)arg_as_real(arguments[0]);
    v.y = (float)arg_as_real(arguments[1]);
    v.z = (float)arg_as_real(arguments[2]);
    return vec3_value(machine, module, v);
}

// op "+": f^self^, std.math.vector3.Vector3 -> std.math.vector3.Vector3;
static LhatValue vec3_add(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count)
{
    const MathModule *module = (const MathModule *)context;
    Vec3 left, right;
    if (count < 2 || !vec3_arg(module, arguments[0], &left) ||
        !vec3_arg(module, arguments[1], &right)) {
        return lhat_nil();
    }
    Vec3 sum = { left.x + right.x, left.y + right.y, left.z + right.z };
    return vec3_value(machine, module, sum);
}

// op "-": f^self^, std.math.vector3.Vector3 -> std.math.vector3.Vector3;
static LhatValue vec3_sub(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count)
{
    const MathModule *module = (const MathModule *)context;
    Vec3 left, right;
    if (count < 2 || !vec3_arg(module, arguments[0], &left) ||
        !vec3_arg(module, arguments[1], &right)) {
        return lhat_nil();
    }
    Vec3 difference = { left.x - right.x, left.y - right.y,
                         left.z - right.z };
    return vec3_value(machine, module, difference);
}

// op "-": f^self^ -> std.math.vector3.Vector3;
//
// 02 の 11.8改: the unary spelling of the same name, told apart from the
// binary one by taking no argument. Its own function -- what it computes has
// nothing in common with subtraction beyond the sign.
static LhatValue vec3_neg(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count)
{
    const MathModule *module = (const MathModule *)context;
    Vec3 v;
    if (count < 1 || !vec3_arg(module, arguments[0], &v)) {
        return lhat_nil();
    }
    Vec3 negated = { -v.x, -v.y, -v.z };
    return vec3_value(machine, module, negated);
}

// op "*": both orders --
//   f^self^, number^ -> std.math.vector3.Vector3;
//   f^number^, self^ -> std.math.vector3.Vector3;
//
// 02 の 11.3改's trailing self^ makes the receiver the right operand, so
// '2 * v' hands the number over first and 'v * 2' hands the vector. Scaling
// is the same either way; only which side to unwrap differs, and the values
// themselves say that.
static LhatValue vec3_scale(LhatMachine *machine, void *context,
                             const LhatValue *arguments, size_t count)
{
    const MathModule *module = (const MathModule *)context;
    if (count < 2) {
        return lhat_nil();
    }
    size_t vector_at = lhat_is_number(arguments[0]) ? 1 : 0;
    size_t number_at = vector_at == 0 ? 1 : 0;
    Vec3 v;
    if (!vec3_arg(module, arguments[vector_at], &v) ||
        !lhat_is_number(arguments[number_at])) {
        return lhat_nil();
    }
    float by = (float)arg_as_real(arguments[number_at]);
    Vec3 scaled = { v.x * by, v.y * by, v.z * by };
    return vec3_value(machine, module, scaled);
}

// f^self^, std.math.vector3.Vector3 -> number^;
static LhatValue vec3_dot(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count)
{
    (void)machine;
    const MathModule *module = (const MathModule *)context;
    Vec3 left, right;
    if (count < 2 || !vec3_arg(module, arguments[0], &left) ||
        !vec3_arg(module, arguments[1], &right)) {
        return lhat_nil();
    }
    return lhat_real((double)left.x * right.x + (double)left.y * right.y +
                     (double)left.z * right.z);
}

// f^self^, std.math.vector3.Vector3 -> std.math.vector3.Vector3;
static LhatValue vec3_cross(LhatMachine *machine, void *context,
                             const LhatValue *arguments, size_t count)
{
    const MathModule *module = (const MathModule *)context;
    Vec3 a, b;
    if (count < 2 || !vec3_arg(module, arguments[0], &a) ||
        !vec3_arg(module, arguments[1], &b)) {
        return lhat_nil();
    }
    Vec3 crossed = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                      a.x * b.y - a.y * b.x };
    return vec3_value(machine, module, crossed);
}

// f^self^ -> number^;
static LhatValue vec3_length(LhatMachine *machine, void *context,
                              const LhatValue *arguments, size_t count)
{
    (void)machine;
    const MathModule *module = (const MathModule *)context;
    Vec3 v;
    if (count < 1 || !vec3_arg(module, arguments[0], &v)) {
        return lhat_nil();
    }
    return lhat_real(sqrt((double)v.x * v.x + (double)v.y * v.y +
                          (double)v.z * v.z));
}

// f^self^ -> std.math.vector3.Vector3;
static LhatValue vec3_normalized(LhatMachine *machine, void *context,
                                  const LhatValue *arguments, size_t count)
{
    const MathModule *module = (const MathModule *)context;
    Vec3 v;
    if (count < 1 || !vec3_arg(module, arguments[0], &v)) {
        return lhat_nil();
    }
    double length = sqrt((double)v.x * v.x + (double)v.y * v.y +
                         (double)v.z * v.z);
    if (length == 0.0) {
        return vec3_value(machine, module, v);  // the zero vector stays
    }
    Vec3 unit = { (float)(v.x / length), (float)(v.y / length),
                   (float)(v.z / length) };
    return vec3_value(machine, module, unit);
}

// tostring: f^self^ -> string^;
//
// 02 の 14.17: the text a reader reads. The components are spelled the way L^
// spells a number^ -- 14.8's two representations, which lhat_value_text
// already decides between -- rather than by a printf format of this
// library's own, so a component reads here as it would anywhere else.
static LhatValue vec3_tostring(LhatMachine *machine, void *context,
                                const LhatValue *arguments, size_t count)
{
    const MathModule *module = (const MathModule *)context;
    Vec3 v;
    if (count < 1 || !vec3_arg(module, arguments[0], &v)) {
        return lhat_nil();
    }
    const double held[3] = { (double)v.x, (double)v.y, (double)v.z };
    static const char *const names[3] = { "x", "y", "z" };

    // Three numbers, each at most a few dozen bytes of '%g' with an exponent.
    char text[128];
    size_t used = 0;
    text[used++] = '{';
    for (size_t i = 0; i < 3; i++) {
        char spelt[64];
        size_t length = lhat_value_text(lhat_real(held[i]), spelt, sizeof spelt);
        if (length >= sizeof spelt) {
            return lhat_nil();  // no number^ spells this long
        }
        int written = snprintf(text + used, sizeof text - used, "%s%s:%s",
                               i == 0 ? "" : " ", names[i], spelt);
        if (written < 0 || (size_t)written >= sizeof text - used) {
            return lhat_nil();
        }
        used += (size_t)written;
    }
    if (used + 1 >= sizeof text) {
        return lhat_nil();
    }
    text[used++] = '}';

    // 14.17 gives tostring one signature and no error arm, so a heap that
    // cannot hold the answer says nothing -- the same nil^ vec3_value
    // answers with when the machine has no room for a value either.
    LhatValue out = lhat_nil();
    return lhat_machine_make_string(machine, text, used, &out) ? out
                                                              : lhat_nil();
}

bool lhatstdlib_mathvector3_register(LhatProgram *program)
{
    MathModule *module = (MathModule *)lhat_calloc(1, sizeof *module);
    if (module == NULL) {
        return false;
    }
    // 05 の 8.7: the program gives this back when it goes -- a register
    // that answers bool hands its caller no handle to free it with.
    if (!lhat_program_on_dispose(program, lhat_free, module)) {
        lhat_free(module);
        return false;
    }

    module->vec3 = lhat_register_hostvalue_type(program, "std.math.vector3",
                                                 "Vector3", sizeof(Vec3));
    if (module->vec3 == NULL) {
        return false;
    }

    return lhat_register_hostvalue_field(program, "std.math.vector3", "Vector3", "x",
                                         offsetof(Vec3, x),
                                         LHAT_HVFIELD_F32) &&
           lhat_register_hostvalue_field(program, "std.math.vector3", "Vector3", "y",
                                         offsetof(Vec3, y),
                                         LHAT_HVFIELD_F32) &&
           lhat_register_hostvalue_field(program, "std.math.vector3", "Vector3", "z",
                                         offsetof(Vec3, z),
                                         LHAT_HVFIELD_F32) &&
           lhat_register_hostvalue_member(
               program, "std.math.vector3", "Vector3", "+",
               "f^self^, std.math.vector3.Vector3 -> std.math.vector3.Vector3;", vec3_add,
               module) &&
           lhat_register_hostvalue_member(
               program, "std.math.vector3", "Vector3", "-",
               "f^self^, std.math.vector3.Vector3 -> std.math.vector3.Vector3;", vec3_sub,
               module) &&
           // 02 の 11.8改: the unary spelling of the same name, told apart by
           // taking no argument. 05 の 8.7 is what lets one name carry both.
           lhat_register_hostvalue_member(
               program, "std.math.vector3", "Vector3", "-",
               "f^self^ -> std.math.vector3.Vector3;", vec3_neg, module) &&
           lhat_register_hostvalue_member(
               program, "std.math.vector3", "Vector3", "*",
               "f^self^, number^ -> std.math.vector3.Vector3;", vec3_scale,
               module) &&
           // 02 の 11.3改: the self^ written last, so this is the arm '2 * v'
           // finds -- a built-in number^ on the left carries no answer for a
           // host value, and this is the side that does.
           lhat_register_hostvalue_member(
               program, "std.math.vector3", "Vector3", "*",
               "f^number^, self^ -> std.math.vector3.Vector3;", vec3_scale,
               module) &&
           lhat_register_hostvalue_member(
               program, "std.math.vector3", "Vector3", "dot",
               "f^self^, std.math.vector3.Vector3 -> number^;", vec3_dot, module) &&
           lhat_register_hostvalue_member(
               program, "std.math.vector3", "Vector3", "cross",
               "f^self^, std.math.vector3.Vector3 -> std.math.vector3.Vector3;",
               vec3_cross, module) &&
           lhat_register_hostvalue_member(program, "std.math.vector3", "Vector3",
                                          "length", "f^self^ -> number^;",
                                          vec3_length, module) &&
           lhat_register_hostvalue_member(
               program, "std.math.vector3", "Vector3", "normalized",
               "f^self^ -> std.math.vector3.Vector3;", vec3_normalized, module) &&
           // 02 の 14.17: written down the library's way rather than the
           // machine's. The name is the bare one -- the hat is 14.17改's,
           // and what it keeps apart is a plain table's names from the
           // built-in's, which a registered type has none of.
           lhat_register_hostvalue_member(program, "std.math.vector3", "Vector3",
                                          "tostring", "f^self^ -> string^;",
                                          vec3_tostring, module) &&
           lhat_register_func(
               program, "std.math.vector3", "new",
               "f^number^, number^, number^ -> std.math.vector3.Vector3;",
               vec3_new, module);
}
