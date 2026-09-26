// Private implementation shared by the small f32 host-value math modules.
// The including file supplies MATH_MODULE, MATH_TYPE, MATH_DIM and MATH_NAMES;
// MATH_VECTOR2/3 enable their respective cross products; MATH_VECTOR2/3/4
// enable the component-wise product. MATH_COMPLEX or MATH_QUATERNION enables
// the algebraic product and conjugate instead.
// Each translation unit gets its own callbacks and registration context.
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define MATH_FULL MATH_MODULE "." MATH_TYPE

typedef struct {
    const LhatHostValueTag *tag;
} MathModule;

typedef struct {
    float component[MATH_DIM];
} MathValue;

static const char *const math_names[MATH_DIM] = MATH_NAMES;

static double math_real(LhatValue v)
{
    return lhat_is_integer(v) ? (double)lhat_as_integer(v) : lhat_as_real(v);
}

static bool math_arg(const MathModule *module, LhatValue v, MathValue *out)
{
    const void *bytes = lhat_hostvalue_data(v, module->tag);
    if (bytes == NULL) {
        return false;
    }
    memcpy(out, bytes, sizeof *out);
    return true;
}

static LhatValue math_value(LhatMachine *machine, const MathModule *module,
                            MathValue v)
{
    LhatValue out = lhat_nil();
    return lhat_make_hostvalue(machine, module->tag, &v, &out) ? out : lhat_nil();
}

// A module function supplies a fresh host value on each call. Static module
// values cannot be registered with the core's scalar-only constant API.
typedef struct {
    const MathModule *module;
    MathValue value;
} MathConstant;

static MathConstant math_constants[8];

static void math_constant(LhatMachine *machine, void *context,
                          const LhatValue *args, size_t count,
                          LhatValue *answers, int *answer_count)
{
    (void)args;
    (void)count;
    const MathConstant *constant = (const MathConstant *)context;
    answers[0] = math_value(machine, constant->module, constant->value);
    *answer_count = 1;
}

static bool math_register_constant(LhatProgram *program, MathModule *module,
                                   size_t index, const char *name, MathValue value)
{
    if (index >= sizeof math_constants / sizeof math_constants[0]) {
        return false;
    }
    math_constants[index].module = module;
    math_constants[index].value = value;
    return lhat_register_func(program, MATH_MODULE, name,
                              "f^ -> " MATH_FULL ";", math_constant,
                              &math_constants[index]);
}

static void math_new(LhatMachine *machine, void *context,
                     const LhatValue *args, size_t count,
                     LhatValue *answers, int *answer_count)
{
    if (count < MATH_DIM) {
        return;
    }
    MathValue v = { { 0 } };
    for (size_t i = 0; i < MATH_DIM; i++) {
        if (!lhat_is_number(args[i])) {
            return;
        }
        v.component[i] = (float)math_real(args[i]);
    }
    answers[0] = math_value(machine, (const MathModule *)context, v);
    *answer_count = 1;
}

static void math_add(LhatMachine *machine, void *context,
                     const LhatValue *args, size_t count,
                     LhatValue *answers, int *answer_count)
{
    const MathModule *module = (const MathModule *)context;
    MathValue a, b, result = { { 0 } };
    if (count < 2 || !math_arg(module, args[0], &a) ||
        !math_arg(module, args[1], &b)) {
        return;
    }
    for (size_t i = 0; i < MATH_DIM; i++) {
        result.component[i] = a.component[i] + b.component[i];
    }
    answers[0] = math_value(machine, module, result);
    *answer_count = 1;
}

static void math_sub(LhatMachine *machine, void *context,
                     const LhatValue *args, size_t count,
                     LhatValue *answers, int *answer_count)
{
    const MathModule *module = (const MathModule *)context;
    MathValue a, b, result = { { 0 } };
    if (count < 2 || !math_arg(module, args[0], &a) ||
        !math_arg(module, args[1], &b)) {
        return;
    }
    for (size_t i = 0; i < MATH_DIM; i++) {
        result.component[i] = a.component[i] - b.component[i];
    }
    answers[0] = math_value(machine, module, result);
    *answer_count = 1;
}

static void math_neg(LhatMachine *machine, void *context,
                     const LhatValue *args, size_t count,
                     LhatValue *answers, int *answer_count)
{
    const MathModule *module = (const MathModule *)context;
    MathValue v;
    if (count < 1 || !math_arg(module, args[0], &v)) {
        return;
    }
    for (size_t i = 0; i < MATH_DIM; i++) {
        v.component[i] = -v.component[i];
    }
    answers[0] = math_value(machine, module, v);
    *answer_count = 1;
}

