// L^ (lhat) -- tests for std.math, which is the proving ground for 05 の
// 8.9's host values.
//
// What is pinned here is the machinery rather than the arithmetic: a host
// value crossing every boundary it is allowed to cross (an operator, a
// method, an L^ subroutine's parameters and result, a coroutine's own
// locals, an interpolation hole), refusing every one it is not (a table, a
// capture, a yield, the program's answer), equality being bytes rather
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
            "let^ v = std.math.vec3(3, 4, 12)\n"
            "if^ v.x = 3.0 and^ v.y = 4.0 and^ v.z = 12.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("fields write the value in place, and only this copy");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "var^ a = std.math.vec3(1, 2, 3)\n"
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
            "let^ a = std.math.vec3(1, 2, 3)\n"
            "let^ b = std.math.vec3(10, 20, 30)\n"
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
            "import^ std.math\n"
            "let^ v = -std.math.vec3(1, 2, 3)\n"
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
            "import^ std.math\n"
            "let^ v = std.math.vec3(1, 2, 3)\n"
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
    LHAT_CHECK(!checks("import^ std.math\n"
                       "let^ v = std.math.vec3(1, 2, 3)\n"
                       "let^ w = 2 - v\n"
                       "return^ 1\n"),
               "no arm answers a scalar on the left of '-'");

    LHAT_TEST("methods answer numbers and values alike");
    {
        // (2,0,0): the one normalized answer float represents exactly.
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ v = std.math.vec3(3, 4, 0)\n"
            "let^ u = std.math.vec3(2, 0, 0).normalized()\n"
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
            "let^ a = std.math.vec3(1, 2, 3)\n"
            "let^ b = std.math.vec3(1, 2, 3)\n"
            "let^ c = std.math.vec3(1, 2, 4)\n"
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
            "let^ v = std.math.vec3(1, 2, 3)\n"
            "if^ v isa^ std.math.Vector3 { return^ 1 }\n"
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
            "let^ blend = f^p:std.math.Vector3, q:std.math.Vector3"
            " -> std.math.Vector3 {\n"
            "    return^ (p + q) * 0.5\n"
            "}\n"
            "let^ mid = blend(std.math.vec3(2, 4, 6),"
            " std.math.vec3(4, 8, 10))\n"
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
            "let^ make = p^ -> std.math.Vector3 {\n"
            "    do^{\n"
            "        return^ std.math.vec3(5, 6, 7)\n"
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
            "    let^ held = std.math.vec3(4, 5, 6)\n"
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
            "import^ std.math\n"
            "let^ a = box^std.math.vec3(1, 2, 3)\n"
            "let^ b = box^std.math.vec3(10, 20, 30)\n"
            "let^ c = box^std.math.vec3(0, 0, 0)\n"
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
            "import^ std.math\n"
            "let^ t = { held = box^std.math.vec3(1, 2, 3) }\n"
            "var^ maybe : std.math.Vector3.Box^|nil^ = nil^\n"
            "maybe := t.held\n"
            "let^ read = f^b:std.math.Vector3.Box^ -> number^ {\n"
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
            "import^ std.math\n"
            "var^ v = std.math.vec3(1, 1, 1)\n"
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
            "import^ std.math\n"
            "let^ a = box^std.math.vec3(1, 2, 3)\n"
            "let^ b = box^std.math.vec3(1, 2, 3)\n"
            "let^ c = box^std.math.vec3(9, 9, 9)\n"
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
            "import^ std.math\n"
            "let^ live = box^std.math.vec3(4, 5, 6)\n"
            "let^ t = {\n"
            "    [constbox^std.math.vec3(1, 2, 3)] = 7,\n"
            "    [constbox^live] = 8,\n"
            "}\n"
            "var^ r = 0\n"
            "if^ t[constbox^std.math.vec3(1, 2, 3)] = 7 { r := r + 1 }\n"
            "if^ t[constbox^live] = 8 { r := r + 10 }\n"
            "return^ r\n");
        LHAT_CHECK_RAN_INTEGER(ran, 11);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a mutable box is refused where a key is stored");
    LHAT_CHECK(!checks("import^ std.math\n"
                       "let^ b = box^std.math.vec3(1, 2, 3)\n"
                       "let^ t = { [b] = 1 }\n"),
               "a literal key has to be sealed");
    LHAT_CHECK(!checks("import^ std.math\n"
                       "let^ b = box^std.math.vec3(1, 2, 3)\n"
                       "var^ t = { [constbox^b] = 1 }\n"
                       "t[b] := 2\n"),
               "a stored index key has to be sealed");

    // A lookup reads the bytes of the moment, and everything the table
    // holds is sealed -- so a live box asks fine, and tracks its set().
    LHAT_TEST("a lookup may ask with a live box");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ b = box^std.math.vec3(1, 2, 3)\n"
            "let^ t = {\n"
            "    [constbox^b] = 7,\n"
            "    [constbox^std.math.vec3(9, 9, 9)] = 8,\n"
            "}\n"
            "var^ r = 0\n"
            "if^ t[b] = 7 { r := r + 1 }\n"
            "b.set(std.math.vec3(9, 9, 9))\n"
            "if^ t[b] = 8 { r := r + 10 }\n"
            "return^ r\n");
        LHAT_CHECK_RAN_INTEGER(ran, 11);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a bare host value is never stored as a key");
    LHAT_CHECK(!checks("import^ std.math\n"
                       "let^ v = std.math.vec3(1, 2, 3)\n"
                       "let^ t = { [v] = 1 }\n"),
               "a literal key is one slot");
    LHAT_CHECK(!checks("import^ std.math\n"
                       "var^ t = { [constbox^std.math.vec3(1, 2, 3)] = 1 }\n"
                       "let^ v = std.math.vec3(1, 2, 3)\n"
                       "t[v] := 2\n"),
               "a stored index key is one slot");

    // ...but a lookup may ask with one: the bytes of the moment against the
    // sealed keys the table holds -- b.get() is what the box compare reads
    // anyway.
    LHAT_TEST("a lookup may ask with the bare value");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ t = {\n"
            "    [constbox^std.math.vec3(1, 2, 3)] = 7,\n"
            "}\n"
            "let^ b = box^std.math.vec3(1, 2, 3)\n"
            "var^ r = 0\n"
            "if^ t[std.math.vec3(1, 2, 3)] = 7 { r := r + 1 }\n"
            "if^ t[b.get()] = 7 { r := r + 10 }\n"
            "if^ t[std.math.vec3(9, 9, 9)] = nil^ { r := r + 100 }\n"
            "return^ r\n");
        LHAT_CHECK_RAN_INTEGER(ran, 111);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a ConstBox^ has no set");
    LHAT_CHECK(!checks("import^ std.math\n"
                       "let^ b = constbox^std.math.vec3(1, 2, 3)\n"
                       "b.set(std.math.vec3(4, 5, 6))\n"),
               "the sealed box is get-only");

    // 8.9改: a Box^ fits a ConstBox^ seat -- the get-only view -- and never
    // the other way around, where set() would be a lie.
    LHAT_TEST("a Box^ fits a ConstBox^ seat and not the reverse");
    LHAT_CHECK(checks("import^ std.math\n"
                      "let^ read = f^b:std.math.Vector3.ConstBox^ -> number^ {\n"
                      "    return^ b.get().x\n"
                      "}\n"
                      "let^ live = box^std.math.vec3(1, 2, 3)\n"
                      "let^ x = read(live)\n"),
               "the view takes the live box");
    LHAT_CHECK(!checks("import^ std.math\n"
                       "let^ write = p^b:std.math.Vector3.Box^ {\n"
                       "    b.set(std.math.vec3(0, 0, 0))\n"
                       "}\n"
                       "let^ sealed = constbox^std.math.vec3(1, 2, 3)\n"
                       "write(sealed)\n"),
               "the sealed box stays out of a set seat");

    // 8.9改: a field reads straight off the box's bytes; a write still goes
    // through set().
    LHAT_TEST("a field reads off the box and never writes");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ b = constbox^std.math.vec3(1, 2, 3)\n"
            "if^ b.y = 2.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
    LHAT_CHECK(!checks("import^ std.math\n"
                       "let^ b = box^std.math.vec3(1, 2, 3)\n"
                       "b.y := 9\n"),
               "the field is not a place to write");

    LHAT_TEST("set takes exactly the value the box was made for");
    LHAT_CHECK(!checks("import^ std.math\n"
                       "let^ b = box^std.math.vec3(1, 2, 3)\n"
                       "b.set(4)\n"),
               "a number is not the boxed type");

    LHAT_TEST("and box^ takes exactly a host value");
    LHAT_CHECK(!checks("import^ std.math\n"
                       "let^ b = box^7\n"),
               "a number needs no box");

    // 8.9改: a box writes itself as its flavour over its bytes, so a
    // printed key reads as a value rather than as an opaque handle.
    LHAT_TEST("a box writes its flavour and its content");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ b = constbox^std.math.vec3(1, 2, 3)\n"
            "return^ $\"{b}\"\n");
        LHAT_CHECK_RAN_TEXT(ran, "std.math.Vector3.ConstBox^(1.0, 2.0, 3.0)");
        lhat_test_ran_dispose(&ran);
    }

    // 8.9改: the variadic tail cannot say a host value's type, so a bare
    // one is boxed to ride it -- print(v) is the everyday arrival.
    LHAT_TEST("a variadic seat refuses a bare host value");
    LHAT_CHECK(!checks("import^ std.math\n"
                       "let^ f = f^... -> nil^ { return^ nil^ }\n"
                       "let^ x = f(std.math.vec3(1, 2, 3))\n"),
               "box it to pass it");

    // 8.9改: a table of computed keys types its walk focus loosely (the
    // dictionary type is still an open design), so the checker cannot see
    // the host value k.get() answers -- the placement refuses the width at
    // run time instead of overwriting the neighbouring slots.
    LHAT_TEST("an untyped seat refuses a wide answer at run time");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ t = { [constbox^std.math.vec3(1, 2, 3)] = \"a\" }\n"
            "var^ r = 0\n"
            "for^ k, v in^ t { let^ g = k.get() r := r + 1 }\n"
            "return^ r\n");
        LHAT_CHECK((ran).ok, "the program ran");
        LHAT_CHECK_EQ_INT((ran).status, LHAT_RUN_TYPE_ERROR);
        lhat_test_ran_dispose(&ran);
    }

    // typeof^ answers the box's own name; isa^ tells box and value apart.
    LHAT_TEST("the box's type is its own");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ b = box^std.math.vec3(1, 2, 3)\n"
            "return^ typeof^(b).signature\n");
        LHAT_CHECK_RAN_TEXT(ran, "std.math.Vector3.Box^");
        lhat_test_ran_dispose(&ran);
    }

    // 02 の 14.11: a box is a copyable node, so it may sit on a prototype --
    // baked sealed, handed to each instance as an unsealed copy of its own.
    LHAT_TEST("a box may be a field's default");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ D = def^{ self^{ pos = box^std.math.vec3(1, 2, 3) } }\n"
            "let^ a = D.new()\n"
            "let^ b = D.new()\n"
            "a.pos.set(std.math.vec3(9, 9, 9))\n"
            "if^ b.pos.get().x = 1.0 and^ a.pos.get().x = 9.0 { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("and the prototype's own box takes no set");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ D = def^{ self^{ pos = box^std.math.vec3(1, 2, 3) } }\n"
            "D.self^.pos.set(std.math.vec3(0, 0, 0))\n"
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
        LhatTestRan ran = run_source("import^ std.math\n"
                                     "return^ std.math.vec3(3, 4, 0).tostring()\n");
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
            "import^ std.math\n"
            "let^ v = std.math.vec3(1, 2, 3)\n"
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
        LhatTestRan ran = run_source("import^ std.math\n"
                                     "return^ std.math.vec3(0.5, -2, 1e10).tostring()\n");
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
            "import^ std.math\n"
            "let^ v = std.math.vec3(9, 8, 7)\n"
            "var^ n = 0\n"
            "for^ i from^ 1 to^ 64 {\n"
            "    let^ w = v + std.math.vec3(i, 0, 0)\n"
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
                       "let^ t = { v := std.math.vec3(1, 2, 3) }\n"),
               "table literal");

    LHAT_TEST("a written table type refuses one too");
    LHAT_CHECK(!checks("import^ std.math\n"
                       "let^ f = f^t:t^{ v : std.math.Vector3 } -> number^ {\n"
                       "    return^ 1\n"
                       "}\n"),
               "written member type");

    LHAT_TEST("a capture refuses a host value");
    LHAT_CHECK(!checks("import^ std.math\n"
                       "let^ v = std.math.vec3(1, 2, 3)\n"
                       "let^ f = f^ -> number^ { return^ v.x }\n"),
               "capture");

    // 8.9改: a yield^ carries a host value whole -- one seat, full width.
    // Only the mixed forms stay refused: a run's positions are single
    // slots, so a host value never rides among them.
    LHAT_TEST("a yield^ carries a host value whole");
    {
        LhatTestRan ran = run_source(
            "import^ std.math\n"
            "let^ gen = f^ -> c^{f^ -> std.math.Vector3;, } {\n"
            "    yield^ std.math.vec3(1, 2, 3)\n"
            "    yield^ std.math.vec3(4, 5, 6)\n"
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
            "import^ std.math\n"
            "let^ gen = f^ -> c^{f^std.math.Vector3 -> number^;, } {\n"
            "    var^ got : std.math.Vector3 = yield^ 0\n"
            "    got := yield^ got.x\n"
            "    yield^ got.y\n"
            "}\n"
            "let^ c = gen()\n"
            "c.start()\n"
            "let^ a = c.resume(std.math.vec3(7, 8, 9))\n"
            "let^ b = c.resume(std.math.vec3(1, 2, 3))\n"
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
            "import^ std.math\n"
            "let^ gen = f^p:std.math.Vector3 -> "
            "c^{f^ -> std.math.Vector3;, std.math.Vector3} {\n"
            "    yield^ p\n"
            "    return^ p + p\n"
            "}\n"
            "let^ c = gen(std.math.vec3(2, 3, 4))\n"
            "let^ first = c.start()\n"
            "let^ last = c.resume()\n"
            "if^ first.x = 2.0 and^ last.y = 6.0 { return^ 1 }\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a host value still stays out of a yielded run");
    LHAT_CHECK(!checks("import^ std.math\n"
                       "let^ gen = p^ {\n"
                       "    yield^ 1, std.math.vec3(1, 2, 3)\n"
                       "}\n"),
               "a run position is one slot");

    LHAT_TEST("the program's answer refuses a host value");
    LHAT_CHECK(!checks("import^ std.math\n"
                       "return^ std.math.vec3(1, 2, 3)\n"),
               "top-level return");

    LHAT_TEST("a registered member is not written over");
    LHAT_CHECK(!checks("import^ std.math\n"
                       "var^ v = std.math.vec3(1, 2, 3)\n"
                       "v.dot := f^self^, o:std.math.Vector3 -> number^ {\n"
                       "    return^ 0\n"
                       "}\n"),
               "member overwrite");

    // The doors that stay open, pinned so a rule tightening by accident is
    // seen here: fields write, parameters and results pass.
    LHAT_TEST("what must keep checking still checks");
    LHAT_CHECK(checks("import^ std.math\n"
                      "var^ v = std.math.vec3(1, 2, 3)\n"
                      "v.x := 9\n"
                      "let^ f = f^p:std.math.Vector3 -> std.math.Vector3 {\n"
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
    test_tostring();
    test_collection();
    test_escapes();
    return lhat_test_report("test_math");
}
