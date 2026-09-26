// L^ (lhat) -- tests for std.math.vector2.
//
// test_mathvector3.c pins 05 の 8.9's machinery on a three-component value;
// this pins the arithmetic of the two-component one, and the machinery again
// where a narrower value could take a different path -- a parameter's width,
// a coroutine's saved registers, a box's bytes. The numbers are chosen so a
// wrong slot somewhere answers a wrong number here.

#include "stdlibutil.h"
#include "testutil.h"

#include "../stdlib/mathvector2.h"

static const LhatTestRegister regs[] = {lhatstdlib_mathvector2_register};

static LhatTestRan run_source(const char *text)
{
    return lhat_test_run(regs, 1, text);
}

static bool checks(const char *text)
{
    return lhat_test_check_text(regs, 1, text);
}

static void test_fields(void)
{
    LHAT_TEST("fields read the bytes back");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector2\n"
            "let^ v = std.math.vector2.new(3, 4)\n"
            "if^ v.x = 3.0 and^ v.y = 4.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("fields write the value in place, and only this copy");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector2\n"
            "var^ a = std.math.vector2.new(1, 2)\n"
            "let^ b = a\n"
            "a.x := 7\n"
            "a.y := a.x + 2\n"
            "if^ a.x = 7.0 and^ a.y = 9.0 and^ b.x = 1.0 and^ b.y = 2.0 {\n"
            "    return^ 1\n"
            "}\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_operators(void)
{
    LHAT_TEST("operators chain through stack temporaries");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector2\n"
            "let^ a = std.math.vector2.new(1, 2)\n"
            "let^ b = std.math.vector2.new(10, 20)\n"
            "let^ v = (a + b) * 2 - a\n"
            "if^ v.x = 21.0 and^ v.y = 42.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("the unary '-' negates both components");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector2\n"
            "let^ v = -std.math.vector2.new(1, 2)\n"
            "if^ v.x = -1.0 and^ v.y = -2.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a scalar scales from either side");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector2\n"
            "let^ v = std.math.vector2.new(1, 3)\n"
            "let^ a = 2 * v\n"
            "let^ b = v * 2\n"
            "if^ a = b and^ a.y = 6.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("and an arm that was not written is not invented");
    LHAT_CHECK(!checks("import^ std.math.vector2\n"
                       "let^ v = std.math.vector2.new(1, 2)\n"
                       "let^ w = 2 - v\n"
                       "return^ 1\n"),
               "no arm answers a scalar on the left of '-'");
}

