// L^ (lhat) -- tests for std.math.vector3, which is the proving ground for 05 の
// 8.9's host values.
//
// What is pinned here is the machinery rather than the arithmetic: a host
// value crossing every boundary it is allowed to cross (an operator, a
// method, an L^ subroutine's parameters and result, a coroutine's own
// locals, an interpolation hole), refusing every one it is not (a table, a
// capture, a yield, the program's answer), equality being bytes rather
// than heads, and fields reading and writing the value in place. The
// numbers are chosen so a wrong slot somewhere answers a wrong number here.

#include <stdio.h>

#include "stdlibutil.h"
#include "testutil.h"

#include "../stdlib/mathvector3.h"

static const LhatTestRegister regs[] = {lhatstdlib_mathvector3_register};

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
            "import^ std.math.vector3\n"
            "let^ v = std.math.vector3.new(3, 4, 12)\n"
            "if^ v.x = 3.0 and^ v.y = 4.0 and^ v.z = 12.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("fields write the value in place, and only this copy");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "var^ a = std.math.vector3.new(1, 2, 3)\n"
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
            "import^ std.math.vector3\n"
            "let^ a = std.math.vector3.new(1, 2, 3)\n"
            "let^ b = std.math.vector3.new(10, 20, 30)\n"
            "let^ v = (a + b) * 2 - a\n"
            "if^ v.x = 21.0 and^ v.y = 42.0 and^ v.z = 63.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // 02 の 11.8改: the unary spelling of a name the type also carries as a
    // binary one. Told apart by the count, so both arms of '-' stand together
    // and each answers what it was written for.
    LHAT_TEST("a host value answers the unary '-'");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "let^ v = -std.math.vector3.new(1, 2, 3)\n"
            "if^ v.x = -1.0 and^ v.y = -2.0 and^ v.z = -3.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // 02 の 11.3改: the arm written with a trailing self^, so the receiver is
    // the operand on the right. Without it a number^ on the left carries no
    // answer for a host value and '2 * v' has nowhere to go.
    LHAT_TEST("and a scalar on the left scales it too");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "let^ v = std.math.vector3.new(1, 2, 3)\n"
            "let^ a = 2 * v\n"
            "let^ b = v * 2\n"
            "if^ a.x = b.x and^ a.y = b.y and^ a.z = b.z and^ a.z = 6.0 {\n"
            "  return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // The counts still do not stand in for each other. A scalar on the left
    // of '-' finds nothing: Vector3 carries the trailing self^ arm for '*'
    // and not for '-', so 14.8's number^ is what is left to fall back on.
    LHAT_TEST("and an arm that was not written is not invented");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "let^ v = std.math.vector3.new(1, 2, 3)\n"
                       "let^ w = 2 - v\n"
                       "return^ 1\n"),
               "no arm answers a scalar on the left of '-'");

    LHAT_TEST("methods answer numbers and values alike");
    {
        // (2,0,0): the one normalized answer float represents exactly.
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "let^ v = std.math.vector3.new(3, 4, 0)\n"
            "let^ u = std.math.vector3.new(2, 0, 0).normalized()\n"
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
            "import^ std.math.vector3\n"
            "let^ a = std.math.vector3.new(1, 2, 3)\n"
            "let^ b = std.math.vector3.new(1, 2, 3)\n"
            "let^ c = std.math.vector3.new(1, 2, 4)\n"
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
            "import^ std.math.vector3\n"
            "let^ v = std.math.vector3.new(1, 2, 3)\n"
            "if^ v isa^ std.math.vector3.Vector3 { return^ 1 }\n"
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
            "import^ std.math.vector3\n"
            "let^ blend = f^p:std.math.vector3.Vector3, q:std.math.vector3.Vector3"
            " -> std.math.vector3.Vector3 {\n"
            "    return^ (p + q) * 0.5\n"
            "}\n"
            "let^ mid = blend(std.math.vector3.new(2, 4, 6),"
            " std.math.vector3.new(4, 8, 10))\n"
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
            "import^ std.math.vector3\n"
            "var^ log = { n := 0 }\n"
            "let^ make = p^ -> std.math.vector3.Vector3 {\n"
            "    do^{\n"
            "        return^ std.math.vector3.new(5, 6, 7)\n"
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
            "import^ std.math.vector3\n"
            "let^ gen = p^ {\n"
            "    let^ held = std.math.vector3.new(4, 5, 6)\n"
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
}

