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

static LhatValue unary(LhatMachine *machine, void *context,
                       const LhatValue *arguments, size_t count)
{
    (void)machine;
    double x = 0.0;
    if (count != 1 || !number_arg(arguments[0], &x)) {
        return lhat_nil();
    }
    return lhat_real(((const Entry *)context)->one(x));
}

static LhatValue binary(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count)
{
    (void)machine;
    double x = 0.0;
    double y = 0.0;
    if (count != 2 || !number_arg(arguments[0], &x) ||
        !number_arg(arguments[1], &y)) {
        return lhat_nil();
    }
    return lhat_real(((const Entry *)context)->two(x, y));
}

// ---- Degrees --------------------------------------------------------------

static double to_radians(double degrees) { return degrees / degrees_per_radian; }
static double to_degrees(double radians) { return radians * degrees_per_radian; }

// The angle reduced to [0, 360), and whether it sits on a quarter turn --
// where the answers are exact and the libm ones are not (cos(pi/2) is
// 6e-17). An integer multiple of 90 reduces exactly in double.
static bool quarter_turn(double degrees, int *which)
{
    double r = fmod(degrees, 360.0);
    if (r < 0) {
        r += 360.0;
    }
    if (r == 0.0 || r == 90.0 || r == 180.0 || r == 270.0) {
        *which = (int)(r / 90.0);
        return true;
    }
    return false;
}

static double sin_degrees(double degrees)
{
    static const double exact[4] = {0.0, 1.0, 0.0, -1.0};
    int q = 0;
    return quarter_turn(degrees, &q) ? exact[q] : sin(to_radians(degrees));
}

static double cos_degrees(double degrees)
{
    static const double exact[4] = {1.0, 0.0, -1.0, 0.0};
    int q = 0;
    return quarter_turn(degrees, &q) ? exact[q] : cos(to_radians(degrees));
}

static double tan_degrees(double degrees)
{
    int q = 0;
    if (quarter_turn(degrees, &q)) {
        // 0 and 180 are flat; 90 and 270 have no tangent.
        return (q % 2 == 0) ? 0.0 : (q == 1 ? HUGE_VAL : -HUGE_VAL);
    }
    return tan(to_radians(degrees));
}

static double asin_degrees(double x) { return to_degrees(asin(x)); }
static double acos_degrees(double x) { return to_degrees(acos(x)); }
static double atan_degrees(double x) { return to_degrees(atan(x)); }
static double atan2_degrees(double y, double x) { return to_degrees(atan2(y, x)); }

// ---- The rest -------------------------------------------------------------

// Through wrappers rather than by address: a C library function may be an
// import whose address is not a constant (MSVC), and a static table wants
// one.
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

static LhatValue math_lerp(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    double a = 0.0, b = 0.0, t = 0.0;
    if (count != 3 || !number_arg(arguments[0], &a) ||
        !number_arg(arguments[1], &b) || !number_arg(arguments[2], &t)) {
        return lhat_nil();
    }
    return lhat_real(lerp(a, b, t));
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

static LhatValue math_max(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    return extreme(arguments, count, true);
}

static LhatValue math_min(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    return extreme(arguments, count, false);
}

// ---- Registration ---------------------------------------------------------

bool lhatstdlib_math_register(LhatProgram *program)
{
    // 13.4: registration signatures take no parameter names. Every
    // function here is an f^: a number in, a number out, nothing touched.
#define ONE "f^number^ -> number^;"
#define TWO "f^number^, number^ -> number^;"
    static const Entry entries[] = {
        {"sin", ONE, sin_degrees, NULL},
        {"cos", ONE, cos_degrees, NULL},
        {"tan", ONE, tan_degrees, NULL},
        {"asin", ONE, asin_degrees, NULL},
        {"acos", ONE, acos_degrees, NULL},
        {"atan", ONE, atan_degrees, NULL},
        {"atan2", TWO, NULL, atan2_degrees},
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
    return lhat_register_func(program, "std.math", "min",
                              "f^number^, ...:number^ -> number^;", math_min,
                              NULL) &&
           lhat_register_func(program, "std.math", "max",
                              "f^number^, ...:number^ -> number^;", math_max,
                              NULL) &&
           lhat_register_func(program, "std.math", "lerp",
                              "f^number^, number^, number^ -> number^;",
                              math_lerp, NULL);
}