static void math_scale(LhatMachine *machine, void *context,
                       const LhatValue *args, size_t count,
                       LhatValue *answers, int *answer_count)
{
    const MathModule *module = (const MathModule *)context;
    if (count < 2) {
        return;
    }
    size_t value_at = lhat_is_number(args[0]) ? 1 : 0;
    size_t number_at = value_at == 0 ? 1 : 0;
    MathValue v;
    if (!math_arg(module, args[value_at], &v) || !lhat_is_number(args[number_at])) {
        return;
    }
    float by = (float)math_real(args[number_at]);
    for (size_t i = 0; i < MATH_DIM; i++) {
        v.component[i] *= by;
    }
    answers[0] = math_value(machine, module, v);
    *answer_count = 1;
}

#if defined(MATH_VECTOR2) || defined(MATH_VECTOR3) || defined(MATH_VECTOR4)
// / has no scalar-on-the-left arm. Component-wise vector division follows
// Hadamard multiplication; a zero divisor follows floating-point arithmetic.
static void math_divide(LhatMachine *machine, void *context,
                        const LhatValue *args, size_t count,
                        LhatValue *answers, int *answer_count)
{
    const MathModule *module = (const MathModule *)context;
    MathValue a, result = { { 0 } };
    if (count < 2 || !math_arg(module, args[0], &a)) {
        return;
    }
    MathValue b;
    if (math_arg(module, args[1], &b)) {
        for (size_t i = 0; i < MATH_DIM; i++) {
            result.component[i] = a.component[i] / b.component[i];
        }
    } else if (lhat_is_number(args[1])) {
        float divisor = (float)math_real(args[1]);
        for (size_t i = 0; i < MATH_DIM; i++) {
            result.component[i] = a.component[i] / divisor;
        }
    } else {
        return;
    }
    answers[0] = math_value(machine, module, result);
    *answer_count = 1;
}

// * between two vectors is the Hadamard (component-wise) product. This is
// separate from number^ scaling, which accepts a scalar on either side.
static void math_hadamard(LhatMachine *machine, void *context,
                          const LhatValue *args, size_t count,
                          LhatValue *answers, int *answer_count)
{
    const MathModule *module = (const MathModule *)context;
    MathValue a, b, result = { { 0 } };
    if (count < 2 || !math_arg(module, args[0], &a) ||
        !math_arg(module, args[1], &b)) {
        return;
    }
    for (size_t i = 0; i < MATH_DIM; i++) {
        result.component[i] = a.component[i] * b.component[i];
    }
    answers[0] = math_value(machine, module, result);
    *answer_count = 1;
}

// Unclamped linear interpolation. Compute in number^ precision before
// rounding each component back to the host value's f32 representation.
static void math_lerp(LhatMachine *machine, void *context,
                      const LhatValue *args, size_t count,
                      LhatValue *answers, int *answer_count)
{
    const MathModule *module = (const MathModule *)context;
    MathValue a, b, result = { { 0 } };
    if (count < 3 || !math_arg(module, args[0], &a) ||
        !math_arg(module, args[1], &b) || !lhat_is_number(args[2])) {
        return;
    }
    double t = math_real(args[2]);
    for (size_t i = 0; i < MATH_DIM; i++) {
        result.component[i] = (float)(a.component[i] +
                                      ((double)b.component[i] - a.component[i]) * t);
    }
    answers[0] = math_value(machine, module, result);
    *answer_count = 1;
}
#endif

#if defined(MATH_VECTOR2)
// atan2 chooses the signed direction from +x in the range [-pi, pi].
// C's atan2(0, 0) is specified, so the zero vector answers 0 radians.
static void math_angle(LhatMachine *machine, void *context,
                       const LhatValue *args, size_t count,
                       LhatValue *answers, int *answer_count)
{
    (void)machine;
    MathValue v;
    if (count < 1 || !math_arg((const MathModule *)context, args[0], &v)) {
        return;
    }
    double angle = v.component[0] == 0.0f && v.component[1] == 0.0f
                       ? 0.0
                       : atan2((double)v.component[1], (double)v.component[0]);
    answers[0] = lhat_real(angle);
    *answer_count = 1;
}

// x-right/y-down coordinates: positive radians turn +x toward +y, clockwise
// on screen.
static void math_rotate(LhatMachine *machine, void *context,
                        const LhatValue *args, size_t count,
                        LhatValue *answers, int *answer_count)
{
    const MathModule *module = (const MathModule *)context;
    MathValue v, result = { { 0 } };
    if (count < 2 || !math_arg(module, args[0], &v) ||
        !lhat_is_number(args[1])) {
        return;
    }
    double angle = math_real(args[1]);
    double cosine = cos(angle), sine = sin(angle);
    result.component[0] = (float)(v.component[0] * cosine - v.component[1] * sine);
    result.component[1] = (float)(v.component[0] * sine + v.component[1] * cosine);
    answers[0] = math_value(machine, module, result);
    *answer_count = 1;
}
#endif

