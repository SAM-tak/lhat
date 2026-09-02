// L^ (lhat) -- tests for 02 の 13.14: a written type as an expression.
//
// A type spelling standing where a value is wanted is the descriptor
// typeof^ answers, and the name a let^ binds one to stands for the type in
// type positions -- the alias. The parse never backtracks (01 の 4.5): t^{
// and c^{ decide on two tokens, an f^/p^ head is read once and the '{' or
// the ';' says which it was, and a '|' after a run of names re-reads the
// run as the type name it already spells.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture.h"

static void test_aliases(void)
{
    Unit u;

    // The asked-for spelling, verbatim.
    LHAT_TEST("a union headed by t^{ is a value and its name is a type");
    check_text(&u,
               "let^Type = t^{number^[3]}|p^;|nil^\n"
               "let^v : Type = {1, 2, 3}\n"
               "let^w : Type = nil^\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The important crossing: FUNC is on names_a_type's refusal list, so an
    // f^ alias only resolves through the declaration flag.
    LHAT_TEST("a signature alias stands in an annotation");
    check_text(&u,
               "let^ Sig = f^number^ -> number^;\n"
               "let^ g : Sig = f^ x { return^ x * 2 }\n"
               "let^ n : number^ = g(3)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("an alias of an alias resolves through both");
    check_text(&u,
               "let^ A = t^{ x : number^ }\n"
               "let^ B = A|nil^\n"
               "let^ v : B = nil^\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a union headed by a name is the type it spells");
    check_text(&u,
               "let^ N = number^|nil^\n"
               "let^ x : N = 1\n"
               "let^ y : N = nil^\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 束縛の右辺は旧世界 (S46): the name inside its own let^ resolves past
    // the binding being made, and nothing outside declares it.
    LHAT_TEST("an alias may not name itself");
    check_text(&u, "let^ T = t^{ next : T|nil^ }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNKNOWN_TYPE);
    unit_dispose(&u);

    // The value's own type is the typeinfo typeof^ answers, so the name in
    // a value position is not a member of what it names.
    LHAT_TEST("the alias as a value is a descriptor, not an instance");
    check_text(&u,
               "let^ T = t^{ x : number^ }\n"
               "let^ v : T = T\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // A var^ may come to hold some other descriptor, so its name never
    // stands in a type position.
    LHAT_TEST("a var^-bound descriptor is not a type name");
    check_text(&u,
               "var^ T = t^{ x : number^ }\n"
               "let^ v : T = 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNKNOWN_TYPE);
    unit_dispose(&u);
}

static void test_parse_discrimination(void)
{
    Unit u;

    // The literal side of the shared head is untouched.
    LHAT_TEST("a literal with annotations and defaults still parses");
    check_text(&u,
               "let^ f = f^ x:number^, y:number^ = 2 -> number^ {\n"
               "    return^ x + y\n"
               "}\n"
               "let^ n : number^ = f(1, 2)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.3: a written signature carries no parameter names.
    LHAT_TEST("a named parameter in a signature is refused");
    check_text(&u, "let^ S = f^ x:number^ -> nil^;\n");
    LHAT_CHECK_EQ_INT(syntax_errors(&u), 1);
    unit_dispose(&u);

    // And a bare type where a body's parameter name is wanted.
    LHAT_TEST("a type where a parameter name is wanted is refused");
    check_text(&u, "let^ f = f^ t^{ x : number^ } { return^ 1 }\n");
    LHAT_CHECK_EQ_INT(syntax_errors(&u), 1);
    unit_dispose(&u);

    // A bare t^ stays the name it always was -- only 't^{' is a type head.
    LHAT_TEST("a bare t^ in expression position is still a name");
    check_text(&u, "let^ x = t^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
    unit_dispose(&u);
}

static void test_runtime(void)
{
    Run r;

    LHAT_TEST("the descriptor runs: fits^, signature, typeof^ equality");
    run_checked_text(&r,
             "let^ Type = t^{number^[3]}|p^;|nil^\n"
             "let^ v : Type = {1, 2, 3}\n"
             "var^ n = 0\n"
             "if^ v fits^ Type { n := n + 1 }\n"
             "let^ Sig = f^number^ -> number^;\n"
             "let^ g : Sig = f^ x { return^ x * 2 }\n"
             "n := n + g(10)\n"
             "if^ Type.signature.length > 0 { n := n + 100 }\n"
             "let^ N = number^|nil^\n"
             "let^ one : N = 1\n"
             "if^ typeof^(one) = number^|nil^ { n := n + 1000 }\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 1121);
    run_dispose(&r);
}

int main(void)
{
    test_aliases();
    test_parse_discrimination();
    test_runtime();
    return lhat_test_report("test_check_typedef");
}