// 05 の 8.9: the box the language hangs under every host value type --
// `T.Box^`, made with box^, read with get(), written with set().
static void test_boxing(void)
{
    LHAT_TEST("box^ boxes and get/set unbox and write");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "let^ a = box^std.math.vector3.new(1, 2, 3)\n"
            "let^ b = box^std.math.vector3.new(10, 20, 30)\n"
            "let^ c = box^std.math.vector3.new(0, 0, 0)\n"
            "c.set(a.get() + b.get())\n"
            "let^ landed = c.get()\n"
            "if^ landed.x = 11.0 and^ landed.y = 22.0 and^ landed.z = 33.0 {\n"
            "    return^ 1\n"
            "}\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a box is an ordinary heap value");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "let^ t = { held = box^std.math.vector3.new(1, 2, 3) }\n"
            "var^ maybe : std.math.vector3.Vector3.Box^|nil^ = nil^\n"
            "maybe := t.held\n"
            "let^ read = f^b:std.math.vector3.Vector3.Box^ -> number^ {\n"
            "    return^ b.get().y\n"
            "}\n"
            "if^ read(maybe ?? t.held) = 2.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // The box holds its own bytes: what was boxed and what is read back are
    // two copies, so neither write moves the other.
    LHAT_TEST("a box has value semantics at both ends");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "var^ v = std.math.vector3.new(1, 1, 1)\n"
            "let^ b = box^v\n"
            "v.x := 9\n"
            "var^ w = b.get()\n"
            "w.y := 9\n"
            "if^ b.get().x = 1.0 and^ b.get().y = 1.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // 8.9改: '=' is the bytes under the same tag, as it is on the stack;
    // is^ still asks for the very box.
    LHAT_TEST("two boxes of the same bytes are equal, not the same");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "let^ a = box^std.math.vector3.new(1, 2, 3)\n"
            "let^ b = box^std.math.vector3.new(1, 2, 3)\n"
            "let^ c = box^std.math.vector3.new(9, 9, 9)\n"
            "var^ r = 0\n"
            "if^ a = b { r := r + 1 }\n"
            "if^ a is^ b { r := r + 10 }\n"
            "if^ a = c { r := r + 100 }\n"
            "return^ r\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // 8.9改: constbox^ makes the sealed, get-only box -- off a value or as
    // a copy of a live box -- and only that one may be a key. Lookups key
    // by content, so a fresh copy finds the entry.
    LHAT_TEST("a constbox^ keys a table by its bytes");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "let^ live = box^std.math.vector3.new(4, 5, 6)\n"
            "let^ t = {\n"
            "    [constbox^std.math.vector3.new(1, 2, 3)] = 7,\n"
            "    [constbox^live] = 8,\n"
            "}\n"
            "var^ r = 0\n"
            "if^ t[constbox^std.math.vector3.new(1, 2, 3)] = 7 { r := r + 1 }\n"
            "if^ t[constbox^live] = 8 { r := r + 10 }\n"
            "return^ r\n");
        LHAT_CHECK_RAN_INTEGER(ran, 11);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a mutable box is refused where a key is stored");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "let^ b = box^std.math.vector3.new(1, 2, 3)\n"
                       "let^ t = { [b] = 1 }\n"),
               "a literal key has to be sealed");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "let^ b = box^std.math.vector3.new(1, 2, 3)\n"
                       "var^ t = { [constbox^b] = 1 }\n"
                       "t[b] := 2\n"),
               "a stored index key has to be sealed");

    // A lookup reads the bytes of the moment, and everything the table
    // holds is sealed -- so a live box asks fine, and tracks its set().
    LHAT_TEST("a lookup may ask with a live box");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "let^ b = box^std.math.vector3.new(1, 2, 3)\n"
            "let^ t = {\n"
            "    [constbox^b] = 7,\n"
            "    [constbox^std.math.vector3.new(9, 9, 9)] = 8,\n"
            "}\n"
            "var^ r = 0\n"
            "if^ t[b] = 7 { r := r + 1 }\n"
            "b.set(std.math.vector3.new(9, 9, 9))\n"
            "if^ t[b] = 8 { r := r + 10 }\n"
            "return^ r\n");
        LHAT_CHECK_RAN_INTEGER(ran, 11);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a bare host value is never stored as a key");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "let^ v = std.math.vector3.new(1, 2, 3)\n"
                       "let^ t = { [v] = 1 }\n"),
               "a literal key is one slot");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "var^ t = { [constbox^std.math.vector3.new(1, 2, 3)] = 1 }\n"
                       "let^ v = std.math.vector3.new(1, 2, 3)\n"
                       "t[v] := 2\n"),
               "a stored index key is one slot");

    // ...but a lookup may ask with one: the bytes of the moment against the
    // sealed keys the table holds -- b.get() is what the box compare reads
    // anyway.
    LHAT_TEST("a lookup may ask with the bare value");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "let^ t = {\n"
            "    [constbox^std.math.vector3.new(1, 2, 3)] = 7,\n"
            "}\n"
            "let^ b = box^std.math.vector3.new(1, 2, 3)\n"
            "var^ r = 0\n"
            "if^ t[std.math.vector3.new(1, 2, 3)] = 7 { r := r + 1 }\n"
            "if^ t[b.get()] = 7 { r := r + 10 }\n"
            "if^ t[std.math.vector3.new(9, 9, 9)] = nil^ { r := r + 100 }\n"
            "return^ r\n");
        LHAT_CHECK_RAN_INTEGER(ran, 111);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a ConstBox^ has no set");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "let^ b = constbox^std.math.vector3.new(1, 2, 3)\n"
                       "b.set(std.math.vector3.new(4, 5, 6))\n"),
               "the sealed box is get-only");

    // 8.9改: a Box^ fits a ConstBox^ seat -- the get-only view -- and never
    // the other way around, where set() would be a lie.
    LHAT_TEST("a Box^ fits a ConstBox^ seat and not the reverse");
    LHAT_CHECK(checks("import^ std.math.vector3\n"
                      "let^ read = f^b:std.math.vector3.Vector3.ConstBox^ -> number^ {\n"
                      "    return^ b.get().x\n"
                      "}\n"
                      "let^ live = box^std.math.vector3.new(1, 2, 3)\n"
                      "let^ x = read(live)\n"),
               "the view takes the live box");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "let^ write = p^b:std.math.vector3.Vector3.Box^ {\n"
                       "    b.set(std.math.vector3.new(0, 0, 0))\n"
                       "}\n"
                       "let^ sealed = constbox^std.math.vector3.new(1, 2, 3)\n"
                       "write(sealed)\n"),
               "the sealed box stays out of a set seat");

    // 8.9改: a field reads straight off the box's bytes; a write still goes
    // through set().
    LHAT_TEST("a field reads off the box and never writes");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "let^ b = constbox^std.math.vector3.new(1, 2, 3)\n"
            "if^ b.y = 2.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "let^ b = box^std.math.vector3.new(1, 2, 3)\n"
                       "b.y := 9\n"),
               "the field is not a place to write");

    LHAT_TEST("set takes exactly the value the box was made for");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "let^ b = box^std.math.vector3.new(1, 2, 3)\n"
                       "b.set(4)\n"),
               "a number is not the boxed type");

    LHAT_TEST("and box^ takes exactly a host value");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "let^ b = box^7\n"),
               "a number needs no box");

    // 8.9改: a box writes itself as its flavour over its bytes, so a
    // printed key reads as a value rather than as an opaque handle.
    LHAT_TEST("a box writes its flavour and its content");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "let^ b = constbox^std.math.vector3.new(1, 2, 3)\n"
            "return^ $\"{b}\"\n");
        LHAT_CHECK_RAN_TEXT(ran, "std.math.vector3.Vector3.ConstBox^(1.0, 2.0, 3.0)");
        lhat_test_ran_dispose(&ran);
    }

    // 8.9改: the variadic tail cannot say a host value's type, so a bare
    // one is boxed to ride it -- print(v) is the everyday arrival.
    LHAT_TEST("a variadic seat refuses a bare host value");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "let^ f = f^... -> nil^ { return^ nil^ }\n"
                       "let^ x = f(std.math.vector3.new(1, 2, 3))\n"),
               "box it to pass it");

    // 8.9改 with 03 の 3.1③: a table of computed keys cannot say its walk's
    // K and V (the dictionary type is still an open design), so strict asks
    // the focus for annotations -- which is what keeps the host value
    // k.get() answers visible to every later rule. The width guard at the
    // placements stays as the unchecked run's backstop.
    LHAT_TEST("an undescribed walk cannot hide a wide answer");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "let^ t = { [constbox^std.math.vector3.new(1, 2, 3)] = \"a\" }\n"
                       "for^ k, v in^ t { let^ g = k.get() }\n"),
               "the focus asks for annotations first");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "let^ t = { [constbox^std.math.vector3.new(1, 2, 3)] = \"a\" }\n"
                       "for^ k:std.math.vector3.Vector3.ConstBox^, v:string^ in^ t {\n"
                       "    print(k.get(), v)\n"
                       "}\n"),
               "and the annotated focus meets the variadic rule");

    // typeof^ answers the box's own name; isa^ tells box and value apart.
    LHAT_TEST("the box's type is its own");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "let^ b = box^std.math.vector3.new(1, 2, 3)\n"
            "return^ typeof^(b).signature\n");
        LHAT_CHECK_RAN_TEXT(ran, "std.math.vector3.Vector3.Box^");
        lhat_test_ran_dispose(&ran);
    }

    // 02 の 14.11: a box is a copyable node, so it may sit on a prototype --
    // baked sealed, handed to each instance as an unsealed copy of its own.
    LHAT_TEST("a box may be a field's default");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "let^ D = def^{ self^{ pos = box^std.math.vector3.new(1, 2, 3) } }\n"
            "let^ a = D.new()\n"
            "let^ b = D.new()\n"
            "a.pos.set(std.math.vector3.new(9, 9, 9))\n"
            "if^ b.pos.get().x = 1.0 and^ a.pos.get().x = 9.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("and the prototype's own box takes no set");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "let^ D = def^{ self^{ pos = box^std.math.vector3.new(1, 2, 3) } }\n"
            "D.self^.pos.set(std.math.vector3.new(0, 0, 0))\n"
            "return^ 0\n");
        LHAT_CHECK(ran.ok, "the program ran");
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_SEALED);
        lhat_test_ran_dispose(&ran);
    }
}

