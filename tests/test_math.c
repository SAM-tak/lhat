// L^ (lhat) -- tests for std.math, which is the proving ground for 05 の
// 8.9's host values.
//
// What is pinned here is the machinery rather than the arithmetic: a host
// value crossing every boundary it is allowed to cross (an operator, a
// method, an L^ subroutine's parameters and result, a coroutine's own
// locals), refusing every one it is not (a table, a capture, a yield, the
// program's answer, an interpolation hole), equality being bytes rather
// than heads, and fields reading and writing the value in place. The
// numbers are chosen so a wrong slot somewhere answers a wrong number here.

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

// ---------------------------------------------------------------------------
// The value side
// ---------------------------------------------------------------------------

static void test_fields(void)
{
    LHAT_TEST("fields read the bytes back");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ v = std.math.lvec3(3, 4, 12)\n"
            "if^ v.x = 3.0 and^ v.y = 4.0 and^ v.z = 12.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("fields write the value in place, and only this copy");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "var^ a = std.math.lvec3(1, 2, 3)\n"
            "let^ b = a\n"
            "a.x := 7\n"
            "a.z := a.x + 2\n"
            "if^ a.x = 7.0 and^ a.z = 9.0 and^ b.x = 1.0 and^ b.z = 3.0 {\n"
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
        // ((a + b) * 2 - a).dot(unit x) = (2*1+2*10-1) = 21 when a=(1,2,3),
        // b=(10,20,30): x of (a+b)*2-a is 21.
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ a = std.math.lvec3(1, 2, 3)\n"
            "let^ b = std.math.lvec3(10, 20, 30)\n"
            "let^ v = (a + b) * 2 - a\n"
            "if^ v.x = 21.0 and^ v.y = 42.0 and^ v.z = 63.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("methods answer numbers and values alike");
    {
        // (2,0,0): the one normalized answer float represents exactly.
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ v = std.math.lvec3(3, 4, 0)\n"
            "let^ u = std.math.lvec3(2, 0, 0).normalized()\n"
            "if^ v.length() = 5.0 and^ v.dot(v) = 25.0 and^ u.x = 1.0 {\n"
            "    return^ 1\n"
            "}\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_equality(void)
{
    // The regression this pins: a head-only compare would call two
    // same-typed values equal whatever their bytes.
    LHAT_TEST("equality is bytes, not heads");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ a = std.math.lvec3(1, 2, 3)\n"
            "let^ b = std.math.lvec3(1, 2, 3)\n"
            "let^ c = std.math.lvec3(1, 2, 4)\n"
            "if^ a = b and^ !(a = c) and^ a != c { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_narrowing(void)
{
    LHAT_TEST("isa^ answers against the registered type");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ v = std.math.lvec3(1, 2, 3)\n"
            "if^ v isa^ std.math.LVector3 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_subroutines(void)
{
    LHAT_TEST("host values pass through an L^ subroutine whole");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ blend = f^p:std.math.LVector3, q:std.math.LVector3"
            " -> std.math.LVector3 {\n"
            "    return^ (p + q) * 0.5\n"
            "}\n"
            "let^ mid = blend(std.math.lvec3(2, 4, 6),"
            " std.math.lvec3(4, 8, 10))\n"
            "if^ mid.x = 3.0 and^ mid.y = 6.0 and^ mid.z = 8.0 {\n"
            "    return^ 1\n"
            "}\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a wide answer survives the finally^ drain");
    {
        // The RETURN wrote the whole width into the caller's slot before the
        // drain; the cleanup writing its own registers over must not tear it.
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "var^ log = { n := 0 }\n"
            "let^ make = p^ -> std.math.LVector3 {\n"
            "    do^{\n"
            "        return^ std.math.lvec3(5, 6, 7)\n"
            "    finally^:\n"
            "        log.n := 1\n"
            "    }\n"
            "}\n"
            "let^ v = make()\n"
            "if^ v.x = 5.0 and^ v.z = 7.0 and^ log.n = 1 {\n"
            "    return^ 1\n"
            "}\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_coroutine_locals(void)
{
    LHAT_TEST("a coroutine's own host value locals survive suspension");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ gen = p^ {\n"
            "    let^ held = std.math.lvec3(4, 5, 6)\n"
            "    yield^ 1\n"
            "    yield^ held.y\n"
            "}\n"
            "let^ co = gen()\n"
            "co.start()\n"
            "let^ second = co.resume(nil^)\n"
            "if^ second = 5.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_boxing(void)
{
    LHAT_TEST("the container boxes and unboxes");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ a = std.math.Vector3.new(1, 2, 3) as^ std.math.Vector3\n"
            "let^ b = std.math.Vector3.new(10, 20, 30) as^ std.math.Vector3\n"
            "let^ c = std.math.Vector3.new(0, 0, 0) as^ std.math.Vector3\n"
            "c.set(a.get() + b.get())\n"
            "let^ landed = c.get()\n"
            "a.dispose()\n"
            "b.dispose()\n"
            "c.dispose()\n"
            "if^ landed.x = 11.0 and^ landed.y = 22.0 and^ landed.z = 33.0 {\n"
            "    return^ 1\n"
            "}\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_collection(void)
{
    // 05 の 8.9: nothing here is the collector's -- a host value in a
    // register must read the same after a full cycle, and the CONT slots
    // must never be walked as references.
    LHAT_TEST("a collection walks past host values in registers");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ v = std.math.lvec3(9, 8, 7)\n"
            "var^ n = 0\n"
            "for^ i from^ 1 to^ 64 {\n"
            "    let^ w = v + std.math.lvec3(i, 0, 0)\n"
            "    n := n + w.y\n"
            "}\n"
            "L^.collectgarbage()\n"
            "if^ v.x = 9.0 and^ n = 512.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

// ---------------------------------------------------------------------------
// The escape rules (05 の 8.9): every one of these must refuse to check
// ---------------------------------------------------------------------------

static void test_escapes(void)
{
    LHAT_TEST("a table member refuses a host value");
    LHAT_CHECK(!checks("import^ std.math\n"
                       "let^ t = { v := std.math.lvec3(1, 2, 3) }\n"),
               "table literal");

    LHAT_TEST("a written table type refuses one too");
    LHAT_CHECK(!checks("import^ std.math\n"
                       "let^ f = f^t:t^{ v : std.math.LVector3 } -> number^ {\n"
                       "    return^ 1\n"
                       "}\n"),
               "written member type");

    LHAT_TEST("a capture refuses a host value");
    LHAT_CHECK(!checks("import^ std.math\n"
                       "let^ v = std.math.lvec3(1, 2, 3)\n"
                       "let^ f = f^ -> number^ { return^ v.x }\n"),
               "capture");

    LHAT_TEST("a yield^ refuses a host value");
    LHAT_CHECK(!checks("import^ std.math\n"
                       "let^ gen = p^ { yield^ std.math.lvec3(1, 2, 3) }\n"),
               "yield value");

    LHAT_TEST("the program's answer refuses a host value");
    LHAT_CHECK(!checks("import^ std.math\n"
                       "return^ std.math.lvec3(1, 2, 3)\n"),
               "top-level return");

    LHAT_TEST("an interpolation hole refuses a whole host value");
    LHAT_CHECK(!checks("import^ std.math\n"
                       "let^ v = std.math.lvec3(1, 2, 3)\n"
                       "let^ s = $\"{v}\"\n"),
               "interpolation hole");

    LHAT_TEST("a registered member is not written over");
    LHAT_CHECK(!checks("import^ std.math\n"
                       "var^ v = std.math.lvec3(1, 2, 3)\n"
                       "v.dot := f^self^, o:std.math.LVector3 -> number^ {\n"
                       "    return^ 0\n"
                       "}\n"),
               "member overwrite");

    // The doors that stay open, pinned so a rule tightening by accident is
    // seen here: fields write, parameters and results pass.
    LHAT_TEST("what must keep checking still checks");
    LHAT_CHECK(checks("import^ std.math\n"
                      "var^ v = std.math.lvec3(1, 2, 3)\n"
                      "v.x := 9\n"
                      "let^ f = f^p:std.math.LVector3 -> std.math.LVector3 {\n"
                      "    return^ p + p\n"
                      "}\n"
                      "let^ w = f(v)\n"
                      "return^ w.x\n"),
               "the allowed shapes");
}

int main(void)
{
    test_fields();
    test_operators();
    test_equality();
    test_narrowing();
    test_subroutines();
    test_coroutine_locals();
    test_boxing();
    test_collection();
    test_escapes();
    return lhat_test_report("test_math");
}
