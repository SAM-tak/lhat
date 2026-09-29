// L^ (lhat) -- arithmetic and host-value integration for the new math types.
#include "stdlibutil.h"
#include "testutil.h"

#include "../stdlib/math.h"
#include "../stdlib/mathcomplex.h"
#include "../stdlib/mathvector2.h"
#include "../stdlib/mathquaternion.h"
#include "../stdlib/mathvector3.h"
#include "../stdlib/mathvector4.h"

static const LhatTestRegister regs[] = {
    lhatstdlib_math_register,
    lhatstdlib_mathvector2_register,
    lhatstdlib_mathcomplex_register,
    lhatstdlib_mathvector3_register,
    lhatstdlib_mathquaternion_register,
    lhatstdlib_mathvector4_register,
};

static LhatTestRan run_source(const char *source)
{
    return lhat_test_run(regs, sizeof regs / sizeof regs[0], source);
}

static void test_inferred_scalar_field_writes(void)
{
    LHAT_TEST("narrowed operator results lose speculative host-value widths");
    static const char *const expressions[] = {"a * b", "a + b", "a * 2"};
    static const int expected[] = {300, 103, 200};
    for (size_t i = 0; i < sizeof expressions / sizeof *expressions; i++) {
        char source[1024];
        snprintf(source, sizeof source,
            "import^ std.math\n"
            "let^ g = f^ a, b { return^ %s }\n"
            "let^ T = def^{ self^{ x = 100 },\n"
            " update = p^self^, dt:number^ { self^.x := g(self^.x, dt) }, }\n"
            "let^ t = T.new()\nt.update(3)\n"
            "if^ t.x = %d { return^ 1 }\nreturn^ 0\n",
            expressions[i], expected[i]);
        LhatTestRan ran = run_source(source);
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("inferred rubberstep result can be assigned to an instance field");
    LhatTestRan ran = run_source(
        "import^ std.math\n"
        "let^ rubberstep = f^current, target, halfLife, deltaTime {\n"
        " let^ delta = target - current\n"
        " if^ delta.abs() > 0 {\n"
        "  let^ omega = 1.0 / std.math.max(0.00000001, halfLife ** 2)\n"
        "  let^ a = 0.5 * omega * deltaTime ** 2\n"
        "  let^ a2 = a ** 2\n"
        "  return^ current + delta * (omega * deltaTime * (1.0 + a + 0.48 * a2 + 0.235 * a2 * a)).clamp(0, 1)\n"
        " }\nreturn^ target\n}\n"
        "let^ T = def^{ self^{ x = 100 },\n"
        " update = p^self^, dt:number^ { self^.x := rubberstep(self^.x, -100, 0.5, dt) }, }\n"
        "let^ t = T.new()\nt.update(0.5)\nt.update(0.5)\n"
        "if^ t.x = -100 { return^ 1 }\nreturn^ 0\n");
    LHAT_CHECK_RAN_INTEGER(ran, 1);
    lhat_test_ran_dispose(&ran);
}

static void test_vector_abs(void)
{
    LHAT_TEST("vector abs returns component-wise magnitudes without changing the source");
    LhatTestRan ran = run_source(
        "import^ std.math\n"
        "let^ a = std.math.vector2.new(-2, 3)\n"
        "let^ b = std.math.vector3.new(0, -4, -5)\n"
        "let^ c = std.math.vector4.new(-6, 7, -8, -9)\n"
        "let^ aa:std.math.vector2.Vector2 = a.abs()\n"
        "let^ bb:std.math.vector3.Vector3 = b.abs()\n"
        "let^ cc:std.math.vector4.Vector4 = c.abs()\n"
        "if^ aa.x = 2 and^ aa.y = 3\n"
        " and^ bb.x = 0 and^ bb.y = 4 and^ bb.z = 5\n"
        " and^ cc.x = 6 and^ cc.y = 7 and^ cc.z = 8 and^ cc.w = 9\n"
        " and^ a.x = -2 and^ b.y = -4 and^ b.z = -5\n"
        " and^ c.x = -6 and^ c.z = -8 and^ c.w = -9 { return^ 1 }\n"
        "return^ 0\n");
    LHAT_CHECK_RAN_INTEGER(ran, 1);
    lhat_test_ran_dispose(&ran);
}

static void test_vector_clamp(void)
{
    LHAT_TEST("vector clamp accepts scalar and component-wise bounds without mutation");
    LhatTestRan ran = run_source(
        "import^ std.math\n"
        "let^ a = std.math.vector2.new(-2, 3)\n"
        "let^ b = std.math.vector3.new(-2, 0.5, 3)\n"
        "let^ c = std.math.vector4.new(-2, 0, 1, 3)\n"
        "let^ aa:std.math.vector2.Vector2 = a.clamp(0, 1)\n"
        "let^ bb:std.math.vector3.Vector3 = b.clamp(0, 1)\n"
        "let^ cc:std.math.vector4.Vector4 = c.clamp(0, 1)\n"
        "let^ av = a.clamp(std.math.vector2.new(-1, 1), std.math.vector2.new(2, 2))\n"
        "let^ bv = b.clamp(std.math.vector3.new(-3, 1, -2), std.math.vector3.new(-1, 2, 2))\n"
        "let^ cv = c.clamp(std.math.vector4.new(-1, -2, 2, -4), std.math.vector4.new(0, -1, 2, 2))\n"
        "if^ aa.x = 0 and^ aa.y = 1\n"
        " and^ bb.x = 0 and^ bb.y = 0.5 and^ bb.z = 1\n"
        " and^ cc.x = 0 and^ cc.y = 0 and^ cc.z = 1 and^ cc.w = 1\n"
        " and^ av.x = -1 and^ av.y = 2\n"
        " and^ bv.x = -2 and^ bv.y = 1 and^ bv.z = 2\n"
        " and^ cv.x = -1 and^ cv.y = -1 and^ cv.z = 2 and^ cv.w = 2\n"
        " and^ a.x = -2 and^ a.y = 3\n"
        " and^ b.x = -2 and^ b.y = 0.5 and^ b.z = 3\n"
        " and^ c.x = -2 and^ c.y = 0 and^ c.z = 1 and^ c.w = 3 { return^ 1 }\n"
        "return^ 0\n");
    LHAT_CHECK_RAN_INTEGER(ran, 1);
    lhat_test_ran_dispose(&ran);

    LHAT_TEST("vector clamp rejects mixed scalar/vector bounds and different dimensions");
    LHAT_CHECK(!lhat_test_check_text(regs, sizeof regs / sizeof regs[0],
        "import^ std.math\n"
        "let^ v = std.math.vector2.zero().clamp(0, std.math.vector2.one())\n"),
        "both bounds must use the same overload");
    LHAT_CHECK(!lhat_test_check_text(regs, sizeof regs / sizeof regs[0],
        "import^ std.math\n"
        "let^ v = std.math.vector3.zero().clamp(std.math.vector4.zero(), std.math.vector4.one())\n"),
        "vector bounds must match the receiver's dimension");
}

static void test_vector4(void)
{
    LHAT_TEST("Vector4 zero and one functions return values");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector4\n"
            "var^ a = std.math.vector4.zero()\n"
            "a.w := 3\n"
            "if^ std.math.vector4.zero().w = 0.0\n"
            " and^ std.math.vector4.one().x = 1.0\n"
            " and^ std.math.vector4.one().y = 1.0\n"
            " and^ std.math.vector4.one().z = 1.0\n"
            " and^ std.math.vector4.one().w = 1.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("Vector4 division and lerp include w");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector4\n"
            "let^ a = std.math.vector4.new(8, -12, 20, 32)\n"
            "let^ b = std.math.vector4.new(2, -4, 5, 8)\n"
            "let^ by = a / b\n"
            "let^ scalar = a / 2\n"
            "let^ mid = a.lerp(b, 0.5)\n"
            "let^ beyond = a.lerp(b, 2)\n"
            "if^ by.x = 4.0 and^ by.y = 3.0 and^ by.z = 4.0 and^ by.w = 4.0\n"
            " and^ scalar.w = 16.0 and^ mid.x = 5.0 and^ mid.w = 20.0\n"
            " and^ beyond.x = -4.0 and^ beyond.w = -16.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("Vector4 Hadamard product and scalar scaling are distinct overloads");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector4\n"
            "let^ a = std.math.vector4.new(2, -3, 4, 5)\n"
            "let^ b = std.math.vector4.new(6, 7, -8, 0)\n"
            "let^ c = a * b\n"
            "let^ d = (2 * a) * b\n"
            "let^ e = a * 2\n"
            "if^ c.x = 12.0 and^ c.y = -21.0 and^ c.z = -32.0 and^ c.w = 0.0\n"
            " and^ d.x = 24.0 and^ d.y = -42.0 and^ d.z = -64.0\n"
            " and^ e.w = 10.0 and^ b * a = c { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("Vector4 fields, arithmetic and copy semantics");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector4\n"
            "var^ a = std.math.vector4.new(1, 2, 3, 4)\n"
            "let^ b = a\n"
            "a.w := 5\n"
            "let^ c = (a + b) * 2 - a\n"
            "let^ d = 2 * -b\n"
            "if^ c.x = 3.0 and^ c.y = 6.0 and^ c.z = 9.0 and^ c.w = 13.0\n"
            " and^ b.w = 4.0 and^ d.w = -8.0 and^ a != b { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("Vector4 dot, length, normalized and zero");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector4\n"
            "let^ a = std.math.vector4.new(1, 2, 2, 4)\n"
            "let^ b = std.math.vector4.new(0, 0, 0, 2).normalized()\n"
            "let^ z = std.math.vector4.new(0, 0, 0, 0).normalized()\n"
            "if^ a dot^ a = 25.0 and^ a.length() = 5.0 and^ b.w = 1.0\n"
            " and^ z = std.math.vector4.new(0, 0, 0, 0) { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("Vector4 tostring and interpolation");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector4\n"
            "let^ a = std.math.vector4.new(1, 2, 3, 4)\n"
            "return^ $\"{a}\"\n");
        LHAT_CHECK_RAN_TEXT(ran, "{x:1.0 y:2.0 z:3.0 w:4.0}");
        lhat_test_ran_dispose(&ran);
    }
}

