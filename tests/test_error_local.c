// L^ (lhat) -- tests for 04 の 2.7 and 2.7改: the family of errors a
// subroutine may not return.
//
// localerrordef^ declares under localerror^, which is disjoint from error^
// rather than under it. That disjointness is the whole design: putting the
// new family below error^ would have made 'p^ … -> T|error^' admit one, and
// then error^ itself could no longer be written in a result -- 2.3's `cause`
// with it. So the first thing pinned here is that error^ still behaves
// exactly as it did.
//
// The rule the family exists for is that what a caller receives may not
// touch localerror^: not a written result, not an inferred one, not one
// nested inside a table. What is left is resolving it where it was raised --
// catch^, or a try^{ } arm -- and one way for the thing that failed to
// survive: 10 章's chain, which 2.3 widened `cause` for.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture.h"

// Two declarations of the same shape, one per family, and a subroutine that
// fails the way one of them may.
//
// THERE IS NO `bad()` HERE, and that is the rule working: a subroutine whose
// result touches localerror^ cannot be written at all, so nothing raises one
// across a call. What raises one is 2.5's construction, written where it is
// caught -- which is the only place such an error ever comes to be.
#define DECLS                                           \
    "errordef^ IOError { NotFound, Denied }\n"          \
    "localerrordef^ IOFatal { Interrupted, Torn }\n"    \
    "let^ fine = f^ -> number^|IOError { return^ 1 }\n"

// How one is raised until as^ answers one for itself.
#define RAISED "error^IOFatal.Interrupted{ }"

