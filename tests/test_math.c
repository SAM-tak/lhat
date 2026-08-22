// L^ (lhat) -- tests for std.math, and for what number^ itself gained
// beside it (02 の 14.21改's abs/sign/clamp, 14.8改2's constants, and '**'
// taking any exponent).
//
// Every case asks its question from inside L^. Reals are compared with
// '=' where 14.8's tolerance is wanted and with '.eq(x, 0)' where an exact
// answer is the point -- the quarter turns, which std.math promises exactly.

#include "stdlibutil.h"
#include "testutil.h"

#include "../stdlib/math.h"

static const LhatTestRegister regs[] = {lhatstdlib_math_register};

static LhatTestRan run_source(const char *text)
{
    return lhat_test_run(regs, 1, text);
}

static bool checks(const char *text)
{
    return lhat_test_check_text(regs, 1, text);
}

// Runs `expr` and answers 1 when it is exactly `exact`.
#define EXACTLY(expr, exact)                                     \
    "import^ std.math\n"                                         \
    "if^ (" expr ").eq(" exact ", 0) { return^ 1 }\n"            \
    "return^ 0\n"

static void test_degrees(void)
{
    LHAT_TEST("the quarter turns are exact");
    {
        static const char *const cases[] = {
            EXACTLY("std.math.sin(90)", "1"),
            EXACTLY("std.math.cos(90)", "0"),
            EXACTLY("std.math.sin(180)", "0"),
            EXACTLY("std.math.cos(180)", "-1"),
            EXACTLY("std.math.sin(-90)", "-1"),
            EXACTLY("std.math.cos(720)", "1"),
            EXACTLY("std.math.tan(180)", "0"),
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            LhatTestRan ran = run_source(cases[i]);
            LHAT_CHECK_RAN_INTEGER(ran, 1);
            lhat_test_ran_dispose(&ran);
        }
    }

    LHAT_TEST("and the rest are degrees in, degrees out");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ ok = std.math.sin(30) = 0.5 and^ std.math.cos(60) = 0.5\n"
            "    and^ std.math.asin(1) = 90 and^ std.math.acos(0.5) = 60\n"
            "    and^ std.math.atan(1) = 45 and^ std.math.atan2(1, -1) = 135\n"
            "    and^ std.math.rad(180) = number^.pi and^ std.math.deg(number^.pi) = 180\n"
            "if^ ok { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_functions(void)
{
    LHAT_TEST("the one-argument functions");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ ok = std.math.sqrt(16) = 4 and^ std.math.cbrt(27) = 3\n"
            "    and^ std.math.exp(0) = 1 and^ std.math.log(number^.e) = 1\n"
            "    and^ std.math.log2(8) = 3 and^ std.math.log10(1000) = 3\n"
            "if^ ok { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // 02 の 14.12: log registered twice is one name with two arms.
    LHAT_TEST("log takes a base as a second arm");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "if^ std.math.log(81, 3) = 4 and^ std.math.log(1) = 0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("the two-argument functions");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ ok = std.math.hypot(3, 4) = 5 and^ std.math.fmod(-7, 3) = -1\n"
            "    and^ std.math.lerp(10, 20, 0.25) = 12.5\n"
            "if^ ok { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // 14.8改: min and max answer the argument as it came, so integers stay
    // integers -- and 13.7's tail takes any number of them.
    LHAT_TEST("min and max take one or more and keep the representation");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ m = std.math.max(3, 9, 4)\n"
            "let^ n = std.math.min(3)\n"
            "if^ m is^ 9 and^ n is^ 3 and^ std.math.min(2.5, 1) = 1 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("and none of them takes a string");
    LHAT_CHECK(!checks("import^ std.math\nreturn^ std.math.sqrt(\"4\")\n"),
               "the signature says number^");
}

static void test_number_members(void)
{
    // 02 の 14.21改: abs, sign and clamp beside the roundings, on the value
    // itself. An integer stays one.
    LHAT_TEST("abs, sign and clamp are members of number^");
    {
        LhatTestRan ran = run_source(
            "let^ ok = (-5).abs() is^ 5 and^ (-2.5).abs() = 2.5\n"
            "    and^ (-7).sign() is^ -1 and^ (0).sign() is^ 0 and^ (0.5).sign() is^ 1\n"
            "    and^ (15).clamp(0, 10) is^ 10 and^ (-3).clamp(0, 10) is^ 0\n"
            "    and^ (2.5).clamp(0, 10) = 2.5\n"
            "if^ ok { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("clamp asks for its two bounds");
    LHAT_CHECK(!checks("return^ (5).clamp(1)\n"), "one bound is not enough");

    // 02 の 14.8改2: the constants are static members of the type's own
    // word; nothing else is.
    LHAT_TEST("number^ carries its constants");
    {
        LhatTestRan ran = run_source(
            "let^ ok = number^.tau = 2 * number^.pi and^ number^.e > 2.718\n"
            "    and^ number^.inf > 1e308 and^ -number^.inf < -1e308\n"
            "    and^ !(number^.nan = number^.nan)\n"
            "if^ ok { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
    LHAT_CHECK(!checks("return^ number^.phi\n"), "no such constant");
    LHAT_CHECK(!checks("return^ number^\n"), "the bare word is still no value");

    // 14.8改: '**' takes any exponent now, and always answers a real.
    LHAT_TEST("** takes a fractional exponent");
    {
        LhatTestRan ran = run_source(
            "if^ 2 ** 0.5 = 1.4142135623730951 and^ 2 ** 10 = 1024\n"
            "    and^ 2 ** -1 = 0.5 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

int main(void)
{
    test_degrees();
    test_functions();
    test_number_members();
    return lhat_test_report("test_math");
}