static void test_complex(void)
{
    LHAT_TEST("Complex zero, one and i have their conventional values");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.complex\n"
            "let^ z = std.math.complex.zero()\n"
            "let^ one = std.math.complex.one()\n"
            "let^ i = std.math.complex.i()\n"
            "if^ z.re = 0.0 and^ z.im = 0.0\n"
            " and^ one.re = 1.0 and^ one.im = 0.0\n"
            " and^ i.re = 0.0 and^ i.im = 1.0\n"
            " and^ (i * i).re = -1.0 { return^ 1 }\nreturn^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("Complex multiplication, scaling, conjugate and magnitude");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.complex\n"
            "let^ a = std.math.complex.new(1, 2)\n"
            "let^ b = std.math.complex.new(3, 4)\n"
            "let^ p = a * b\n"
            "let^ q = b * a\n"
            "let^ s = 2 * a + a * 2 - a\n"
            "let^ n = -a\n"
            "if^ p.re = -5.0 and^ p.im = 10.0 and^ p = q\n"
            " and^ s.re = 3.0 and^ s.im = 6.0 and^ n.im = -2.0\n"
            " and^ a.conjugate().im = -2.0\n"
            " and^ std.math.complex.new(3, 4).length() = 5.0\n"
            " and^ std.math.complex.new(0, 2).normalized().im = 1.0\n"
            " and^ std.math.complex.new(0, 0).normalized().re = 0.0\n"
            " { return^ 1 }\nreturn^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("Complex field mutation and tostring");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.complex\n"
            "var^ a = std.math.complex.new(1, 2)\n"
            "let^ b = a\n"
            "a.im := 3\n"
            "if^ b.im = 2.0 and^ a.tostring() = \"{re:1.0 im:3.0}\" { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_quaternion(void)
{
    LHAT_TEST("Quaternion identity is a fresh unit value");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.quaternion\n"
            "var^ a = std.math.quaternion.identity()\n"
            "a.w := 2\n"
            "let^ b = std.math.quaternion.identity()\n"
            "let^ q = std.math.quaternion.new(1, 2, 3, 4)\n"
            "let^ c = q * b\n"
            "if^ b.x = 0.0 and^ b.y = 0.0 and^ b.z = 0.0\n"
            " and^ b.w = 1.0 and^ c.x = 1.0 and^ c.y = 2.0\n"
            " and^ c.z = 3.0 and^ c.w = 4.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("fromTo makes the shortest rotation between nonzero Vector3 directions");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "import^ std.math.quaternion\n"
            "let^ x = std.math.vector3.new(2, 0, 0)\n"
            "let^ y = std.math.vector3.new(0, 3, 0)\n"
            "let^ one = std.math.quaternion.new(0, 0, 0, 1)\n"
            "let^ q = std.math.quaternion.fromTo(x, y) ?? one\n"
            "let^ moved = q.rotate(x)\n"
            "let^ same = std.math.quaternion.fromTo(x, x) ?? one\n"
            "let^ opposite = std.math.quaternion.fromTo(x, -x) ?? one\n"
            "let^ reversed = opposite.rotate(x)\n"
            "let^ arbitrary = std.math.quaternion.fromTo(\n"
            "  std.math.vector3.new(1, 2, 3), std.math.vector3.new(-1, -2, -3)) ?? one\n"
            "let^ arbitrary_moved = arbitrary.rotate(std.math.vector3.new(1, 2, 3))\n"
            "if^ moved.x < 0.00001 and^ moved.x > -0.00001\n"
            " and^ moved.y > 1.99999 and^ moved.y < 2.00001\n"
            " and^ q.length() > 0.99999 and^ q.length() < 1.00001\n"
            " and^ same.w = 1.0 and^ reversed.x = -2.0\n"
            " and^ arbitrary_moved.x > -1.00001 and^ arbitrary_moved.x < -0.99999\n"
            " and^ arbitrary_moved.y > -2.00001 and^ arbitrary_moved.y < -1.99999\n"
            " and^ arbitrary_moved.z > -3.00001 and^ arbitrary_moved.z < -2.99999\n"
            " { return^ 1 }\nreturn^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("fromTo and fromAxisAngle reject a zero direction");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "import^ std.math.quaternion\n"
            "let^ z = std.math.vector3.new(0, 0, 0)\n"
            "let^ x = std.math.vector3.new(1, 0, 0)\n"
            "if^ std.math.quaternion.fromTo(z, x) = nil^\n"
            " and^ std.math.quaternion.fromTo(x, z) = nil^\n"
            " and^ std.math.quaternion.fromAxisAngle(z, 1) = nil^\n"
            " { return^ 1 }\nreturn^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("axis-angle uses right-handed radians and rotation handles scaling");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "import^ std.math.quaternion\n"
            "let^ one = std.math.quaternion.new(0, 0, 0, 1)\n"
            "let^ z = std.math.vector3.new(0, 0, 4)\n"
            "let^ x = std.math.vector3.new(1, 0, 0)\n"
            "let^ q = std.math.quaternion.fromAxisAngle(z, 1.5707963267948966) ?? one\n"
            "let^ moved = (3 * q).rotate(x)\n"
            "let^ unchanged = std.math.quaternion.new(0, 0, 0, 0).rotate(x)\n"
            "if^ moved.x < 0.00001 and^ moved.x > -0.00001\n"
            " and^ moved.y > 0.99999 and^ moved.y < 1.00001\n"
            " and^ moved.z = 0.0 and^ unchanged.x = 1.0\n"
            " and^ q.length() > 0.99999 and^ q.length() < 1.00001\n"
            " { return^ 1 }\nreturn^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("slerp takes the shortest arc and returns a unit quaternion");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.quaternion\n"
            "let^ one = std.math.quaternion.new(0, 0, 0, 1)\n"
            "let^ half = one.slerp(std.math.quaternion.new(0, 0, 1, 0), 0.5)\n"
            "let^ same = one.slerp(-one, 0.5)\n"
            "let^ parallel = one.slerp(one, 0.5)\n"
            "let^ scaled = (2 * one).slerp(std.math.quaternion.new(0, 0, 0, 3), 0.5)\n"
            "let^ zero = std.math.quaternion.new(0, 0, 0, 0)\n"
            "let^ fallback = zero.slerp(one, 0.5)\n"
            "if^ half.z > 0.707 and^ half.z < 0.708\n"
            " and^ half.w > 0.707 and^ half.w < 0.708\n"
            " and^ half.length() > 0.999 and^ half.length() < 1.001\n"
            " and^ same.w = 1.0 and^ parallel.w = 1.0\n"
            " and^ scaled.w = 1.0 and^ fallback.w = 0.5 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("Quaternion Hamilton product is noncommutative");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.quaternion\n"
            "let^ i = std.math.quaternion.new(1, 0, 0, 0)\n"
            "let^ j = std.math.quaternion.new(0, 1, 0, 0)\n"
            "let^ k = std.math.quaternion.new(0, 0, 1, 0)\n"
            "let^ one = std.math.quaternion.new(0, 0, 0, 1)\n"
            "let^ ij = i * j\n"
            "let^ ji = j * i\n"
            "let^ ii = i * i\n"
            "if^ ij.z = 1.0 and^ ij.w = 0.0 and^ ji.z = -1.0\n"
            " and^ ii.w = -1.0 and^ (i * one).x = 1.0\n"
            " and^ (one * i).x = 1.0 and^ k.z = 1.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("Quaternion scalar operations, conjugate and length");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.quaternion\n"
            "var^ a = std.math.quaternion.new(1, 2, 2, 4)\n"
            "let^ b = a\n"
            "a.w := 5\n"
            "let^ c = a.conjugate()\n"
            "let^ d = 2 * b + b * 2 - b\n"
            "let^ z = std.math.quaternion.new(0, 0, 0, 0).normalized()\n"
            "if^ b.w = 4.0 and^ c.x = -1.0 and^ c.y = -2.0\n"
            " and^ c.z = -2.0 and^ c.w = 5.0 and^ d.w = 12.0\n"
            " and^ b ⋅ b = 25.0 and^ b.length() = 5.0\n"
            " and^ std.math.quaternion.new(0, 0, 0, 2).normalized().w = 1.0\n"
            " and^ z.w = 0.0 { return^ 1 }\nreturn^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("Quaternion tostring exposes vector-first order");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.quaternion\n"
            "return^ std.math.quaternion.new(1, 2, 3, 4).tostring()\n");
        LHAT_CHECK_RAN_TEXT(ran, "{x:1.0 y:2.0 z:3.0 w:4.0}");
        lhat_test_ran_dispose(&ran);
    }
}