#if defined(MATH_COMPLEX) || defined(MATH_QUATERNION)
static void math_mul(LhatMachine *machine, void *context,
                     const LhatValue *args, size_t count,
                     LhatValue *answers, int *answer_count)
{
    const MathModule *module = (const MathModule *)context;
    MathValue a, b, result = { { 0 } };
    if (count < 2 || !math_arg(module, args[0], &a) ||
        !math_arg(module, args[1], &b)) {
        return;
    }
#if defined(MATH_COMPLEX)
    // (re, im) * (re, im).
    result.component[0] = a.component[0] * b.component[0] - a.component[1] * b.component[1];
    result.component[1] = a.component[0] * b.component[1] + a.component[1] * b.component[0];
#else
    // Vector-first (x, y, z, w), with w the scalar part. Hamilton product.
    const float *u = a.component, *v = b.component;
    result.component[0] = u[3]*v[0] + u[0]*v[3] + u[1]*v[2] - u[2]*v[1];
    result.component[1] = u[3]*v[1] - u[0]*v[2] + u[1]*v[3] + u[2]*v[0];
    result.component[2] = u[3]*v[2] + u[0]*v[1] - u[1]*v[0] + u[2]*v[3];
    result.component[3] = u[3]*v[3] - u[0]*v[0] - u[1]*v[1] - u[2]*v[2];
#endif
    answers[0] = math_value(machine, module, result);
    *answer_count = 1;
}

static void math_conjugate(LhatMachine *machine, void *context,
                           const LhatValue *args, size_t count,
                           LhatValue *answers, int *answer_count)
{
    const MathModule *module = (const MathModule *)context;
    MathValue v;
    if (count < 1 || !math_arg(module, args[0], &v)) {
        return;
    }
#if defined(MATH_COMPLEX)
    v.component[1] = -v.component[1];
#else
    for (size_t i = 0; i < 3; i++) {
        v.component[i] = -v.component[i];
    }
#endif
    answers[0] = math_value(machine, module, v);
    *answer_count = 1;
}
#endif

static double math_magnitude(MathValue v)
{
    double sum = 0.0;
    for (size_t i = 0; i < MATH_DIM; i++) {
        sum += (double)v.component[i] * v.component[i];
    }
    return sqrt(sum);
}

#if defined(MATH_QUATERNION)
// Shortest-arc spherical interpolation. Normalize nonzero inputs first so
// scaled representations of the same rotation produce the same unit answer.
// When either input is zero (no rotation to interpolate), use an unclamped
// linear blend instead; it preserves zero endpoints without dividing by zero.
static void math_slerp(LhatMachine *machine, void *context,
                       const LhatValue *args, size_t count,
                       LhatValue *answers, int *answer_count)
{
    const MathModule *module = (const MathModule *)context;
    MathValue a, b, result = { { 0 } };
    if (count < 3 || !math_arg(module, args[0], &a) ||
        !math_arg(module, args[1], &b) || !lhat_is_number(args[2])) {
        return;
    }
    double t = math_real(args[2]);
    double a_length = math_magnitude(a), b_length = math_magnitude(b);
    if (a_length == 0.0 || b_length == 0.0) {
        for (size_t i = 0; i < 4; i++) {
            result.component[i] = (float)((1.0 - t) * a.component[i] +
                                           t * b.component[i]);
        }
    } else {
        double u[4], v[4], dot = 0.0;
        for (size_t i = 0; i < 4; i++) {
            u[i] = a.component[i] / a_length;
            v[i] = b.component[i] / b_length;
            dot += u[i] * v[i];
        }
        if (dot < 0.0) {
            dot = -dot;
            for (size_t i = 0; i < 4; i++) {
                v[i] = -v[i];
            }
        }
        // Near parallel, sin(angle) is too small for a stable quotient.
        double from = 1.0 - t, to = t;
        if (dot < 0.9995) {
            double angle = acos(fmin(dot, 1.0));
            double sine = sin(angle);
            from = sin((1.0 - t) * angle) / sine;
            to = sin(t * angle) / sine;
        }
        double blended[4], length = 0.0;
        for (size_t i = 0; i < 4; i++) {
            blended[i] = from * u[i] + to * v[i];
            length += blended[i] * blended[i];
        }
        length = sqrt(length);
        for (size_t i = 0; i < 4; i++) {
            result.component[i] = (float)(length == 0.0 ? blended[i]
                                                         : blended[i] / length);
        }
    }
    answers[0] = math_value(machine, module, result);
    *answer_count = 1;
}
#endif

