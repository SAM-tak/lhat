// L^ (lhat) -- sample standard library: std.math.
//
// The proving library for 05 の 8.9's host values. LVector3 is the value
// side -- three f32 components living in stack slots, moved by copy,
// compared by bytes, with no lifetime for anyone to manage. Vector3 is the
// reference side (05 の 8.8): a container a table can hold, get() and set()
// the unboxing and boxing between the two. The split is deliberate: arithmetic never allocates, and
// keeping a value is spelled out as boxing rather than happening by
// accident.

#include "error.h"
#include "math.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

// Threaded through as every registration's `context` (05 の 8.7) -- see
// stdlib/io.c's IoModule comment for why this is not a file-scope static.
typedef struct {
    const LhatErrorKind *out_of_memory;
    const LhatHostValueTag *lvec3;   // std.math.LVector3 (the value)
    const LhatHostDataTag *boxed;    // std.math.Vector3 (the container)
} MathModule;

typedef struct {
    float x;
    float y;
    float z;
} LVec3;

static LhatValue fail_with(LhatMachine *machine, const LhatErrorKind *kind,
                           const char *message)
{
    LhatValue error = lhat_nil();
    return lhat_machine_make_error(machine, kind, message, lhat_nil(), &error)
               ? error
               : lhat_nil();
}

static double arg_as_real(LhatValue value)
{
    return lhat_is_integer(value) ? (double)lhat_as_integer(value)
                                  : lhat_as_real(value);
}

// The value back out of an argument, or false when the argument is not an
// LVector3 -- the double check 8.9 shares with 8.8's pointer path.
static bool lvec3_arg(const MathModule *module, LhatValue argument,
                      LVec3 *out)
{
    const void *bytes = lhat_hostvalue_data(argument, module->lvec3);
    if (bytes == NULL) {
        return false;
    }
    memcpy(out, bytes, sizeof *out);
    return true;
}

static LhatValue lvec3_value(LhatMachine *machine, const MathModule *module,
                             LVec3 v)
{
    LhatValue out = lhat_nil();
    return lhat_make_hostvalue(machine, module->lvec3, &v, &out) ? out
                                                                 : lhat_nil();
}

// f^number^, number^, number^ -> std.math.LVector3;
static LhatValue math_lvec3(LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count)
{
    const MathModule *module = (const MathModule *)context;
    if (count < 3) {
        return lhat_nil();
    }
    LVec3 v;
    v.x = (float)arg_as_real(arguments[0]);
    v.y = (float)arg_as_real(arguments[1]);
    v.z = (float)arg_as_real(arguments[2]);
    return lvec3_value(machine, module, v);
}

// op "+": f^self^, std.math.LVector3 -> std.math.LVector3;
static LhatValue lvec3_add(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count)
{
    const MathModule *module = (const MathModule *)context;
    LVec3 left, right;
    if (count < 2 || !lvec3_arg(module, arguments[0], &left) ||
        !lvec3_arg(module, arguments[1], &right)) {
        return lhat_nil();
    }
    LVec3 sum = { left.x + right.x, left.y + right.y, left.z + right.z };
    return lvec3_value(machine, module, sum);
}

// op "-": f^self^, std.math.LVector3 -> std.math.LVector3;
static LhatValue lvec3_sub(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count)
{
    const MathModule *module = (const MathModule *)context;
    LVec3 left, right;
    if (count < 2 || !lvec3_arg(module, arguments[0], &left) ||
        !lvec3_arg(module, arguments[1], &right)) {
        return lhat_nil();
    }
    LVec3 difference = { left.x - right.x, left.y - right.y,
                         left.z - right.z };
    return lvec3_value(machine, module, difference);
}

// op "-": f^self^ -> std.math.LVector3;
//
// 02 の 11.8改: the unary spelling of the same name, told apart from the
// binary one by taking no argument. Its own function -- what it computes has
// nothing in common with subtraction beyond the sign.
static LhatValue lvec3_neg(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count)
{
    const MathModule *module = (const MathModule *)context;
    LVec3 v;
    if (count < 1 || !lvec3_arg(module, arguments[0], &v)) {
        return lhat_nil();
    }
    LVec3 negated = { -v.x, -v.y, -v.z };
    return lvec3_value(machine, module, negated);
}

