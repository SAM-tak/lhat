// L^ (lhat) -- tests for 02 の 19 章: enum^.
//
// The declaration builds singletons where it stands; what is pinned here is
// the value side -- numbering, written values of any kind, identity,
// fits^ against the declaration and a member, the built-in members -- and
// the match that 19.5 lets stand without other^.

#include "fixture.h"

static void test_enums(void)
{
    Run r;

    LHAT_TEST("members carry their numbers and their written values");
    run_text(&r,
             "enum^ E {\n"
             "    AAA,\n"
             "    BBB,\n"
             "    CCC = \"aaa\",\n"
             "    DDD = 10,\n"
             "    EEE,\n"
             "}\n"
             "var^ n = 0\n"
             "if^ E.AAA.value = 1 { n := n + 1 }\n"
             "if^ E.BBB.value = 2 { n := n + 10 }\n"
             "if^ E.CCC.value = \"aaa\" { n := n + 100 }\n"
             "if^ E.DDD.value = 10 { n := n + 1000 }\n"
             "if^ E.EEE.value = 11 { n := n + 10000 }\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 11111);
    run_dispose(&r);

    LHAT_TEST("a member is one value, and the declaration answers fits^");
    // fits^ against a resolved name reads what the checker settled
    // (compile.c's from_checked_type), so this one runs checked.
    run_checked_text(&r,
             "enum^ E { AAA, BBB }\n"
             "let^ x = E.AAA\n"
             "var^ n = 0\n"
             "if^ x = E.AAA { n := n + 1 }\n"
             "if^ x fits^ E { n := n + 10 }\n"
             "if^ x fits^ E.AAA { n := n + 100 }\n"
             "if^ x fits^ E.BBB { n := n - 1 else^: n := n + 1000 }\n"
             "if^ x.enum = E { n := n + 10000 }\n"
             "if^ x.tostring() = \"AAA\" { n := n + 100000 }\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 111111);
    run_dispose(&r);

    LHAT_TEST("a value may be a table, evaluated where the declaration ran");
    run_text(&r,
             "enum^ E { AAA = { 7, 8 } }\n"
             "return^ E.AAA.value[2]\n");
    CHECK_INTEGER(&r, 8);
    run_dispose(&r);

    LHAT_TEST("a match naming every member stands without other^");
    run_text(&r,
             "enum^ E { AAA, BBB, CCC }\n"
             "let^ x = E.BBB\n"
             "return^ for^x:\n"
             "    when^E.AAA: 1\n"
             "    when^E.BBB: 2\n"
             "    when^E.CCC: 3\n"
             ";\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);
}

int main(void)
{
    test_enums();
    return lhat_test_report("test_vm_enum");
}