static void math_length(LhatMachine *machine, void *context,
                        const LhatValue *args, size_t count,
                        LhatValue *answers, int *answer_count)
{
    (void)machine;
    MathValue v;
    if (count < 1 || !math_arg((const MathModule *)context, args[0], &v)) {
        return;
    }
    answers[0] = lhat_real(math_magnitude(v));
    *answer_count = 1;
}

static void math_normalized(LhatMachine *machine, void *context,
                            const LhatValue *args, size_t count,
                            LhatValue *answers, int *answer_count)
{
    const MathModule *module = (const MathModule *)context;
    MathValue v;
    if (count < 1 || !math_arg(module, args[0], &v)) {
        return;
    }
    double length = math_magnitude(v);
    if (length != 0.0) {
        for (size_t i = 0; i < MATH_DIM; i++) {
            v.component[i] = (float)(v.component[i] / length);
        }
    }
    answers[0] = math_value(machine, module, v);
    *answer_count = 1;
}

#if !defined(MATH_COMPLEX)
static void math_dot(LhatMachine *machine, void *context,
                     const LhatValue *args, size_t count,
                     LhatValue *answers, int *answer_count)
{
    (void)machine;
    const MathModule *module = (const MathModule *)context;
    MathValue a, b;
    if (count < 2 || !math_arg(module, args[0], &a) ||
        !math_arg(module, args[1], &b)) {
        return;
    }
    double sum = 0.0;
    for (size_t i = 0; i < MATH_DIM; i++) {
        sum += (double)a.component[i] * b.component[i];
    }
    answers[0] = lhat_real(sum);
    *answer_count = 1;
}
#endif

#if defined(MATH_VECTOR2) || defined(MATH_VECTOR3)
static void math_cross(LhatMachine *machine, void *context,
                       const LhatValue *args, size_t count,
                       LhatValue *answers, int *answer_count)
{
    const MathModule *module = (const MathModule *)context;
    MathValue a, b;
    if (count < 2 || !math_arg(module, args[0], &a) ||
        !math_arg(module, args[1], &b)) {
        return;
    }
#if defined(MATH_VECTOR2)
    // Signed area: the z component of the lifted 3D cross product.
    (void)machine;
    answers[0] = lhat_real((double)a.component[0] * b.component[1] -
                           (double)a.component[1] * b.component[0]);
#else
    MathValue result = { { 0 } };
    result.component[0] = a.component[1] * b.component[2] - a.component[2] * b.component[1];
    result.component[1] = a.component[2] * b.component[0] - a.component[0] * b.component[2];
    result.component[2] = a.component[0] * b.component[1] - a.component[1] * b.component[0];
    answers[0] = math_value(machine, module, result);
#endif
    *answer_count = 1;
}
#endif