// 02 の 14.17 with 05 の 8.9: a host value is written down like any other
// value. The library registers the spelling under the bare name -- the hat
// is 14.17改's, and what it keeps apart is a plain table's names, which a
// registered type has none of.
static void test_tostring(void)
{
    LHAT_TEST("a host value spells itself");
    {
        LhatTestRan ran = run_source("import^ std.math.vector3\n"
                                     "return^ std.math.vector3.new(3, 4, 0).tostring()\n");
        LHAT_CHECK_RAN_TEXT(ran, "{x:3.0 y:4.0 z:0.0}");
        lhat_test_ran_dispose(&ran);
    }

    // 01 の 5.4: the hole asks for tostring^, and on a host value the two
    // spellings are one member -- so what the registration wrote is what
    // lands here. 05 の 8.9: the receiver is three slots wide, and the key
    // sits past them rather than in the middle of the value.
    LHAT_TEST("and an interpolation hole writes that same spelling");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "let^ v = std.math.vector3.new(1, 2, 3)\n"
            "return^ $\"v = {v}, -v = {-v}\"\n");
        LHAT_CHECK_RAN_TEXT(ran,
                            "v = {x:1.0 y:2.0 z:3.0}, -v = {x:-1.0 y:-2.0 z:-3.0}");
        lhat_test_ran_dispose(&ran);
    }

    // 14.8: the components are numbers, so they are spelled the way a
    // number^ is spelled anywhere else -- 3 and 3.0 apart, and no printf
    // format of this library's own.
    LHAT_TEST("the components read as L^ spells a number^");
    {
        LhatTestRan ran = run_source("import^ std.math.vector3\n"
                                     "return^ std.math.vector3.new(0.5, -2, 1e10).tostring()\n");
        LHAT_CHECK_RAN_TEXT(ran, "{x:0.5 y:-2.0 z:1e+10}");
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
            "import^ std.math.vector3\n"
            "let^ v = std.math.vector3.new(9, 8, 7)\n"
            "var^ n = 0\n"
            "for^ i from^ 1 to^ 64 {\n"
            "    let^ w = v + std.math.vector3.new(i, 0, 0)\n"
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
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "let^ t = { v := std.math.vector3.new(1, 2, 3) }\n"),
               "table literal");

    LHAT_TEST("a written table type refuses one too");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "let^ f = f^t:t^{ v : std.math.vector3.Vector3 } -> number^ {\n"
                       "    return^ 1\n"
                       "}\n"),
               "written member type");

    LHAT_TEST("a capture refuses a host value");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "let^ v = std.math.vector3.new(1, 2, 3)\n"
                       "let^ f = f^ -> number^ { return^ v.x }\n"),
               "capture");

    // 8.9改: a yield^ carries a host value whole -- one seat, full width.
    // Only the mixed forms stay refused: a run's positions are single
    // slots, so a host value never rides among them.
    LHAT_TEST("a yield^ carries a host value whole");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "let^ gen = f^ -> c^{f^ -> std.math.vector3.Vector3} {\n"
            "    yield^ std.math.vector3.new(1, 2, 3)\n"
            "    yield^ std.math.vector3.new(4, 5, 6)\n"
            "}\n"
            "var^ total = 0\n"
            "for^ v in^ gen() { total := total + v.x }\n"
            "if^ total = 5.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("and a resume sends one whole");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "let^ gen = f^ -> c^{f^std.math.vector3.Vector3 -> number^} {\n"
            "    var^ got : std.math.vector3.Vector3 = yield^ 0\n"
            "    got := yield^ got.x\n"
            "    yield^ got.y\n"
            "}\n"
            "let^ c = gen()\n"
            "c.start()\n"
            "let^ a = c.resume(std.math.vector3.new(7, 8, 9))\n"
            "let^ b = c.resume(std.math.vector3.new(1, 2, 3))\n"
            "if^ (a ?? 0) + (b ?? 0) = 9.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // Y and T are the one type here -- 8.9 still keeps a host value out of
    // a union, so a mixed union(Y, T) cannot carry one; same-typed they
    // fold to the value itself.
    LHAT_TEST("a coroutine may return one, and a wide parameter crosses");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "let^ gen = f^p:std.math.vector3.Vector3 -> "
            "c^{f^ -> std.math.vector3.Vector3 -> std.math.vector3.Vector3} {\n"
            "    yield^ p\n"
            "    return^ p + p\n"
            "}\n"
            "let^ c = gen(std.math.vector3.new(2, 3, 4))\n"
            "let^ first = c.start()\n"
            "let^ last = c.resume()\n"
            "if^ first.x = 2.0 and^ last.y = 6.0 { return^ 1 }\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a host value still stays out of a yielded run");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "let^ gen = p^ {\n"
                       "    yield^ 1, std.math.vector3.new(1, 2, 3)\n"
                       "}\n"),
               "a run position is one slot");

    LHAT_TEST("the program's answer refuses a host value");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "return^ std.math.vector3.new(1, 2, 3)\n"),
               "top-level return");

    LHAT_TEST("a registered member is not written over");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "var^ v = std.math.vector3.new(1, 2, 3)\n"
                       "v.dot := f^self^, o:std.math.vector3.Vector3 -> number^ {\n"
                       "    return^ 0\n"
                       "}\n"),
               "member overwrite");

    // 8.9改: every rule above is about the width a place has room for, not
    // about the kind's identity -- so a union carrying a host value is as
    // wide as the value in it and lands in none of those places either.
    // These are the same refusals written with a '|nil^' on, and they are
    // what says the lift below opened no hole.
    LHAT_TEST("and a union carrying one is refused in the same places");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "let^ v = std.math.vector3.new(1, 2, 3)\n"
                       "let^ t = { held = if^ true^: v el^: nil^ ; }\n"),
               "a table member");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "let^ f = f^t:t^{ v : std.math.vector3.Vector3|nil^ } {\n"
                       "    return^ nil^\n"
                       "}\n"),
               "a written member type");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "var^ m : std.math.vector3.Vector3|nil^ = nil^\n"
                       "let^ f = f^ -> nil^ { let^ held = m return^ nil^ }\n"),
               "a capture");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "var^ m : std.math.vector3.Vector3|nil^ = nil^\n"
                       "print(m)\n"),
               "the variadic seat -- what '?.' used to slip through");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "var^ m : std.math.vector3.Vector3|nil^ = nil^\n"
                       "return^ m\n"),
               "the program's answer");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "let^ gen = p^ {\n"
                       "    var^ m : std.math.vector3.Vector3|nil^ = nil^\n"
                       "    yield^ 1, m\n"
                       "}\n"),
               "a run's position");

    // 8.9改: what the lift opens. A host value stands in a union whose other
    // arms the head slot's tag tells apart, so the room one reservation
    // makes serves both -- the value writes its width, the nil^ writes the
    // head and leaves the rest untouched.
    LHAT_TEST("a host value stands beside a nil^");
    LHAT_CHECK(checks("import^ std.math.vector3\n"
                      "let^ f = f^ -> std.math.vector3.Vector3|nil^ { return^ nil^ }\n"
                      "var^ m = f()\n"
                      "if^ m? { let^ x = m.x }\n"
                      "let^ y = (m ?? std.math.vector3.new(0, 0, 0)).x\n"
                      "let^ known = m isa^ std.math.vector3.Vector3\n"),
               "written, narrowed, defaulted and asked about");

    LHAT_TEST("and beside an error");
    LHAT_CHECK(checks("import^ std.math.vector3\n"
                      "errordef^ E { Bad }\n"
                      "let^ f = f^ -> std.math.vector3.Vector3|E.Bad {\n"
                      "    return^ error^E.Bad{}\n"
                      "}\n"
                      "let^ v = try^ f()\n"
                      "return^ v.x\n"),
               "try^ tells the arms apart");

    // The arms it may not stand beside: any^ has no head to read, and a
    // second wide arm would want a second reading of the one head.
    LHAT_TEST("but not beside an any^ or a second wide arm");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "let^ f = f^ -> std.math.vector3.Vector3|any^ { return^ nil^ }\n"),
               "any^ would be the escape written out");
    LHAT_CHECK(!checks("import^ std.math.vector3\n"
                       "let^ f = f^ -> std.math.vector3.Vector3|string^ {\n"
                       "    return^ \"a\"\n"
                       "}\n"),
               "no construct tells these apart");

    // The doors that stay open, pinned so a rule tightening by accident is
    // seen here: fields write, parameters and results pass.
    LHAT_TEST("what must keep checking still checks");
    LHAT_CHECK(checks("import^ std.math.vector3\n"
                      "var^ v = std.math.vector3.new(1, 2, 3)\n"
                      "v.x := 9\n"
                      "let^ f = f^p:std.math.vector3.Vector3 -> std.math.vector3.Vector3 {\n"
                      "    return^ p + p\n"
                      "}\n"
                      "let^ w = f(v)\n"
                      "return^ w.x\n"),
               "the allowed shapes");
}