static void test_methods(void)
{
    LHAT_TEST("dot, length and normalized");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector2\n"
            "let^ v = std.math.vector2.new(3, 4)\n"
            "let^ u = std.math.vector2.new(0, 2).normalized()\n"
            "let^ z = std.math.vector2.new(0, 0).normalized()\n"
            "if^ v.length() = 5.0 and^ v.dot(v) = 25.0 and^ u.x = 0.0\n"
            "    and^ u.y = 1.0 and^ z.x = 0.0 and^ z.y = 0.0 {\n"
            "    return^ 1\n"
            "}\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // The sign says the turn: counter-clockwise from x to y is positive.
    LHAT_TEST("cross answers the signed area");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector2\n"
            "let^ x = std.math.vector2.new(1, 0)\n"
            "let^ y = std.math.vector2.new(0, 1)\n"
            "let^ a = std.math.vector2.new(2, 3)\n"
            "let^ b = std.math.vector2.new(5, 7)\n"
            "if^ x.cross(y) = 1.0 and^ y.cross(x) = -1.0\n"
            "    and^ a.cross(b) = -1.0 and^ a.cross(a) = 0.0 {\n"
            "    return^ 1\n"
            "}\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_equality_and_narrowing(void)
{
    LHAT_TEST("equality is bytes, and fits^ knows the type");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector2\n"
            "let^ a = std.math.vector2.new(1, 2)\n"
            "let^ b = std.math.vector2.new(1, 2)\n"
            "let^ c = std.math.vector2.new(1, 3)\n"
            "let^ n = 7\n"
            "if^ a = b and^ a != c and^ a fits^ std.math.vector2.Vector2\n"
            "    and^ !(n fits^ std.math.vector2.Vector2) {\n"
            "    return^ 1\n"
            "}\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

// A parameter reserves the registered width, and the caller lays the
// argument down the same way -- written, inferred, or after a receiver.
static void test_subroutines(void)
{
    LHAT_TEST("host values pass through an L^ subroutine whole");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector2\n"
            "let^ Vector2 = std.math.vector2.Vector2\n"
            "let^ blend = f^p:Vector2, q:Vector2 -> Vector2 { (p + q) * 0.5 }\n"
            "let^ mid = blend(std.math.vector2.new(2, 4),"
            " std.math.vector2.new(4, 8))\n"
            "if^ mid.x = 3.0 and^ mid.y = 6.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("and so does one whose type was inferred");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector2\n"
            "let^ blend = f^p:std.math.vector2.Vector2, q { (p + q) * 0.5 }\n"
            "let^ v = std.math.vector2.new(3, 4)\n"
            "let^ mid = blend(v, v * 2)\n"
            "if^ mid.x = 4.5 and^ mid.y = 6.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a receiver does not put the parameters out of step");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector2\n"
            "let^ Holder = def^{\n"
            "  self^{ },\n"
            "  mix := f^self^, a:std.math.vector2.Vector2,"
            " b:std.math.vector2.Vector2, k:number^ -> number^ {\n"
            "    return^ (a + b).y * k\n"
            "  },\n"
            "}\n"
            "let^ h = Holder.new()\n"
            "let^ v = std.math.vector2.new(3, 4)\n"
            "if^ h.mix(v, v * 2, 10) = 120.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_coroutines(void)
{
    LHAT_TEST("a coroutine's own host value locals survive suspension");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector2\n"
            "let^ gen = p^ {\n"
            "    let^ held = std.math.vector2.new(4, 5)\n"
            "    yield^ 1\n"
            "    yield^ held.y\n"
            "}\n"
            "let^ co = gen()\n"
            "co.start()\n"
            "let^ second = co.resume()\n"
            "if^ second = 5.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a yield^ carries a host value whole");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector2\n"
            "let^ gen = f^ -> c^{f^ -> std.math.vector2.Vector2} {\n"
            "    yield^ std.math.vector2.new(1, 2)\n"
            "    yield^ std.math.vector2.new(4, 5)\n"
            "}\n"
            "var^ total = 0\n"
            "for^ v in^ gen() { total := total + v.x * v.y }\n"
            "if^ total = 22.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_boxing(void)
{
    LHAT_TEST("box^ boxes and get/set unbox and write");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector2\n"
            "let^ a = box^std.math.vector2.new(1, 2)\n"
            "let^ b = box^std.math.vector2.new(10, 20)\n"
            "let^ c = box^std.math.vector2.new(0, 0)\n"
            "c.set(a.get() + b.get())\n"
            "let^ landed = c.get()\n"
            "if^ landed.x = 11.0 and^ landed.y = 22.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a constbox^ keys a table by its bytes");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector2\n"
            "let^ t = { [constbox^std.math.vector2.new(1, 2)] = 7 }\n"
            "var^ r = 0\n"
            "if^ t[constbox^std.math.vector2.new(1, 2)] = 7 { r := r + 1 }\n"
            "if^ t[std.math.vector2.new(1, 2)] = 7 { r := r + 10 }\n"
            "if^ t[std.math.vector2.new(2, 1)] = nil^ { r := r + 100 }\n"
            "return^ r\n");
        LHAT_CHECK_RAN_INTEGER(ran, 111);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_tostring(void)
{
    LHAT_TEST("a host value spells itself");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector2\n"
            "return^ std.math.vector2.new(0.5, -2).tostring()\n");
        LHAT_CHECK_RAN_TEXT(ran, "{x:0.5 y:-2.0}");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("and an interpolation hole writes that same spelling");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector2\n"
            "let^ v = std.math.vector2.new(1, 2)\n"
            "return^ $\"v = {v}, -v = {-v}\"\n");
        LHAT_CHECK_RAN_TEXT(ran, "v = {x:1.0 y:2.0}, -v = {x:-1.0 y:-2.0}");
        lhat_test_ran_dispose(&ran);
    }
}

static void test_collection(void)
{
    LHAT_TEST("a collection walks past host values in registers");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector2\n"
            "let^ v = std.math.vector2.new(9, 8)\n"
            "var^ n = 0\n"
            "for^ i from^ 1 to^ 64 {\n"
            "    let^ w = v + std.math.vector2.new(i, 0)\n"
            "    n := n + w.y\n"
            "}\n"
            "L^.collectgarbage()\n"
            "if^ v.x = 9.0 and^ n = 512.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

// 05 の 8.9: the places a host value may not go, narrow as this one is.
static void test_escapes(void)
{
    LHAT_TEST("a table member refuses a host value");
    LHAT_CHECK(!checks("import^ std.math.vector2\n"
                       "let^ t = { v := std.math.vector2.new(1, 2) }\n"),
               "table literal");

    LHAT_TEST("a capture refuses a host value");
    LHAT_CHECK(!checks("import^ std.math.vector2\n"
                       "let^ v = std.math.vector2.new(1, 2)\n"
                       "let^ f = f^ -> number^ { return^ v.x }\n"),
               "capture");

    LHAT_TEST("the program's answer refuses a host value");
    LHAT_CHECK(!checks("import^ std.math.vector2\n"
                       "return^ std.math.vector2.new(1, 2)\n"),
               "top-level return");
}

int main(void)
{
    test_fields();
    test_operators();
    test_methods();
    test_equality_and_narrowing();
    test_subroutines();
    test_coroutines();
    test_boxing();
    test_tostring();
    test_collection();
    test_escapes();
    return lhat_test_report("test_mathvector2");
}