static void math_tostring(LhatMachine *machine, void *context,
                          const LhatValue *args, size_t count,
                          LhatValue *answers, int *answer_count)
{
    MathValue v;
    if (count < 1 || !math_arg((const MathModule *)context, args[0], &v)) {
        return;
    }
    char text[160];
    size_t used = 0;
    text[used++] = '{';
    for (size_t i = 0; i < MATH_DIM; i++) {
        char spelt[64];
        if (lhat_value_text(lhat_real((double)v.component[i]), spelt, sizeof spelt) >= sizeof spelt) {
            return;
        }
        int written = snprintf(text + used, sizeof text - used, "%s%s:%s",
                               i == 0 ? "" : " ", math_names[i], spelt);
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
    answers[0] = lhat_machine_make_string(machine, text, used, &out) ? out : lhat_nil();
    *answer_count = 1;
}

// Register all common members in the same order as vector2/vector3.
static bool math_register(LhatProgram *program, MathModule *module,
                          const char *constructor_signature)
{
    module->tag = lhat_register_hostvalue_type(program, MATH_MODULE, MATH_TYPE,
                                               sizeof(MathValue));
    if (module->tag == NULL) {
        return false;
    }
    for (size_t i = 0; i < MATH_DIM; i++) {
        if (!lhat_register_hostvalue_field(program, MATH_MODULE, MATH_TYPE,
                                           math_names[i], i * sizeof(float),
                                           LHAT_HVFIELD_F32)) {
            return false;
        }
    }
    return lhat_register_hostvalue_member(program, MATH_MODULE, MATH_TYPE, "+",
                                          "f^self^, " MATH_FULL " -> " MATH_FULL ";",
                                          math_add, module) &&
           lhat_register_hostvalue_member(program, MATH_MODULE, MATH_TYPE, "-",
                                          "f^self^, " MATH_FULL " -> " MATH_FULL ";",
                                          math_sub, module) &&
           lhat_register_hostvalue_member(program, MATH_MODULE, MATH_TYPE, "-",
                                          "f^self^ -> " MATH_FULL ";", math_neg, module) &&
           lhat_register_hostvalue_member(program, MATH_MODULE, MATH_TYPE, "*",
                                          "f^self^, number^ -> " MATH_FULL ";",
                                          math_scale, module) &&
           lhat_register_hostvalue_member(program, MATH_MODULE, MATH_TYPE, "*",
                                          "f^number^, self^ -> " MATH_FULL ";",
                                          math_scale, module) &&
#if defined(MATH_VECTOR2) || defined(MATH_VECTOR3) || defined(MATH_VECTOR4)
           lhat_register_hostvalue_member(program, MATH_MODULE, MATH_TYPE, "*",
                                          "f^self^, " MATH_FULL " -> " MATH_FULL ";",
                                          math_hadamard, module) &&
           lhat_register_hostvalue_member(program, MATH_MODULE, MATH_TYPE, "/",
                                          "f^self^, number^ -> " MATH_FULL ";",
                                          math_divide, module) &&
           lhat_register_hostvalue_member(program, MATH_MODULE, MATH_TYPE, "/",
                                          "f^self^, " MATH_FULL " -> " MATH_FULL ";",
                                          math_divide, module) &&
           lhat_register_hostvalue_member(program, MATH_MODULE, MATH_TYPE, "lerp",
                                          "f^self^, " MATH_FULL ", number^ -> " MATH_FULL ";",
                                          math_lerp, module) &&
#endif
#if defined(MATH_VECTOR2)
           lhat_register_hostvalue_member(program, MATH_MODULE, MATH_TYPE, "angle",
                                          "f^self^ -> number^;", math_angle, module) &&
           lhat_register_hostvalue_member(program, MATH_MODULE, MATH_TYPE, "rotate",
                                          "f^self^, number^ -> " MATH_FULL ";",
                                          math_rotate, module) &&
#endif
#if defined(MATH_QUATERNION)
           lhat_register_hostvalue_member(program, MATH_MODULE, MATH_TYPE, "slerp",
                                          "f^self^, " MATH_FULL ", number^ -> " MATH_FULL ";",
                                          math_slerp, module) &&
#endif
#if defined(MATH_COMPLEX) || defined(MATH_QUATERNION)
           lhat_register_hostvalue_member(program, MATH_MODULE, MATH_TYPE, "*",
                                          "f^self^, " MATH_FULL " -> " MATH_FULL ";",
                                          math_mul, module) &&
           lhat_register_hostvalue_member(program, MATH_MODULE, MATH_TYPE, "conjugate",
                                          "f^self^ -> " MATH_FULL ";",
                                          math_conjugate, module) &&
#endif
#if !defined(MATH_COMPLEX)
           // The core's operator table maps both dot^ and ⋅ to the member
           // name "dot"; this registration is their operator arm.
           lhat_register_hostvalue_member(program, MATH_MODULE, MATH_TYPE, "dot",
                                          "f^self^, " MATH_FULL " -> number^;",
                                          math_dot, module) &&
#endif
#if defined(MATH_VECTOR2)
           // cross^ and × likewise share the member name "cross".
           lhat_register_hostvalue_member(program, MATH_MODULE, MATH_TYPE, "cross",
                                          "f^self^, " MATH_FULL " -> number^;",
                                          math_cross, module) &&
#elif defined(MATH_VECTOR3)
           lhat_register_hostvalue_member(program, MATH_MODULE, MATH_TYPE, "cross",
                                          "f^self^, " MATH_FULL " -> " MATH_FULL ";",
                                          math_cross, module) &&
#endif
           lhat_register_hostvalue_member(program, MATH_MODULE, MATH_TYPE, "length",
                                          "f^self^ -> number^;", math_length, module) &&
           lhat_register_hostvalue_member(program, MATH_MODULE, MATH_TYPE, "normalized",
                                          "f^self^ -> " MATH_FULL ";",
                                          math_normalized, module) &&
           lhat_register_hostvalue_member(program, MATH_MODULE, MATH_TYPE, "tostring",
                                          "f^self^ -> string^;", math_tostring, module) &&
           lhat_register_func(program, MATH_MODULE, "new", constructor_signature,
                              math_new, module);
}

#undef MATH_FULL