// op "*": both orders --
//   f^self^, number^ -> std.math.LVector3;
//   f^number^, self^ -> std.math.LVector3;
//
// 02 の 11.3改's trailing self^ makes the receiver the right operand, so
// '2 * v' hands the number over first and 'v * 2' hands the vector. Scaling
// is the same either way; only which side to unwrap differs, and the values
// themselves say that.
static LhatValue lvec3_scale(LhatMachine *machine, void *context,
                             const LhatValue *arguments, size_t count)
{
    const MathModule *module = (const MathModule *)context;
    if (count < 2) {
        return lhat_nil();
    }
    size_t vector_at = lhat_is_number(arguments[0]) ? 1 : 0;
    size_t number_at = vector_at == 0 ? 1 : 0;
    LVec3 v;
    if (!lvec3_arg(module, arguments[vector_at], &v) ||
        !lhat_is_number(arguments[number_at])) {
        return lhat_nil();
    }
    float by = (float)arg_as_real(arguments[number_at]);
    LVec3 scaled = { v.x * by, v.y * by, v.z * by };
    return lvec3_value(machine, module, scaled);
}

// f^self^, std.math.LVector3 -> number^;
static LhatValue lvec3_dot(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count)
{
    (void)machine;
    const MathModule *module = (const MathModule *)context;
    LVec3 left, right;
    if (count < 2 || !lvec3_arg(module, arguments[0], &left) ||
        !lvec3_arg(module, arguments[1], &right)) {
        return lhat_nil();
    }
    return lhat_real((double)left.x * right.x + (double)left.y * right.y +
                     (double)left.z * right.z);
}

// f^self^, std.math.LVector3 -> std.math.LVector3;
static LhatValue lvec3_cross(LhatMachine *machine, void *context,
                             const LhatValue *arguments, size_t count)
{
    const MathModule *module = (const MathModule *)context;
    LVec3 a, b;
    if (count < 2 || !lvec3_arg(module, arguments[0], &a) ||
        !lvec3_arg(module, arguments[1], &b)) {
        return lhat_nil();
    }
    LVec3 crossed = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                      a.x * b.y - a.y * b.x };
    return lvec3_value(machine, module, crossed);
}

// f^self^ -> number^;
static LhatValue lvec3_length(LhatMachine *machine, void *context,
                              const LhatValue *arguments, size_t count)
{
    (void)machine;
    const MathModule *module = (const MathModule *)context;
    LVec3 v;
    if (count < 1 || !lvec3_arg(module, arguments[0], &v)) {
        return lhat_nil();
    }
    return lhat_real(sqrt((double)v.x * v.x + (double)v.y * v.y +
                          (double)v.z * v.z));
}

// f^self^ -> std.math.LVector3;
static LhatValue lvec3_normalized(LhatMachine *machine, void *context,
                                  const LhatValue *arguments, size_t count)
{
    const MathModule *module = (const MathModule *)context;
    LVec3 v;
    if (count < 1 || !lvec3_arg(module, arguments[0], &v)) {
        return lhat_nil();
    }
    double length = sqrt((double)v.x * v.x + (double)v.y * v.y +
                         (double)v.z * v.z);
    if (length == 0.0) {
        return lvec3_value(machine, module, v);  // the zero vector stays
    }
    LVec3 unit = { (float)(v.x / length), (float)(v.y / length),
                   (float)(v.z / length) };
    return lvec3_value(machine, module, unit);
}

// ---------------------------------------------------------------------------
// std.math.Vector3 -- the container (05 の 8.8), and 8.9's boxing spelled out
// ---------------------------------------------------------------------------

// f^number^, number^, number^ -> std.math.Vector3|std.error.OutOfMemory;
static LhatValue container_new(LhatMachine *machine, void *context,
                               const LhatValue *arguments, size_t count)
{
    const MathModule *module = (const MathModule *)context;
    LVec3 *held = (LVec3 *)lhat_alloc(sizeof *held);
    if (held == NULL) {
        return fail_with(machine, module->out_of_memory,
                         "std.math.Vector3.new: out of memory");
    }
    held->x = count > 0 ? (float)arg_as_real(arguments[0]) : 0.0f;
    held->y = count > 1 ? (float)arg_as_real(arguments[1]) : 0.0f;
    held->z = count > 2 ? (float)arg_as_real(arguments[2]) : 0.0f;
    LhatValue value = lhat_nil();
    if (!lhat_machine_make_hostdata(machine, module->boxed, held, &value)) {
        lhat_free(held);
        return fail_with(machine, module->out_of_memory,
                         "std.math.Vector3.new: out of memory");
    }
    return value;
}

// p^self^;
static LhatValue container_dispose(LhatMachine *machine, void *context,
                                   const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)count;
    const MathModule *module = (const MathModule *)context;
    lhat_free(lhat_hostdata_pointer(arguments[0], module->boxed));
    return lhat_nil();
}

// f^self^ -> std.math.LVector3;  -- unboxing
static LhatValue container_get(LhatMachine *machine, void *context,
                               const LhatValue *arguments, size_t count)
{
    const MathModule *module = (const MathModule *)context;
    LVec3 *held = count > 0 ? (LVec3 *)lhat_hostdata_pointer(arguments[0],
                                                             module->boxed)
                            : NULL;
    if (held == NULL) {
        return lhat_nil();
    }
    return lvec3_value(machine, module, *held);
}

