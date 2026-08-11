// L^ (lhat) -- tests for the bytecode and the machine.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed. Programs are run end to end, since what is worth pinning is the
// answer rather than the instructions chosen to reach it -- 5.1 expects those
// to be replaced by specialised ones later.

#include <math.h>
#include <string.h>

#include "code.h"
#include "fixture.h"

// 02 の 13.8改: several values, on the stack, with no table between them.
static void test_multi_value_return(void)
{
    Run r;

    LHAT_TEST("several values come back and several names take them");
    run_checked_text(&r,
                     "var^ divmod = f^ a:number^, b:number^ "
                     "-> (number^, number^) {\n"
                     "  return^ a // b, a % b }\n"
                     "var^ q, r2 = divmod(7, 2)\n"
                     "return^ q * 10 + r2\n");
    CHECK_INTEGER(&r, 31);
    run_dispose(&r);

    LHAT_TEST("the positions keep their own types");
    run_checked_text(&r,
                     "var^ both = f^ -> (number^, string^) {\n"
                     "  return^ 7, \"a\" }\n"
                     "var^ n, s = both()\n"
                     "return^ n.tostring^() .. s\n");
    CHECK_STRING(&r, "7a");
    run_dispose(&r);

    // What 13.8 promised when it said the cost would be absorbed by the
    // implementation. The loop's only heap traffic would be a table for the
    // returned pair, so the count is the proof.
    LHAT_TEST("and nothing is allocated to carry them");
    run_checked_text(&r,
                     "var^ divmod = f^ a:number^, b:number^ "
                     "-> (number^, number^) {\n"
                     "  return^ a // b, a % b }\n"
                     "var^ total = 0\n"
                     "repeat^ 2000 {\n"
                     "  var^ q, r2 = divmod(7, 2)\n"
                     "  total := total + q + r2 }\n"
                     "return^ total\n");
    CHECK_INTEGER(&r, 8000);  // (7//2 + 7%2) * 2000
    LHAT_CHECK_EQ_INT(r.ran.collected, 0);
    run_dispose(&r);

    // The negative control. Without it the case above says nothing about
    // whether the table path still costs what it always did.
    LHAT_TEST("where the same loop through a table does allocate");
    run_checked_text(&r,
                     "var^ divmod = f^ a:number^, b:number^ "
                     "-> t^{ number^, number^ } {\n"
                     "  return^ { a // b, a % b } }\n"
                     "var^ total = 0\n"
                     "repeat^ 2000 {\n"
                     "  var^ t = divmod(7, 2)\n"
                     "  total := total + t[1] + t[2] }\n"
                     "return^ total\n");
    CHECK_INTEGER(&r, 8000);
    LHAT_CHECK(r.ran.collected > 1000, "the table path still makes tables");
    run_dispose(&r);

    // 13.8改: the two forms are told apart by what is written, so a table is
    // still a table and needs no escape analysis to stay one.
    LHAT_TEST("'return^ { a, b }' still answers a table");
    run_checked_text(&r,
                     "var^ pair = f^ -> t^{ number^, number^ } {\n"
                     "  return^ { 3, 4 } }\n"
                     "var^ t = pair()\n"
                     "return^ t[1] * 10 + t[2]\n");
    CHECK_INTEGER(&r, 34);
    run_dispose(&r);

    // 8.6: ':=' reaches existing names, and 8.6改3's read-then-write holds
    // here: there is one read.
    LHAT_TEST("existing names take them too");
    run_checked_text(&r,
                     "var^ both = f^ -> (number^, number^) {\n"
                     "  return^ 4, 9 }\n"
                     "var^ a = 0\n"
                     "var^ b = 0\n"
                     "a, b := both()\n"
                     "return^ a * 10 + b\n");
    CHECK_INTEGER(&r, 49);
    run_dispose(&r);

    // 13.8改: the one bridge. A tuple is not a value a name can hold; this
    // makes one that is, with 14.10's positions numbered from 1.
    LHAT_TEST("pack^ makes a table of them");
    run_checked_text(&r,
                     "var^ both = f^ -> (number^, string^) {\n"
                     "  return^ 5, \"b\" }\n"
                     "var^ t = pack^ both()\n"
                     "return^ t[1].tostring^() .. t[2]\n");
    CHECK_STRING(&r, "5b");
    run_dispose(&r);

    // 04 の 3.1 with 13.8改: the error goes around the values. One run is
    // reserved and the head slot's tag tells the two arms apart, so ISERROR
    // reads what it always read and nothing is allocated on either path.
    LHAT_TEST("try^ lets a tuple through and returns the error");
    run_checked_text(&r,
                     "errordef^ DivError { ByZero }\n"
                     "var^ divmod = f^ a:number^, b:number^ "
                     "-> (number^, number^)|DivError {\n"
                     "  if^ b = 0 { return^ error^DivError.ByZero{} }\n"
                     "  return^ a // b, a % b }\n"
                     "var^ go = f^ -> number^|DivError {\n"
                     "  var^ q, r2 = try^ divmod(7, 2)\n"
                     "  return^ q * 10 + r2 }\n"
                     "return^ go()\n");
    CHECK_INTEGER(&r, 31);
    run_dispose(&r);

    LHAT_TEST("and the error arm leaves through the same slot");
    run_checked_text(&r,
                     "errordef^ DivError { ByZero }\n"
                     "var^ divmod = f^ a:number^, b:number^ "
                     "-> (number^, number^)|DivError {\n"
                     "  if^ b = 0 { return^ error^DivError.ByZero{} }\n"
                     "  return^ a // b, a % b }\n"
                     "var^ go = f^ -> number^|DivError {\n"
                     "  var^ q, r2 = try^ divmod(7, 0)\n"
                     "  return^ q * 10 + r2 }\n"
                     "return^ go() catch^ -1\n");
    CHECK_INTEGER(&r, -1);
    run_dispose(&r);

    // 13.9 with 13.8改: Y is a result, so a coroutine may yield several
    // values. R is an input and stays one -- a resume sends one value.
    // What a resume answers is 13.9's union of Y and T, so taking it apart
    // without asking isDone first needs the two to agree -- then the union
    // collapses to that one tuple. 13.9's uniformity is what makes this the
    // natural shape rather than a restriction added here.
    LHAT_TEST("a coroutine yields several values");
    run_checked_text(&r,
                     "var^ gen = p^ -> (number^, string^) {\n"
                     "  yield^ 1, \"a\"\n"
                     "  yield^ 2, \"b\"\n"
                     "  return^ 0, \"\" }\n"
                     "var^ co = gen()\n"
                     "var^ n1, s1 = co.start()\n"
                     "var^ n2, s2 = co.resume(nil^)\n"
                     "return^ n1.tostring^() .. s1 .. n2.tostring^() .. s2\n");
    CHECK_STRING(&r, "1a2b");
    run_dispose(&r);

    // The callee answers a run and the call site reserved one slot. Not
    // quietly boxed: a tuple and a t^{...} are different types.
    LHAT_TEST("a tuple answered where one value was expected faults");
    run_text(&r,
             "var^ both = f^ { return^ 1, 2 }\n"
             "var^ x = both()\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_OK);
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TUPLE_UNEXPECTED);
    run_dispose(&r);

    // 03 の 5.3: both sides declare and the machine collates -- a mismatch
    // is dropped, never reconciled. The annotation promises three positions,
    // so the checked call site reserves three; the body answers two. The
    // checker reports the lie, and compiling goes ahead regardless here,
    // which is what leaves the machine to refuse it a second time.
    LHAT_TEST("a run answering fewer positions than reserved is refused");
    run_checked_text(&r,
                     "var^ both : f^ -> (number^, number^, number^); ="
                     " f^ { return^ 1, 2 }\n"
                     "var^ a, b, c = both()\n"
                     "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_OK);
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TUPLE_ARITY);
    run_dispose(&r);
}