// ---------------------------------------------------------------------------
// 05 の 8.9: a parameter's width comes from the type, not from the spelling
// ---------------------------------------------------------------------------
//
// A host value parameter reserves its registered width of consecutive slots,
// and the caller lays the argument down the same way -- so the two have to
// agree about how wide it is. The compiler used to read that off the written
// annotation, matching the registry by the words used. Two ways of writing
// the same type were then not the same width:
//
//   let^ vector3 = import^ std.math.vector3
//   let^ Vector3 = vector3.Vector3
//   f^p:Vector3, q:Vector3 -> Vector3 { … }     -- one slot each, and wrong
//
// and a parameter with no annotation at all had no spelling to read, however
// well the checker had inferred it. Both come from the signature now, which
// is what the checker settled and what 3401's comment already said the
// compiler should be reading.
static void test_parameter_width(void)
{
    // The three spellings of one type, over the same body. What comes back
    // is the same because the type is the same; the words are not.
    static const char *const spellings[] = {
        "Vector3",                    // a name bound to the type
        "vector3.Vector3",            // through a name bound to the module
        "std.math.vector3.Vector3",   // written out in full
    };

    LHAT_TEST("a parameter is as wide as its type, however the type is spelt");
    for (size_t i = 0; i < sizeof spellings / sizeof spellings[0]; i++) {
        char source[512];
        snprintf(source, sizeof source,
                 "import^ std.math.vector3\n"
                 "let^ vector3 = std.math.vector3\n"
                 "let^ Vector3 = vector3.Vector3\n"
                 "let^ blend = f^p:%s, q:%s -> %s { (p + q) * 0.5 }\n"
                 "let^ v = vector3.new(3, 4, 0)\n"
                 "let^ mid = blend(v, v * 2)\n"
                 "if^ mid.x = 4.5 and^ mid.y = 6.0 and^ mid.z = 0.0 "
                 "{ return^ 1 }\n"
                 "return^ 0\n",
                 spellings[i], spellings[i], spellings[i]);
        LhatTestRan ran = run_source(source);
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // 03 の 3.4: a parameter left to inference has no annotation to read at
    // all. Getting this wrong answered a wrong number rather than faulting,
    // which is the worse of the two ways to be wrong: `q` took one slot,
    // contributed nothing, and `(p + q) * 0.5` came back as `p * 0.5`.
    LHAT_TEST("and so is one whose type was inferred rather than written");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "let^ blend = f^p:std.math.vector3.Vector3, q { (p + q) * 0.5 }\n"
            "let^ v = std.math.vector3.new(3, 4, 0)\n"
            "let^ mid = blend(v, v * 2)\n"
            "if^ mid.x = 4.5 and^ mid.y = 6.0 and^ mid.z = 0.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // 14.4: a receiver is written among the parameters but is not in the
    // type's list of them, so the walk over the signature has to step over
    // it or every parameter after one would take the wrong width.
    LHAT_TEST("a receiver does not put the walk over the signature out of step");
    {
        LhatTestRan ran = run_source(
            "import^ std.math.vector3\n"
            "let^ Holder = def^{\n"
            "  self^{ },\n"
            "  mix := f^self^, a:std.math.vector3.Vector3,"
            " b:std.math.vector3.Vector3 -> number^ {\n"
            "    return^ (a + b).x\n"
            "  },\n"
            "}\n"
            "let^ h = Holder.new()\n"
            "let^ v = std.math.vector3.new(3, 4, 0)\n"
            "if^ h.mix(v, v * 2) = 9.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

// ---------------------------------------------------------------------------
// 13.11 with 11.6改: a written type is what it resolves to, not what it says
// ---------------------------------------------------------------------------
//
// isa^ and as^ take a type the compiler settles, and it settled one by
// matching the words against what the host registered. A name bound to the
// type, or to the module it lives in, is spelt nothing like those words, so
// the compiler reached no type and answered "no such name" -- while the
// checker, which resolves names properly, had passed the very same line.
//
// What the checker resolved is left on the node now, and the compiler reads
// it where the words run out. Every spelling of one type is that one type.
static const char alias_preamble[] =
    "import^ std.math.vector3\n"
    "let^ vector3 = std.math.vector3\n"
    "let^ Vector3 = vector3.Vector3\n"
    "let^ v = vector3.new(3, 4, 0)\n"
    "let^ n = 7\n";

static const char *const alias_spellings[] = {
    "Vector3",                    // a name bound to the type
    "vector3.Vector3",            // through a name bound to the module
    "std.math.vector3.Vector3",   // written out in full
};

#define ALIAS_SPELLING_COUNT \
    (sizeof alias_spellings / sizeof alias_spellings[0])

static void test_type_position_alias(void)
{
    LHAT_TEST("isa^ answers the same for every spelling of one type");
    for (size_t i = 0; i < ALIAS_SPELLING_COUNT; i++) {
        char source[512];
        snprintf(source, sizeof source,
                 "%sif^ v isa^ %s { return^ 1 }\nreturn^ 0\n",
                 alias_preamble, alias_spellings[i]);
        LhatTestRan ran = run_source(source);
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // And answers false where it should. Reading the checker's type wrongly
    // -- as an any^, say -- would make every one of these true, which is the
    // way this could be broken and still look mended.
    LHAT_TEST("and answers false for every spelling, where it should");
    for (size_t i = 0; i < ALIAS_SPELLING_COUNT; i++) {
        char source[512];
        snprintf(source, sizeof source,
                 "%sif^ n isa^ %s { return^ 1 }\nreturn^ 0\n",
                 alias_preamble, alias_spellings[i]);
        LhatTestRan ran = run_source(source);
        LHAT_CHECK_RAN_INTEGER(ran, 0);
        lhat_test_ran_dispose(&ran);
    }

    // 11.6改3: as^ answers the value or a localerror^.CastFailure, and
    // lower_type hands LHAT_BC_ASCAST the same descriptor isa^ tests
    // against -- so it was refused in the same three ways and is mended in
    // the same one.
    //
    // Only the arm that holds is asked here. A cast that could never succeed
    // is a type error before anything runs ("nothing is both of these"), so
    // the other direction is not this test's to make -- what says the
    // descriptor is a real question rather than an empty one is the false
    // case above.
    LHAT_TEST("as^ takes every spelling too");
    for (size_t i = 0; i < ALIAS_SPELLING_COUNT; i++) {
        char source[512];
        snprintf(source, sizeof source,
                 "%sif^ ((v as^ %s) isa^ localerror^.CastFailure) "
                 "{ return^ 0 }\nreturn^ 1\n",
                 alias_preamble, alias_spellings[i]);
        LhatTestRan ran = run_source(source);
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

int main(void)
{
    test_fields();
    test_operators();
    test_equality();
    test_narrowing();
    test_subroutines();
    test_parameter_width();
    test_type_position_alias();
    test_coroutine_locals();
    test_boxing();
    test_tostring();
    test_collection();
    test_escapes();
    return lhat_test_report("test_mathvector3");
}