static void test_value_boundaries(void)
{
    LHAT_TEST("four-component values cross subroutines and boxes whole");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector4\n"
            "import^ std.math.quaternion\n"
            "let^ V = std.math.vector4.Vector4\n"
            "let^ add = f^a:V, b:V -> V { return^ a + b }\n"
            "let^ v = std.math.vector4.new(1, 2, 3, 4)\n"
            "let^ box = box^v\n"
            "box.set(add(box.get(), v))\n"
            "let^ q = std.math.quaternion.new(1, 2, 3, 4)\n"
            "if^ box.get().w = 8.0 and^ q fits^ std.math.quaternion.Quaternion\n"
            " and^ !(v fits^ std.math.quaternion.Quaternion) { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_operator_syntax(void)
{
    LHAT_TEST("cross and dot bind like multiplication, from the left");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "let^ x = std.math.vector3.new(1, 0, 0)\n"
            "let^ y = std.math.vector3.new(0, 1, 0)\n"
            "let^ z = std.math.vector3.new(0, 0, 1)\n"
            "if^ x cross^ y dot^ z = 1.0 and^ x×y⋅z = 1.0\n"
            " and^ 2 * x dot^ x + 3 = 5.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("numeric operands have no cross or dot operator");
    LHAT_CHECK(!lhat_test_check_text(regs, sizeof regs / sizeof regs[0],
                                    "let^ x = 1 dot^ 2\n"),
               "dot^ requires an operator definition");
    LHAT_CHECK(!lhat_test_check_text(regs, sizeof regs / sizeof regs[0],
                                    "let^ x = 1 × 2\n"),
               "× is not numeric multiplication");

    LHAT_TEST("a definition may provide the named product operators");
    {
        LhatTestRan ran = run_source(
            "let^ Pair = def^{\n"
            "  self^{ n = 2 },\n"
            "  op^dot^ := f^self^, other:Pair -> number^ { return^ self^.n + other.n },\n"
            "  op^cross^ := f^self^, other:Pair -> number^ { return^ self^.n * other.n },\n"
            "}\n"
            "let^ p = Pair.new()\n"
            "if^ p dot^ p = 4 and^ p ⋅ p = 4\n"
            " and^ p cross^ p = 4 and^ p × p = 4 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

int main(void)
{
    test_inferred_scalar_field_writes();
    test_vector_abs();
    test_vector_clamp();
    test_operator_syntax();
    test_vector4();
    test_complex();
    test_quaternion();
    test_value_boundaries();
    return lhat_test_report("test_mathvalues");
}