// 02 の 16.3 with 13.8改: a walk's pair is the tuple (K, V), and a single
// name takes the sequence half's values -- neither shape allocates, which is
// what keeps a walk free of per-step heap traffic.
static void test_walk_shapes(void)
{
    Run r;

    LHAT_TEST("one name over a table takes the values in order");
    run_text(&r,
             "var^ total = 0\n"
             "for^ v in^ { 10, 20, 30 } { total := total + v }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 60);
    run_dispose(&r);

    LHAT_TEST("and never visits the keyed half");
    run_text(&r,
             "var^ turns = 0\n"
             "var^ t = { a := 5, b := 7 }\n"
             "for^ v in^ t { turns := turns + 1 }\n"
             "return^ turns\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 13.7: the variadic collector is a table, so its walk changes shape the
    // same way -- one name takes the elements themselves now.
    LHAT_TEST("one name over ... takes the elements");
    run_text(&r,
             "var^ join = p^ ...:number^ {\n"
             "  var^ total = 0\n"
             "  for^ x in^ ... { total := total + x }\n"
             "  return^ total }\n"
             "return^ join(1, 2, 3)\n");
    CHECK_INTEGER(&r, 6);
    run_dispose(&r);

    // A step's only heap traffic would be a pair table -- 2000 steps of one
    // walk touch the heap not at all, so the collector's count is the proof. (The walk
    // coroutine itself is still made once per loop, which is why the loop
    // here is one loop of many steps.)
    LHAT_TEST("two names walk a table without allocating per step");
    run_checked_text(&r,
                     "var^ t = { }\n"
                     "for^ i from^ 1 to^ 2000 { t[i] := i }\n"
                     "var^ total = 0\n"
                     "for^ k, v in^ t { total := total + v }\n"
                     "return^ total\n");
    CHECK_INTEGER(&r, 2000 * 2001 / 2);
    // A step apiece would be ~2000; a stray object or two from the build
    // phase crossing a collection threshold is not that.
    LHAT_CHECK(r.ran.collected < 10, "no per-step allocation");
    run_dispose(&r);

    LHAT_TEST("and so does one name");
    run_checked_text(&r,
                     "var^ t = { }\n"
                     "for^ i from^ 1 to^ 2000 { t[i] := i }\n"
                     "var^ total = 0\n"
                     "for^ v in^ t { total := total + v }\n"
                     "return^ total\n");
    CHECK_INTEGER(&r, 2000 * 2001 / 2);
    // A step apiece would be ~2000; a stray object or two from the build
    // phase crossing a collection threshold is not that.
    LHAT_CHECK(r.ran.collected < 10, "no per-step allocation");
    run_dispose(&r);

    // The negative control: driving the walk by hand still answers the pair
    // as a table -- the one place a name has to be able to hold the answer.
    LHAT_TEST("a hand-driven walk still allocates its pairs");
    run_text(&r,
             "var^ t = { 10, 20, 30 }\n"
             "var^ total = 0\n"
             "repeat^ 700 {\n"
             "  var^ w = t.iterate^()\n"
             "  var^ pair = w.start()\n"
             "  total := total + pair[2]\n"
             "}\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 700 * 10);
    LHAT_CHECK(r.ran.collected > 500, "the pair tables were made and dropped");
    run_dispose(&r);

    // A user iterator that yields tuples meets the same binds the built-in
    // walk does, with the same zero cost.
    LHAT_TEST("a user iterator yields tuples into two names");
    run_checked_text(&r,
                     "var^ gen = p^ -> (number^, number^) {\n"
                     "  yield^ 1, 10\n"
                     "  yield^ 2, 20\n"
                     "  return^ 0, 0 }\n"
                     "var^ total = 0\n"
                     "for^ k, v in^ gen() { total := total + k + v }\n"
                     "return^ total\n");
    CHECK_INTEGER(&r, 33);
    run_dispose(&r);

    // And one that yields tables -- the shape from before tuples -- still
    // works: the machine expands its positions into the same run.
    LHAT_TEST("a user iterator yielding tables still meets two names");
    run_text(&r,
             "var^ gen = p^ {\n"
             "  yield^ { 1, 10 }\n"
             "  yield^ { 2, 20 } }\n"
             "var^ total = 0\n"
             "for^ k, v in^ gen() { total := total + k + v }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 33);
    run_dispose(&r);

    // 16.3's other guarantees, over the new shapes.
    LHAT_TEST("break^ leaves a two-name walk cleanly");
    run_text(&r,
             "var^ total = 0\n"
             "for^ k, v in^ { 10, 20, 30 } {\n"
             "  if^ k = 3 { break^ }\n"
             "  total := total + v }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 30);
    run_dispose(&r);

    LHAT_TEST("a two-name walk over an empty table walks no turns");
    run_text(&r,
             "var^ turns = 0\n"
             "for^ k, v in^ { } { turns := turns + 1 }\n"
             "return^ turns\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);
}

int main(void)
{
    test_multi_value_return();
    test_walk_shapes();
    return lhat_test_report("test_vm_tuple");
}
