// L^ (lhat) -- sample standard library: std.math (see math.h).
//
// Thin wrappers over <math.h>. Every argument is read as a double through
// lhat_number_as_real, and every answer is a real -- except min and max,
// which hand back the argument they chose as it came, so an integer stays
// one (02 の 14.8改). A wrong argument answers nil^, which is what the
// checker already ruled out; nothing here can fail otherwise.

#include "math.h"

#include <math.h>

static const double degrees_per_radian = 57.29577951308232087680;

static bool number_arg(LhatValue value, double *out)
{
    if (!lhat_is_number(value)) {
        return false;
    }
    *out = lhat_number_as_real(value);
    return true;
}

// One double in, one double out (or two in) is most of the module, so one
// LhatHostFn serves them all and the context says which C function. A
// function pointer is not a void *, so the entry itself is the context.
typedef struct {
    const char *name;
    const char *signature;
    double (*one)(double);
    double (*two)(double, double);
} Entry;

static void unary(LhatMachine *machine, void *context,
                  const LhatValue *arguments, size_t count,
                  LhatValue *answers, int *answer_count)
{
    (void)machine;
    double x = 0.0;
    if (count != 1 || !number_arg(arguments[0], &x)) {
        return;
    }
    answers[0] = lhat_real(((const Entry *)context)->one(x));
    *answer_count = 1;
}

static void binary(LhatMachine *machine, void *context,
                   const LhatValue *arguments, size_t count,
                   LhatValue *answers, int *answer_count)
{
    (void)machine;
    double x = 0.0;
    double y = 0.0;
    if (count != 2 || !number_arg(arguments[0], &x) ||
        !number_arg(arguments[1], &y)) {
        return;
    }
    answers[0] = lhat_real(((const Entry *)context)->two(x, y));
    *answer_count = 1;
}

// Through wrappers rather than by address: a C library function may be an
// import whose address is not a constant (MSVC), and a static table wants
// one. Angles are radians, as <math.h> and Lua take them, so the trigonometry
// is libm's as it stands.
static double sin_of(double x) { return sin(x); }
static double cos_of(double x) { return cos(x); }
static double tan_of(double x) { return tan(x); }
static double asin_of(double x) { return asin(x); }
static double acos_of(double x) { return acos(x); }
static double atan_of(double x) { return atan(x); }
static double atan2_of(double y, double x) { return atan2(y, x); }
static double to_radians(double degrees) { return degrees / degrees_per_radian; }
static double to_degrees(double radians) { return radians * degrees_per_radian; }
static double sqrt_of(double x) { return sqrt(x); }
static double cbrt_of(double x) { return cbrt(x); }
static double exp_of(double x) { return exp(x); }
static double log_of(double x) { return log(x); }
static double log2_of(double x) { return log2(x); }
static double log10_of(double x) { return log10(x); }
static double hypot_of(double x, double y) { return hypot(x, y); }
static double fmod_of(double x, double y) { return fmod(x, y); }
static double log_base(double x, double base) { return log(x) / log(base); }

static double lerp(double a, double b, double t) { return a + (b - a) * t; }

static void math_lerp(LhatMachine *machine, void *context,
                      const LhatValue *arguments, size_t count,
                      LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    double a = 0.0, b = 0.0, t = 0.0;
    if (count != 3 || !number_arg(arguments[0], &a) ||
        !number_arg(arguments[1], &b) || !number_arg(arguments[2], &t)) {
        return;
    }
    answers[0] = lhat_real(lerp(a, b, t));
    *answer_count = 1;
}

// 13.7: one required, the rest variadic -- the tail reaches a host
// function uncollected, so the arguments are simply the whole array.
static LhatValue extreme(const LhatValue *arguments, size_t count, bool most)
{
    if (count == 0) {
        return lhat_nil();
    }
    LhatValue best = arguments[0];
    double best_real = 0.0;
    if (!number_arg(best, &best_real)) {
        return lhat_nil();
    }
    for (size_t i = 1; i < count; i++) {
        double candidate = 0.0;
        if (!number_arg(arguments[i], &candidate)) {
            return lhat_nil();
        }
        if (most ? candidate > best_real : candidate < best_real) {
            best = arguments[i];
            best_real = candidate;
        }
    }
    return best;
}

static void math_max(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count,
                     LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    answers[0] = extreme(arguments, count, true);
    *answer_count = 1;
}

static void math_min(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count,
                     LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    answers[0] = extreme(arguments, count, false);
    *answer_count = 1;
}

// ---- Registration ---------------------------------------------------------

bool lhatstdlib_math_register(LhatProgram *program)
{
    // 13.4: registration signatures take no parameter names. Every
    // function here is an f^: a number in, a number out, nothing touched.
#define ONE "f^number^ -> number^;"
#define TWO "f^number^, number^ -> number^;"
    static const Entry entries[] = {
        {"sin", ONE, sin_of, NULL},
        {"cos", ONE, cos_of, NULL},
        {"tan", ONE, tan_of, NULL},
        {"asin", ONE, asin_of, NULL},
        {"acos", ONE, acos_of, NULL},
        {"atan", ONE, atan_of, NULL},
        {"atan2", TWO, NULL, atan2_of},
        {"deg", ONE, to_degrees, NULL},
        {"rad", ONE, to_radians, NULL},
        {"sqrt", ONE, sqrt_of, NULL},
        {"cbrt", ONE, cbrt_of, NULL},
        {"exp", ONE, exp_of, NULL},
        // 02 の 14.12: a name registered twice gains an arm, and the two
        // are told apart by their counts.
        {"log", ONE, log_of, NULL},
        {"log", TWO, NULL, log_base},
        {"log2", ONE, log2_of, NULL},
        {"log10", ONE, log10_of, NULL},
        {"hypot", TWO, NULL, hypot_of},
        {"fmod", TWO, NULL, fmod_of},
    };
#undef ONE
#undef TWO
    for (size_t i = 0; i < sizeof entries / sizeof entries[0]; i++) {
        const Entry *e = &entries[i];
        if (!lhat_register_func(program, "std.math", e->name, e->signature,
                                e->one != NULL ? unary : binary,
                                (void *)e)) {
            return false;
        }
    }
    // The mathematical constants live beside the functions they serve.
    // number^ keeps inf and nan, which are about the representation.
    return lhat_register_const_real(program, "std.math", NULL, "pi",
                                    3.14159265358979323846) &&
           lhat_register_const_real(program, "std.math", NULL, "tau",
                                    6.28318530717958647692) &&
           lhat_register_const_real(program, "std.math", NULL, "e",
                                    2.71828182845904523536) &&
           lhat_register_func(program, "std.math", "min",
                              "f^number^, ...:number^ -> number^;", math_min,
                              NULL) &&
           lhat_register_func(program, "std.math", "max",
                              "f^number^, ...:number^ -> number^;", math_max,
                              NULL) &&
           lhat_register_func(program, "std.math", "lerp",
                              "f^number^, number^, number^ -> number^;",
                              math_lerp, NULL);
}
