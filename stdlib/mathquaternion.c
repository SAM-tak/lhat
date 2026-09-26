// L^ (lhat) -- quaternions as four f32 components (x, y, z, w).
#include "mathquaternion.h"
#include "mathvector3_internal.h"

#define MATH_MODULE "std.math.quaternion"
#define MATH_TYPE "Quaternion"
#define MATH_DIM 4
#define MATH_NAMES { "x", "y", "z", "w" }
#define MATH_QUATERNION
#include "mathvalue_impl.h"

typedef struct {
    MathModule math;  // mathvalue_impl.h's callbacks receive this address
    const LhatHostValueTag *vector3;
} QuaternionModule;

typedef struct {
    float component[3];
} Vector3Value;

static bool vector3_arg(const QuaternionModule *module, LhatValue argument,
                        Vector3Value *out)
{
    const void *bytes = lhat_hostvalue_data(argument, module->vector3);
    if (bytes == NULL) {
        return false;
    }
    memcpy(out, bytes, sizeof *out);
    return true;
}

static LhatValue vector3_value(LhatMachine *machine,
                               const QuaternionModule *module, Vector3Value v)
{
    LhatValue out = lhat_nil();
    return lhat_make_hostvalue(machine, module->vector3, &v, &out)
               ? out : lhat_nil();
}

static double vector3_length(const double v[3])
{
    return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// The shortest unit rotation from one nonzero direction to another. For
// antiparallel vectors, choose a reproducible perpendicular axis by crossing
// with the least-aligned coordinate axis. No direction exists for zero input.
static void quaternion_from_to(LhatMachine *machine, void *context,
                               const LhatValue *args, size_t count,
                               LhatValue *answers, int *answer_count)
{
    const QuaternionModule *module = (const QuaternionModule *)context;
    Vector3Value from, to;
    if (count < 2 || !vector3_arg(module, args[0], &from) ||
        !vector3_arg(module, args[1], &to)) {
        return;
    }
    double a[3], b[3];
    for (size_t i = 0; i < 3; i++) {
        a[i] = from.component[i];
        b[i] = to.component[i];
    }
    double alen = vector3_length(a), blen = vector3_length(b);
    if (alen == 0.0 || blen == 0.0) {
        answers[0] = lhat_nil();
        *answer_count = 1;
        return;
    }
    for (size_t i = 0; i < 3; i++) {
        a[i] /= alen;
        b[i] /= blen;
    }
    double dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
    MathValue q = { { 0 } };
    if (dot > 1.0 - 1e-12) {
        q.component[3] = 1.0f;
    } else if (dot < -1.0 + 1e-12) {
        size_t basis = 0;
        for (size_t i = 1; i < 3; i++) {
            if (fabs(a[i]) < fabs(a[basis])) {
                basis = i;
            }
        }
        double axis[3] = { 0.0, 0.0, 0.0 };
        // a × unit[basis].
        axis[(basis + 1) % 3] = a[(basis + 2) % 3];
        axis[(basis + 2) % 3] = -a[(basis + 1) % 3];
        double length = vector3_length(axis);
        for (size_t i = 0; i < 3; i++) {
            q.component[i] = (float)(axis[i] / length);
        }
    } else {
        double cross[3] = { a[1]*b[2] - a[2]*b[1],
                            a[2]*b[0] - a[0]*b[2],
                            a[0]*b[1] - a[1]*b[0] };
        double length = sqrt(cross[0]*cross[0] + cross[1]*cross[1] +
                             cross[2]*cross[2] + (1.0 + dot)*(1.0 + dot));
        for (size_t i = 0; i < 3; i++) {
            q.component[i] = (float)(cross[i] / length);
        }
        q.component[3] = (float)((1.0 + dot) / length);
    }
    answers[0] = math_value(machine, &module->math, q);
    *answer_count = 1;
}

// Right-handed axis/angle rotation. Zero axis has no direction and answers
// nil^, just as fromTo does for a zero input direction.
static void quaternion_from_axis_angle(LhatMachine *machine, void *context,
                                       const LhatValue *args, size_t count,
                                       LhatValue *answers, int *answer_count)
{
    const QuaternionModule *module = (const QuaternionModule *)context;
    Vector3Value axis;
    if (count < 2 || !vector3_arg(module, args[0], &axis) ||
        !lhat_is_number(args[1])) {
        return;
    }
    double v[3] = { axis.component[0], axis.component[1], axis.component[2] };
    double length = vector3_length(v);
    if (length == 0.0) {
        answers[0] = lhat_nil();
        *answer_count = 1;
        return;
    }
    double half = math_real(args[1]) * 0.5;
    double sine = sin(half) / length;
    MathValue q = { { (float)(v[0] * sine), (float)(v[1] * sine),
                      (float)(v[2] * sine), (float)cos(half) } };
    answers[0] = math_value(machine, &module->math, q);
    *answer_count = 1;
}

// Rotate a Vector3 by this quaternion, normalizing it first. For the zero
// quaternion (no defined rotation), leave the input vector unchanged.
static void quaternion_rotate(LhatMachine *machine, void *context,
                              const LhatValue *args, size_t count,
                              LhatValue *answers, int *answer_count)
{
    const QuaternionModule *module = (const QuaternionModule *)context;
    MathValue q;
    Vector3Value v;
    if (count < 2 || !math_arg(&module->math, args[0], &q) ||
        !vector3_arg(module, args[1], &v)) {
        return;
    }
    double length = math_magnitude(q);
    if (length != 0.0) {
        double x = q.component[0] / length, y = q.component[1] / length;
        double z = q.component[2] / length, w = q.component[3] / length;
        double tx = 2.0 * (y * v.component[2] - z * v.component[1]);
        double ty = 2.0 * (z * v.component[0] - x * v.component[2]);
        double tz = 2.0 * (x * v.component[1] - y * v.component[0]);
        Vector3Value rotated = { { (float)(v.component[0] + w*tx + y*tz - z*ty),
                                   (float)(v.component[1] + w*ty + z*tx - x*tz),
                                   (float)(v.component[2] + w*tz + x*ty - y*tx) } };
        v = rotated;
    }
    answers[0] = vector3_value(machine, module, v);
    *answer_count = 1;
}

bool lhatstdlib_mathquaternion_register(LhatProgram *program)
{
    static QuaternionModule shared;
    shared.vector3 = lhatstdlib_mathvector3_tag();
    if (shared.vector3 == NULL ||
        !math_register(program, &shared.math,
                       "f^number^, number^, number^, number^ -> std.math.quaternion.Quaternion;") ||
        !math_register_constant(program, &shared.math, 0, "identity",
                                (MathValue){{0, 0, 0, 1}})) {
        return false;
    }
    return lhat_register_func(
               program, "std.math.quaternion", "fromTo",
               "f^std.math.vector3.Vector3, std.math.vector3.Vector3 -> std.math.quaternion.Quaternion|nil^;",
               quaternion_from_to, &shared) &&
           lhat_register_func(
               program, "std.math.quaternion", "fromAxisAngle",
               "f^std.math.vector3.Vector3, number^ -> std.math.quaternion.Quaternion|nil^;",
               quaternion_from_axis_angle, &shared) &&
           lhat_register_hostvalue_member(
               program, "std.math.quaternion", "Quaternion", "rotate",
               "f^self^, std.math.vector3.Vector3 -> std.math.vector3.Vector3;",
               quaternion_rotate, &shared);
}
