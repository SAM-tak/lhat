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

// Whether the opcode appears anywhere in a unit -- its own chunk or a body
// written inside it. A subroutine's instructions live in its own proto, so
// asking only the top one would miss everything a body does.
static bool chunk_has_op_deep(const LhatProto *proto, LhatOpcode op)
{
    if (proto == NULL) {
        return false;
    }
    for (size_t i = 0; i < proto->chunk.count; i++) {
        if (lhat_op(proto->chunk.code[i]) == op) {
            return true;
        }
    }
    for (size_t i = 0; i < proto->proto_count; i++) {
        if (chunk_has_op_deep(proto->protos[i], op)) {
            return true;
        }
    }
    return false;
}

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

    // 13.8改 (S48): the literal. Both arms of the catch^ write the same run,
    // so the error arm's replacement lands where the values would have.
    // 11.7 with 13.8改: '??' is the same shape as catch^, asking about nil^
    // instead -- so it takes a replacement tuple the same way. ISNIL reads
    // the head slot, and a run's head is not nil^.
    LHAT_TEST("?? replaces an absent tuple with a literal");
    run_checked_text(&r,
                     "var^ maybe = f^ b:number^ -> (number^, number^)|nil^ {\n"
                     "  if^ b = 0 { return^ nil^ }\n"
                     "  return^ b, b * 2 }\n"
                     "var^ q, r2 = maybe(0) ?? (5, 6)\n"
                     "return^ q * 10 + r2\n");
    CHECK_INTEGER(&r, 56);
    run_dispose(&r);

    LHAT_TEST("and leaves the values alone when there is one");
    run_checked_text(&r,
                     "var^ maybe = f^ b:number^ -> (number^, number^)|nil^ {\n"
                     "  if^ b = 0 { return^ nil^ }\n"
                     "  return^ b, b * 2 }\n"
                     "var^ q, r2 = maybe(3) ?? (5, 6)\n"
                     "return^ q * 10 + r2\n");
    CHECK_INTEGER(&r, 36);
    run_dispose(&r);

    // S47 said the loop is what discriminates a walk's answer and takes it
    // apart. With '??' the hand-driven form can too: collapse the nil^ and
    // what is left is the pair.
    LHAT_TEST("?? lets a hand-driven walk be taken apart");
    run_checked_text(&r,
                     "var^ t = { 10, 20 }\n"
                     "var^ w = t.iterate^()\n"
                     "var^ k, v = w.start() ?? (0, 0)\n"
                     "return^ k * 100 + v\n");
    CHECK_INTEGER(&r, 110);
    run_dispose(&r);

    LHAT_TEST("and answers the replacement once it runs out");
    run_checked_text(&r,
                     "var^ t = { }\n"
                     "var^ w = t.iterate^()\n"
                     "var^ k, v = w.start() ?? (7, 8)\n"
                     "return^ k * 10 + v\n");
    CHECK_INTEGER(&r, 78);
    run_dispose(&r);

    LHAT_TEST("neither arm of a ?? allocates");
    run_checked_text(&r,
                     "var^ maybe = f^ b:number^ -> (number^, number^)|nil^ {\n"
                     "  if^ b = 0 { return^ nil^ }\n"
                     "  return^ b, b * 2 }\n"
                     "var^ total = 0\n"
                     "repeat^ 2000 {\n"
                     "  var^ q, r2 = maybe(3) ?? (5, 6)\n"
                     "  total := total + q + r2 }\n"
                     "return^ total\n");
    CHECK_INTEGER(&r, 18000);
    LHAT_CHECK_EQ_INT(r.ran.collected, 0);
    run_dispose(&r);

    LHAT_TEST("catch^ replaces a failed tuple with a literal");
    run_checked_text(&r,
                     "errordef^ DivError { ByZero }\n"
                     "var^ divmod = f^ a:number^, b:number^ "
                     "-> (number^, number^)|DivError {\n"
                     "  if^ b = 0 { return^ error^DivError.ByZero{} }\n"
                     "  return^ a // b, a % b }\n"
                     "var^ q, r2 = divmod(7, 0) catch^ (5, 6)\n"
                     "return^ q * 10 + r2\n");
    CHECK_INTEGER(&r, 56);
    run_dispose(&r);

    LHAT_TEST("and leaves the values alone when it succeeds");
    run_checked_text(&r,
                     "errordef^ DivError { ByZero }\n"
                     "var^ divmod = f^ a:number^, b:number^ "
                     "-> (number^, number^)|DivError {\n"
                     "  if^ b = 0 { return^ error^DivError.ByZero{} }\n"
                     "  return^ a // b, a % b }\n"
                     "var^ q, r2 = divmod(7, 2) catch^ (5, 6)\n"
                     "return^ q * 10 + r2\n");
    CHECK_INTEGER(&r, 31);
    run_dispose(&r);

    // One run is shared by both arms, so nothing is boxed on either path.
    LHAT_TEST("and neither arm allocates");
    run_checked_text(&r,
                     "errordef^ DivError { ByZero }\n"
                     "var^ divmod = f^ a:number^, b:number^ "
                     "-> (number^, number^)|DivError {\n"
                     "  if^ b = 0 { return^ error^DivError.ByZero{} }\n"
                     "  return^ a // b, a % b }\n"
                     "var^ total = 0\n"
                     "repeat^ 2000 {\n"
                     "  var^ q, r2 = divmod(7, 2) catch^ (5, 6)\n"
                     "  total := total + q + r2 }\n"
                     "return^ total\n");
    CHECK_INTEGER(&r, 8000);
    LHAT_CHECK_EQ_INT(r.ran.collected, 0);
    run_dispose(&r);

    // 15.12: the body's one expression is what the function answers with,
    // and 13.8改 folds a literal written there into the very node
    // 'return^ 0, 1' makes -- so this needs no rule of its own.
    LHAT_TEST("an implicit return answers a tuple");
    run_checked_text(&r,
                     "var^ pair = f^ -> (number^, number^) { (3, 4) }\n"
                     "var^ a, b = pair()\n"
                     "return^ a * 10 + b\n");
    CHECK_INTEGER(&r, 34);
    run_dispose(&r);

    // The fold is what makes the two spellings one node, so neither leaves a
    // MAKERUN behind -- the machine builds the head at the frame boundary.
    LHAT_TEST("and compiles to what the written return^ compiles to");
    run_checked_text(&r,
                     "var^ pair = f^ -> (number^, number^) { (3, 4) }\n"
                     "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_OK);
    LHAT_CHECK(!chunk_has_op_deep(r.proto, LHAT_BC_MAKERUN),
               "the fold left no run to build");
    run_dispose(&r);

    LHAT_TEST("while a catch^ replacement does build one");
    run_checked_text(&r,
                     "errordef^ DivError { ByZero }\n"
                     "var^ divmod = f^ a:number^, b:number^ "
                     "-> (number^, number^)|DivError {\n"
                     "  if^ b = 0 { return^ error^DivError.ByZero{} }\n"
                     "  return^ a // b, a % b }\n"
                     "var^ q, r2 = divmod(7, 0) catch^ (5, 6)\n"
                     "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_OK);
    LHAT_CHECK(chunk_has_op_deep(r.proto, LHAT_BC_MAKERUN),
               "the literal built its own head");
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
                     "var^ n2, s2 = co.resume()\n"
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

    // Hand-driving allocates nothing either: the pair is a tuple here too,
    // riding the reserved slots rather than a per-step table.
    LHAT_TEST("a hand-driven walk allocates nothing per step");
    run_checked_text(&r,
                     "var^ t = { }\n"
                     "for^ i from^ 1 to^ 700 { t[i] := i }\n"
                     "var^ w = t.iterate^()\n"
                     "var^ n = 0\n"
                     "w.start()\n"
                     "repeat^ 699 {\n"
                     "  w.resume()\n"
                     "  n := n + 1\n"
                     "}\n"
                     "return^ n\n");
    CHECK_INTEGER(&r, 699);
    // Reading `w.resume` makes a native per step -- a member-lookup cost
    // this test is not about -- so ~700 objects are collected regardless.
    // The pair tables would be another ~700 on top; their absence is what
    // the bound checks.
    LHAT_CHECK(r.ran.collected < 1000, "no per-step pair table");
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

// 13.7: the spread that forwards a collected tail forwards a tuple too. The
// positions are already a run on the stack, so the table 'pack^' used to
// interpose is the whole of what this saves.
static void test_tuple_spread(void)
{
    Run r;

    LHAT_TEST("a tuple spreads into a variadic tail");
    run_checked_text(&r,
                     "var^ f = f^ -> (number^, number^) { return^ 10, 20 }\n"
                     "var^ sum = f^ ...:number^ -> number^ {\n"
                     "  var^ total = 0\n"
                     "  for^ x in^ ... { total := total + x }\n"
                     "  return^ total }\n"
                     "return^ sum(f()...)\n");
    CHECK_INTEGER(&r, 30);
    run_dispose(&r);

    LHAT_TEST("values written into the tail may lead it");
    run_checked_text(&r,
                     "var^ f = f^ -> (number^, number^) { return^ 10, 20 }\n"
                     "var^ sum = f^ ...:number^ -> number^ {\n"
                     "  var^ total = 0\n"
                     "  for^ x in^ ... { total := total + x }\n"
                     "  return^ total }\n"
                     "return^ sum(1, 2, f()...)\n");
    CHECK_INTEGER(&r, 33);
    run_dispose(&r);

    LHAT_TEST("and takes its place after the fixed arguments");
    run_checked_text(&r,
                     "var^ f = f^ -> (number^, number^) { return^ 10, 20 }\n"
                     "var^ sum = f^ first:number^, ...:number^ -> number^ {\n"
                     "  var^ total = first\n"
                     "  for^ x in^ ... { total := total + x }\n"
                     "  return^ total }\n"
                     "return^ sum(1, f()...)\n");
    CHECK_INTEGER(&r, 31);
    run_dispose(&r);

    // Each position keeps its own type through the spread, so a tail of
    // string^ takes the string position of a mixed tuple.
    LHAT_TEST("the positions keep their types across the spread");
    run_checked_text(&r,
                     "var^ f = f^ -> (string^, string^) { return^ \"a\", \"b\" }\n"
                     "var^ join = f^ ...:string^ -> string^ {\n"
                     "  var^ out = \"\"\n"
                     "  for^ s in^ ... { out := out .. s }\n"
                     "  return^ out }\n"
                     "return^ join(f()...)\n");
    CHECK_STRING(&r, "ab");
    run_dispose(&r);

    // What the work is for. Both loops pay for the callee's collector, which
    // 13.7 defines as an ordinary table; only the spread through a tuple is
    // spared the second table 'pack^' would build to carry the same values.
    LHAT_TEST("and no table is built to carry the positions there");
    run_checked_text(&r,
                     "var^ f = f^ -> (number^, number^) { return^ 10, 20 }\n"
                     "var^ sum = f^ ...:number^ -> number^ {\n"
                     "  var^ total = 0\n"
                     "  for^ x in^ ... { total := total + x }\n"
                     "  return^ total }\n"
                     "var^ total = 0\n"
                     "repeat^ 2000 { total := total + sum(f()...) }\n"
                     "return^ total\n");
    CHECK_INTEGER(&r, 60000);
    size_t spread_cost = r.ran.collected;
    run_dispose(&r);

    // The control: the same loop into the same variadic tail, with the table
    // written back in. Both pay for the collector, so what is left of the
    // difference is the table 'pack^' builds and the spread does not.
    LHAT_TEST("where routing the same values through a table does build one");
    run_checked_text(&r,
                     "var^ f = f^ -> (number^, number^) { return^ 10, 20 }\n"
                     "var^ sum = f^ ...:t^{ number^, number^ } -> number^ {\n"
                     "  var^ total = 0\n"
                     "  for^ t in^ ... { total := total + t[1] + t[2] }\n"
                     "  return^ total }\n"
                     "var^ total = 0\n"
                     "repeat^ 2000 { total := total + sum(pack^ f()) }\n"
                     "return^ total\n");
    CHECK_INTEGER(&r, 60000);
    LHAT_CHECK(r.ran.collected > spread_cost,
               "the table path allocates what the spread does not");
    run_dispose(&r);
}

int main(void)
{
    test_multi_value_return();
    test_walk_shapes();
    test_tuple_spread();
    return lhat_test_report("test_vm_tuple");
}
