// L^ (lhat) -- tests for 03 の 3.4改4: instantiation checking.
//
// A let^-bound literal whose parameters nothing was written on is checked
// once under each shape its call sites hand it, so strict passes with no
// annotation where the callers decide everything. What cannot take a shape
// -- a var^'s value, an uncalled body, an exported name whose callers are in
// other units -- keeps the undecided reports these walks used to leave
// everywhere.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture.h"

static void test_shapes_decide(void)
{
    Unit u;

    // The 24.lh specimen: an INDEX-only body demands nothing, so before
    // 3.4改4 this was two LHAT_CHECK_ERR_PARAM_UNDECIDED under strict.
    LHAT_TEST("one call shape checks the body and strict passes unannotated");
    check_text(&u,
               "let^ same = f^ want, used {\n"
               "    for^ i from^ 1 to^ 3 {\n"
               "        if^ want[i] != used[i] { return^ false^ }\n"
               "    }\n"
               "    return^ true^\n"
               "}\n"
               "let^ a = { 1, 2, 3 }\n"
               "let^ b = { 4, 5, 6 }\n"
               "let^ ok : bool^ = same(a, b)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // Two shapes, and each call answers under its own -- a joined answer
    // (number^|string^) would refuse both annotations here.
    LHAT_TEST("each call site answers under its own shape");
    check_text(&u,
               "let^ id = f^ x { return^ x }\n"
               "let^ n : number^ = id(1)\n"
               "let^ s : string^ = id(\"s\")\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A chain: the outer body's call is itself the shape the inner takes.
    LHAT_TEST("a shape reaches through a chain of unannotated bodies");
    check_text(&u,
               "let^ dbl = f^ x { return^ x + x }\n"
               "let^ quad = f^ x { return^ dbl(dbl(x)) }\n"
               "let^ n : number^ = quad(2)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A body may call a name whose let^ stands later -- the shape arrives a
    // round after the binding does.
    LHAT_TEST("a call written above the definition still hands its shape in");
    check_text(&u,
               "let^ outer = f^ -> number^ { return^ use(1) }\n"
               "let^ use = f^ x { return^ x + 1 }\n"
               "let^ n : number^ = outer()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.10: recursion goes through this^, which is no call site of the
    // binding -- the one recorded shape carries the body to its fixpoint.
    LHAT_TEST("a recursive body settles under its one shape");
    check_text(&u,
               "let^ fact = f^ n {\n"
               "    if^ n <= 1 { return^ 1 }\n"
               "    return^ n * this^(n - 1)\n"
               "}\n"
               "let^ y : number^ = fact(3)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A literal argument written out in full rides along in a shape; the
    // g(x) inside then has a real signature to check against.
    LHAT_TEST("an annotated literal argument rides along in a shape");
    check_text(&u,
               "let^ apply = f^ g, x { return^ g(x) }\n"
               "let^ n : number^ = apply(f^ v:number^ -> number^ {\n"
               "    return^ v + 1\n"
               "}, 1)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

static void test_refusals(void)
{
    Unit u;

    // The body cannot take the shape: the walk under it reports, the report
    // is rolled back the way a round's is, and the call site is what says
    // so -- the position the writer has to change.
    LHAT_TEST("a shape the body refuses is said at the call");
    check_text(&u,
               "let^ inc = f^ x { return^ x + 1 }\n"
               "let^ n : number^ = inc(1)\n"
               "let^ s = inc(\"s\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_SHAPE_REFUSED);
    unit_dispose(&u);

    // A shape needs every position: an arity error keeps today's reading,
    // and with no shape recorded the parameter stays undecided too.
    LHAT_TEST("a call of the wrong arity records no shape");
    check_text(&u,
               "let^ f = f^ x { return^ x }\n"
               "let^ y = f(1, 2)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);
}

static void test_still_undecided(void)
{
    Unit u;

    // No call, no shape -- the reports 3.1③ always made stand.
    LHAT_TEST("an uncalled unannotated body is still undecided");
    check_text(&u,
               "let^ f = f^ x { return^ x }\n"
               "let^ n = 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PARAM_UNDECIDED);
    unit_dispose(&u);

    // A var^ may come to mean some other subroutine, so no call shape is
    // collected for one -- calling it changes nothing.
    LHAT_TEST("a var^-bound body takes no shapes");
    check_text(&u,
               "var^ f = f^ x { return^ x }\n"
               "let^ y = f(1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PARAM_UNDECIDED);
    unit_dispose(&u);

    // 05 の 4.3 at the unit's edge: an exported name's callers live in units
    // checked after this one, so nothing ever hands it a shape -- a
    // published signature still does not depend on who uses it.
    LHAT_TEST("an exported unannotated body is still undecided");
    check_text(&u,
               "module^ m\n"
               "public^ let^ f = f^ x { return^ x }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PARAM_UNDECIDED);
    unit_dispose(&u);

    // Even one this unit calls itself: the calls of this unit must not be
    // what narrows a promise made to every other.
    LHAT_TEST("a unit's own call does not decide its exported body");
    check_text(&u,
               "module^ m\n"
               "public^ let^ f = f^ x { return^ x }\n"
               "let^ n = f(1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PARAM_UNDECIDED);
    unit_dispose(&u);

    // A literal argument with an unwritten parameter would need the very
    // expectation the shape is being collected to find -- those calls keep
    // today's reading.
    LHAT_TEST("an unannotated literal argument defers the whole shape");
    check_text(&u,
               "let^ apply = f^ g, x { return^ g(x) }\n"
               "let^ n = apply(f^ v { return^ v }, 1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PARAM_UNDECIDED);
    unit_dispose(&u);

    // 3.5: relaxed reports none of the undecided trio, before or after --
    // the shapes only remove reports, never add ones relaxed would show.
    LHAT_TEST("relaxed stays silent about an uncalled body");
    check_relaxed_text(&u,
                       "let^ f = f^ x { return^ x }\n"
                       "let^ n = 1\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

int main(void)
{
    test_shapes_decide();
    test_refusals();
    test_still_undecided();
    return lhat_test_report("test_check_instantiation");
}