// The regression this whole arrangement exists to prevent. If localerror^
// were under error^, none of these would still be writable.
static void test_error_is_untouched(void)
{
    Unit u;

    LHAT_TEST("error^ is still what a subroutine may return");
    check_text(&u, DECLS
               "let^ g = f^ -> number^|error^ { return^ try^ fine() }\n"
               "let^ h = f^ -> number^|IOError { return^ try^ fine() }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 2.3: every kind has one, and 2.7 widened it to take either family.
    // Read off a constructed one rather than off a caught one -- catch^ has
    // already removed the error arm by the time its left is bound, which is
    // 4.1 working and not something to assert around.
    LHAT_TEST("cause is still there and still reachable");
    check_text(&u, DECLS
               "let^ g = p^ {\n"
               "    let^ e = error^IOError.Denied{ }\n"
               "    let^ c = e.cause\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

// 2.7: the two tops are disjoint, which is the row 2.6's table gains.
static void test_the_two_families_are_disjoint(void)
{
    Unit u;

    LHAT_TEST("a localerror^ does not fit an error^ seat");
    check_text(&u, DECLS
               "let^ g = p^ {\n"
               "    var^ e : error^ = " RAISED "\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("but it fits a localerror^ seat");
    check_text(&u, DECLS
               "let^ g = p^ {\n"
               "    var^ e : localerror^ = " RAISED "\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and an error^ does not fit a localerror^ one");
    check_text(&u, DECLS
               "let^ g = p^ {\n"
               "    var^ e : localerror^ = error^IOError.Denied{ }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);
}

// 2.7改, through 5.3's existing enforcement point.
static void test_it_cannot_leave_the_frame(void)
{
    Unit u;

    LHAT_TEST("a bare try^ is refused, whatever the result says");
    check_text(&u, DECLS
               "let^ g = p^ -> number^ { return^ try^ " RAISED " }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_LOCAL_ERROR_ESCAPES);
    unit_dispose(&u);

    // The point of the family. Widening the signature is 5.3's answer and is
    // not this one's -- there is no result that admits it.
    LHAT_TEST("widening the result does not help; the result itself is refused");
    check_text(&u, DECLS
               "let^ g = p^ -> number^|IOFatal { return^ 1 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_LOCAL_ERROR_WRITTEN);
    unit_dispose(&u);

    // 5.3's "返り値型を書いていないとき": inference would otherwise take
    // whatever a try^ let past and quietly make it part of the result.
    LHAT_TEST("a body with no written result cannot launder it either");
    check_text(&u, DECLS
               "let^ g = p^ { return^ try^ " RAISED " }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_LOCAL_ERROR_ESCAPES);
    unit_dispose(&u);

    // The walk reads the whole settled signature rather than its outermost
    // shape, so a table carrying one out is the same escape.
    LHAT_TEST("nested inside what comes back is still coming back");
    check_text(&u, DECLS
               "let^ g = p^ -> t^{ v : number^|IOFatal } { return^ { v = 1 } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_LOCAL_ERROR_WRITTEN);
    unit_dispose(&u);

    // Down is not up. Whoever receives one cannot return it either, and an
    // argument is written at every call.
    LHAT_TEST("a parameter may carry one");
    check_text(&u, DECLS
               "let^ report = p^ e:IOFatal { }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

// What is left: resolving it where it stands.
static void test_resolving_it_here(void)
{
    Unit u;

    LHAT_TEST("catch^ takes either family");
    check_text(&u, DECLS
               "let^ g = p^ -> number^ { return^ " RAISED " catch^ 0 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 4.5: the arm is what keeps it from reaching 5.3 at all.
    LHAT_TEST("a try^{ } arm that names the kind takes it");
    check_text(&u, DECLS
               "let^ g = p^ -> number^ {\n"
               "    try^{\n"
               "        return^ try^ " RAISED "\n"
               "    catch^ IOFatal:\n"
               "        return^ 0\n"
               "    }\n"
               "    return^ 0\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and so does the bare arm");
    check_text(&u, DECLS
               "let^ g = p^ -> number^ {\n"
               "    try^{\n"
               "        return^ try^ " RAISED "\n"
               "    catch^:\n"
               "        return^ 0\n"
               "    }\n"
               "    return^ 0\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 4.5: what no arm takes goes out, which is where 2.7改 catches it.
    LHAT_TEST("an arm for another family does not take it");
    check_text(&u, DECLS
               "let^ g = p^ -> number^ {\n"
               "    try^{\n"
               "        return^ try^ " RAISED "\n"
               "    catch^ IOError:\n"
               "        return^ 0\n"
               "    }\n"
               "    return^ 0\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_LOCAL_ERROR_ESCAPES);
    unit_dispose(&u);
}

// 10 章 with 2.3改: wrapping what failed inside one's own error IS resolving
// it here, and it is the one way the thing that failed outlives the frame.
static void test_the_chain_is_the_way_out(void)
{
    Unit u;

    LHAT_TEST("a localerror^ may be a cause");
    check_text(&u, DECLS
               "let^ g = p^ -> number^|IOError {\n"
               "    return^ " RAISED " catch^ error^IOError.Denied{ cause = it^ }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // And the reason the walk has to stop at a nominal type: `cause` takes
    // either family, so descending into a kind's fields would make every
    // error kind on earth answer "touches localerror^" and nothing could be
    // returned at all. This case is that regression.
    LHAT_TEST("cause taking either family leaves errors returnable");
    check_text(&u, DECLS
               "let^ g = f^ -> number^|IOError { return^ try^ fine() }\n"
               "let^ h = f^ -> number^|error^ { return^ try^ fine() }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The hole that closes: a nominal type is an atom to the walk, so the
    // written side has to refuse a field that would hide one inside it.
    LHAT_TEST("a declared field cannot be one");
    check_text(&u, DECLS
               "errordef^ Wrapper { Held { inner : IOFatal } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_LOCAL_ERROR_WRITTEN);
    unit_dispose(&u);
}

int main(void)
{
    test_error_is_untouched();
    test_the_two_families_are_disjoint();
    test_it_cannot_leave_the_frame();
    test_resolving_it_here();
    test_the_chain_is_the_way_out();
    return lhat_test_report("test_error_local");
}