// p^self^, std.math.LVector3;  -- boxing
static LhatValue container_set(LhatMachine *machine, void *context,
                               const LhatValue *arguments, size_t count)
{
    (void)machine;
    const MathModule *module = (const MathModule *)context;
    LVec3 *held = count > 0 ? (LVec3 *)lhat_hostdata_pointer(arguments[0],
                                                             module->boxed)
                            : NULL;
    LVec3 incoming;
    if (held != NULL && count > 1 &&
        lvec3_arg(module, arguments[1], &incoming)) {
        *held = incoming;
    }
    return lhat_nil();
}

bool lhatstdlib_math_register(LhatProgram *program)
{
    // 05 の 8.7: 登録は検査より前 -- std.error.OutOfMemory を使う側(この
    // モジュール)より前に確実に存在させる。冪等なので他モジュールが先に
    // 呼んでいても構わない。
    if (!lhatstdlib_error_register(program)) {
        return false;
    }

    MathModule *module = (MathModule *)lhat_calloc(1, sizeof *module);
    if (module == NULL) {
        return false;
    }
    module->out_of_memory = lhatstdlib_error_lookup(program, "OutOfMemory");

    module->lvec3 = lhat_register_hostvalue_type(program, "std.math",
                                                 "LVector3", sizeof(LVec3));
    module->boxed = lhat_register_hostdata_type(program, "std.math",
                                                "Vector3");
    if (module->lvec3 == NULL || module->boxed == NULL) {
        lhat_free(module);
        return false;
    }

    return lhat_register_hostvalue_field(program, "std.math", "LVector3", "x",
                                         offsetof(LVec3, x),
                                         LHAT_HVFIELD_F32) &&
           lhat_register_hostvalue_field(program, "std.math", "LVector3", "y",
                                         offsetof(LVec3, y),
                                         LHAT_HVFIELD_F32) &&
           lhat_register_hostvalue_field(program, "std.math", "LVector3", "z",
                                         offsetof(LVec3, z),
                                         LHAT_HVFIELD_F32) &&
           lhat_register_hostvalue_member(
               program, "std.math", "LVector3", "+",
               "f^self^, std.math.LVector3 -> std.math.LVector3;", lvec3_add,
               module) &&
           lhat_register_hostvalue_member(
               program, "std.math", "LVector3", "-",
               "f^self^, std.math.LVector3 -> std.math.LVector3;", lvec3_sub,
               module) &&
           // 02 の 11.8改: the unary spelling of the same name, told apart by
           // taking no argument. 05 の 8.7 is what lets one name carry both.
           lhat_register_hostvalue_member(
               program, "std.math", "LVector3", "-",
               "f^self^ -> std.math.LVector3;", lvec3_neg, module) &&
           lhat_register_hostvalue_member(
               program, "std.math", "LVector3", "*",
               "f^self^, number^ -> std.math.LVector3;", lvec3_scale,
               module) &&
           // 02 の 11.3改: the self^ written last, so this is the arm '2 * v'
           // finds -- a built-in number^ on the left carries no answer for a
           // host value, and this is the side that does.
           lhat_register_hostvalue_member(
               program, "std.math", "LVector3", "*",
               "f^number^, self^ -> std.math.LVector3;", lvec3_scale,
               module) &&
           lhat_register_hostvalue_member(
               program, "std.math", "LVector3", "dot",
               "f^self^, std.math.LVector3 -> number^;", lvec3_dot, module) &&
           lhat_register_hostvalue_member(
               program, "std.math", "LVector3", "cross",
               "f^self^, std.math.LVector3 -> std.math.LVector3;",
               lvec3_cross, module) &&
           lhat_register_hostvalue_member(program, "std.math", "LVector3",
                                          "length", "f^self^ -> number^;",
                                          lvec3_length, module) &&
           lhat_register_hostvalue_member(
               program, "std.math", "LVector3", "normalized",
               "f^self^ -> std.math.LVector3;", lvec3_normalized, module) &&
           lhat_register_func(
               program, "std.math", "lvec3",
               "f^number^, number^, number^ -> std.math.LVector3;",
               math_lvec3, module) &&
           lhat_register_member(
               program, "std.math", "Vector3", "new",
               "f^number^, number^, number^ -> "
               "std.math.Vector3|std.error.OutOfMemory;",
               container_new, module) &&
           lhat_register_member(program, "std.math", "Vector3", "dispose",
                                "p^self^;", container_dispose, module) &&
           lhat_register_member(program, "std.math", "Vector3", "get",
                                "f^self^ -> std.math.LVector3;",
                                container_get, module) &&
           lhat_register_member(program, "std.math", "Vector3", "set",
                                "p^self^, std.math.LVector3;", container_set,
                                module);
}
