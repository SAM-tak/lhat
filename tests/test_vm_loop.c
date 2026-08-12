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

// 16.5: repeat^ is the one that carries no focus.
static void test_repeat(void)
{
    Run r;

    LHAT_TEST("repeat^ n runs n times");
    run_text(&r, "var^ x = 0\nrepeat^ 5 { x := x + 1 }\nreturn^ x\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    LHAT_TEST("repeat^ 0 runs none");
    run_text(&r, "var^ x = 0\nrepeat^ 0 { x := x + 1 }\nreturn^ x\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // The count says how many times, so changing it inside cannot move the
    // finishing line.
    LHAT_TEST("the count is read once");
    run_text(&r,
             "var^ n = 3\n"
             "var^ x = 0\n"
             "repeat^ n { n := 100 x := x + 1 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    LHAT_TEST("repeat^ while^ tests before the body");
    run_text(&r,
             "var^ i = 0\n"
             "repeat^ while^ i < 4 { i := i + 1 }\n"
             "return^ i\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);

    LHAT_TEST("a condition that is false at the start runs nothing");
    run_text(&r,
             "var^ x = 0\n"
             "repeat^ while^ false^ { x := 1 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 16.5: until^ is while^ negated, tested in the same place.
    LHAT_TEST("repeat^ until^ is while^ negated");
    run_text(&r,
             "var^ i = 0\n"
             "repeat^ until^ i ≧ 4 { i := i + 1 }\n"
             "return^ i\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);

    // 14.14改: '[ ... ] :=' builds an entry under a key that is not a name.
    LHAT_TEST("a computed key lands where it says");
    run_text(&r, "var^ t = { [0] := 7 }\nreturn^ t[0]\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("and the key is an ordinary expression");
    run_text(&r,
             "var^ k = 3\n"
             "var^ t = { [k + 1] := 5 }\n"
             "return^ t[4]\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    // 14 章 follows Lua here: t.a and t["a"] are one key.
    LHAT_TEST("a string key and a name are the same entry");
    run_text(&r, "var^ t = { [\"a\"] := 1 }\nreturn^ t.a\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("and it reads back the other way round");
    run_text(&r, "var^ t = { a := 2 }\nreturn^ t[\"a\"]\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    // A keyed entry takes no place in the sequence, so the positional ones
    // carry on counting past it.
    LHAT_TEST("a keyed entry takes no position from the sequence");
    run_text(&r,
             "var^ t = { 10, [\"k\"] := 20, 30 }\n"
             "return^ t[2]\n");
    CHECK_INTEGER(&r, 30);
    run_dispose(&r);

    LHAT_TEST("and is still reachable by its key");
    run_text(&r,
             "var^ t = { 10, [\"k\"] := 20, 30 }\n"
             "return^ t[\"k\"]\n");
    CHECK_INTEGER(&r, 20);
    run_dispose(&r);

    // 04 の 11.3: what the checker cannot see coming, the machine refuses.
    LHAT_TEST("a key that turns out to be nil^ is a fault");
    run_text(&r,
             "var^ x : number^|nil^ = nil^\n"
             "var^ t = { [x] := 1 }\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_KEY);
    run_dispose(&r);

    LHAT_TEST("repeat^ on its own needs break^ to end");
    run_text(&r,
             "var^ i = 0\n"
             "repeat^ { i := i + 1 if^ i = 3 { break^ } }\n"
             "return^ i\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // Or a return^, which leaves the body and the loop with it. The checker
    // reads this as a body whose end is unreachable (03 の 3.4), so what it
    // blesses has to run.
    LHAT_TEST("a return^ leaves an endless repeat^ too");
    run_text(&r,
             "var^ f = f^ -> number^ { repeat^ { return^ 7 } }\n"
             "return^ f()\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("break^ leaves only the loop it is in");
    run_text(&r,
             "var^ n = 0\n"
             "repeat^ 3 {\n"
             "  repeat^ 3 { n := n + 1 break^ }\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 9.8: one loop to leave and none around it is the same mistake as
    // naming more than are there, so it is the same diagnostic -- not the
    // "does not compile yet" a form still waiting on the compiler gets.
    LHAT_TEST("break^ outside a loop is a break^ reaching too far");
    run_text(&r, "break^\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_BREAK_TOO_FAR);
    run_dispose(&r);

    // 02 の 9.8: the hats count the loops to leave, so break^ is one and
    // break^^ is two. Written this way the level is read off the word
    // itself, which is why a plain break^ needs nothing added to it.
    LHAT_TEST("break^^ leaves two loops");
    run_text(&r,
             "var^ n = 0\n"
             "repeat^ 3 {\n"
             "  repeat^ 3 { n := n + 1 break^^ }\n"
             "  n := n + 10\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 02 の 9.8: the bracketed form says the same number, so the two
    // spellings are one thing and neither is the primary one.
    LHAT_TEST("and break^[2] says the same");
    run_text(&r,
             "var^ n = 0\n"
             "repeat^ 3 {\n"
             "  repeat^ 3 { n := n + 1 break^[2] }\n"
             "  n := n + 10\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("three loops and break^^^ leaves all of them");
    run_text(&r,
             "var^ n = 0\n"
             "repeat^ 3 {\n"
             "  repeat^ 3 {\n"
             "    repeat^ 3 { n := 7 break^^^ }\n"
             "    n := n + 100\n"
             "  }\n"
             "  n := n + 10\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // Asking for more loops than stand here is written down wrong rather
    // than quietly clamped to the outermost one.
    LHAT_TEST("a level past the loops that are there is refused by name");
    run_text(&r, "repeat^ 3 { break^^^ }\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_BREAK_TOO_FAR);
    run_dispose(&r);

    LHAT_TEST("and one loop too many is enough to be too far");
    run_text(&r, "repeat^ 3 { repeat^ 3 { repeat^ 3 { break^^^^ } } }\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_BREAK_TOO_FAR);
    run_dispose(&r);

    LHAT_TEST("the bracketed spelling is refused the same way");
    run_text(&r, "repeat^ 3 { repeat^ 3 { break^[3] } }\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_BREAK_TOO_FAR);
    run_dispose(&r);

    // 9.8: the loop the level names ends the way it would have ended on its
    // own, so its epilog^ runs. The ones passed through are left rather than
    // ended, and theirs do not.
    LHAT_TEST("the loop named ends normally, the ones passed through do not");
    run_text(&r,
             "var^ log = { s := \"\" }\n"
             "repeat^ 1 {\n"
             "  repeat^ 1 {\n"
             "    break^^\n"
             "  epilog^:\n"
             "    log.s := log.s .. \"in,\"\n"
             "  }\n"
             "epilog^:\n"
             "  log.s := log.s .. \"out,\"\n"
             "}\n"
             "return^ log.s\n");
    CHECK_STRING(&r, "out,");
    run_dispose(&r);

    LHAT_TEST("and one level runs both, being the inner loop's own end");
    run_text(&r,
             "var^ log = { s := \"\" }\n"
             "repeat^ 1 {\n"
             "  repeat^ 1 {\n"
             "    break^\n"
             "  epilog^:\n"
             "    log.s := log.s .. \"in,\"\n"
             "  }\n"
             "epilog^:\n"
             "  log.s := log.s .. \"out,\"\n"
             "}\n"
             "return^ log.s\n");
    CHECK_STRING(&r, "in,out,");
    run_dispose(&r);

    // 9.8 with 10.7: what a loop is only passed through still has to give
    // back what it took, so its finally^ runs where its epilog^ does not.
    LHAT_TEST("a finally^ passed through still runs");
    run_text(&r,
             "var^ log = { s := \"\" }\n"
             "repeat^ 1 {\n"
             "  repeat^ 1 {\n"
             "    break^^\n"
             "  finally^:\n"
             "    log.s := log.s .. \"fin,\"\n"
             "  }\n"
             "epilog^:\n"
             "  log.s := log.s .. \"out,\"\n"
             "}\n"
             "return^ log.s\n");
    CHECK_STRING(&r, "fin,out,");
    run_dispose(&r);

    // 16.5: an endless repeat^ ends the statements around it unless
    // something leaves it, and a break^ from inside a nested loop is one --
    // which is what the level is for.
    LHAT_TEST("a level reaching out of an endless repeat^ is a way out");
    run_text(&r,
             "var^ n = 0\n"
             "repeat^ {\n"
             "  repeat^ 3 { n := 5 break^^ }\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);
}

// 16.1: for^ is where the value being looked at is defined. Whether it
// repeats is up to the clause that follows.
static void test_for(void)
{
    Run r;

    LHAT_TEST("to^ counts up and includes the limit");
    run_text(&r,
             "var^ total = 0\n"
             "for^ i from^ 1 to^ 4 { total := total + i }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    // 16.4: the direction is not inferred, so this is empty rather than
    // counting down.
    LHAT_TEST("a limit below the start runs none");
    run_text(&r,
             "var^ x = 0\n"
             "for^ i from^ 1 to^ 0 { x := x + 1 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    LHAT_TEST("downto^ counts down");
    run_text(&r,
             "var^ seen = 0\n"
             "for^ i from^ 3 downto^ 1 { seen := seen * 10 + i }\n"
             "return^ seen\n");
    CHECK_INTEGER(&r, 321);
    run_dispose(&r);

    // 16.4: step^ is a positive amount either way; the sign belongs to the
    // clause.
    LHAT_TEST("step^ is a positive amount for both directions");
    run_text(&r,
             "var^ up = 0\n"
             "for^ i from^ 1 to^ 9 step^ 3 { up := up * 10 + i }\n"
             "return^ up\n");
    CHECK_INTEGER(&r, 147);
    run_dispose(&r);

    run_text(&r,
             "var^ down = 0\n"
             "for^ i from^ 9 downto^ 1 step^ 3 { down := down * 10 + i }\n"
             "return^ down\n");
    CHECK_INTEGER(&r, 963);
    run_dispose(&r);

    // 16.4: the bound says how far the loop goes, so it is read before the
    // loop starts and cannot move while it runs.
    LHAT_TEST("the bound is read once");
    run_text(&r,
             "var^ n = 3\n"
             "var^ x = 0\n"
             "for^ i from^ 1 to^ n { n := 100 x := x + 1 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    LHAT_TEST("and downto^ reads its bound once too");
    run_text(&r,
             "var^ n = 1\n"
             "var^ x = 0\n"
             "for^ i from^ 3 downto^ n { n := -100 x := x + 1 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    LHAT_TEST("and step^ is read once as well");
    run_text(&r,
             "var^ s = 1\n"
             "var^ n = 0\n"
             "for^ i from^ 1 to^ 9 step^ s { n := n + 1 s := 3 }\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 9);  // reading s each time round would give three
    run_dispose(&r);

    LHAT_TEST("10 to^ 1 step^ 2 is empty rather than confusing");
    run_text(&r,
             "var^ x = 0\n"
             "for^ i from^ 10 to^ 1 step^ 2 { x := x + 1 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 16.2: a focus with no name written is still a focus.
    LHAT_TEST("an unnamed focus is reached through it^");
    run_text(&r,
             "var^ total = 0\n"
             "for^ 1 to^ 4 { total := total + it^ }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    LHAT_TEST("while^ tests before the body and next^ runs after it");
    run_text(&r,
             "var^ total = 0\n"
             "for^ var^ i := 1 while^ i ≦ 4 next^ i := i + 1 { total := total + i }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    LHAT_TEST("until^ is the same the other way round");
    run_text(&r,
             "var^ total = 0\n"
             "for^ var^ i := 1 until^ i > 4 next^ i := i + 1 { total := total + i }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    LHAT_TEST("the focus is gone after the loop");
    run_text(&r, "for^ i from^ 1 to^ 2 { }\nreturn^ i\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    LHAT_TEST("loops nest");
    run_text(&r,
             "var^ n = 0\n"
             "for^ i from^ 1 to^ 3 {\n"
             "  for^ j from^ 1 to^ 4 { n := n + 1 }\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    // 16.3: `in^ e` asks e for the coroutine to walk. A table answers with
    // one over its keys, so the dense part comes back in index order.
    LHAT_TEST("in^ walks a table's dense part in order");
    run_text(&r,
             "var^ t = { 10, 20, 30 }\n"
             "var^ seen = 0\n"
             "for^ k, v in^ t { seen := seen * 100 + k * 10 + v // 10 }\n"
             "return^ seen\n");
    CHECK_INTEGER(&r, 112233);
    run_dispose(&r);

    LHAT_TEST("and reaches the keyed part too");
    run_text(&r,
             "var^ t = { a := 5, b := 7 }\n"
             "var^ total = 0\n"
             "for^ k, v in^ t { total := total + v }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    LHAT_TEST("an empty table walks no turns");
    run_text(&r,
             "var^ t = { }\n"
             "var^ n = 0\n"
             "for^ k, v in^ t { n := n + 1 }\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 13.10: one name takes the value whole, several take it apart by
    // position. in^ is the marker, so no other mark is needed.
    LHAT_TEST("one name takes what was yielded whole");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 yield^ 2 yield^ 3 }\n"
             "var^ total = 0\n"
             "for^ v in^ gen() { total := total + v }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 6);
    run_dispose(&r);

    LHAT_TEST("a coroutine answers iterate with itself");
    run_text(&r,
             "var^ gen = p^ { yield^ 4 yield^ 5 }\n"
             "var^ c = gen()\n"
             "var^ total = 0\n"
             "for^ v in^ c { total := total + v }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    // Anything with an iterate answers, which is what makes the rule a
    // convention rather than a special case for tables.
    LHAT_TEST("a definition answers by writing iterate");
    run_text(&r,
             "var^ Range = def^{\n"
             "  self^{ upto := 0 },\n"
             "  override^new := f^ n { return^ self^{ upto := n } },\n"
             "  iterate^ := f^self^ {\n"
             "    var^ limit = self^.upto\n"
             "    return^ p^ { for^ i from^ 1 to^ limit { yield^ i } }()\n"
             "  },\n"
             "}\n"
             "var^ total = 0\n"
             "for^ v in^ Range.new(4) { total := total + v }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    LHAT_TEST("a written iterate wins over the built-in one");
    run_text(&r,
             "var^ t = { 1, 2, 3, iterate^ := f^ { return^ p^ { yield^ 9 }() } }\n"
             "var^ total = 0\n"
             "for^ v in^ t { total := total + v }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    // 16.3改: what in^ asks for is the hat spelling, so a bare iterate on a
    // table the writer wrote is a member like any other and the walk is
    // still the built-in one.
    LHAT_TEST("a bare iterate on a plain table is not what in^ asks for");
    run_text(&r,
             "var^ t = { 1, 2, 3, iterate := f^ { return^ p^ { yield^ 9 }() } }\n"
             "var^ total = 0\n"
             "for^ k, v in^ t { if^ v isa^ number^ { total := total + v } }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 6);
    run_dispose(&r);

    // 16.3 puts a table's iterate() on the same footing as any other
    // coroutine, so it is drivable by hand and not only by the loop -- which
    // emits BC_RESUME rather than going through the members. The answer is
    // the tuple every walk yields, so a checked call reserves its width even
    // when discarding it, and stepping past elements works.
    LHAT_TEST("a walk taken by hand starts like any other coroutine");
    run_checked_text(&r,
                     "var^ t = { 10 }\n"
                     "var^ w = t.iterate^()\n"
                     "w.start()\n"
                     "if^ w.done() { return^ 0 }\n"
                     "w.resume(nil^)\n"
                     "if^ w.done() { return^ 1 }\n"
                     "return^ 2\n");
    CHECK_INTEGER(&r, 1);  // one element taken, then the walk ended
    run_dispose(&r);

    // An unchecked compile reserved one slot, and the pair coming back has
    // nowhere to land -- 03 の 5.3's mismatch, dropped rather than
    // reconciled.
    LHAT_TEST("a one-slot hand drive is refused when the pair arrives");
    run_text(&r,
             "var^ t = { 10, 20 }\n"
             "var^ w = t.iterate^()\n"
             "var^ pair = w.start()\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TUPLE_UNEXPECTED);
    run_dispose(&r);

    // 15.2 applies to a walk unchanged: nothing about having no body makes
    // the first resume mean something on its own.
    LHAT_TEST("resuming a walk that has not started is a fault");
    run_text(&r,
             "var^ t = { 10 }\n"
             "var^ w = t.iterate^()\n"
             "w.resume(nil^)\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_COROUTINE_NOT_STARTED);
    run_dispose(&r);

    LHAT_TEST("starting a walk twice is a fault");
    run_checked_text(&r,
                     "var^ t = { 10, 20 }\n"
                     "var^ w = t.iterate^()\n"
                     "w.start()\n"
                     "w.start()\n"
                     "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_COROUTINE_ALREADY_STARTED);
    run_dispose(&r);

    // 10.7: a walk has nothing pending, so disposal is only the state.
    LHAT_TEST("a walk in progress can be disposed");
    run_checked_text(&r,
                     "var^ t = { 10, 20, 30 }\n"
                     "var^ w = t.iterate^()\n"
                     "w.start()\n"
                     "w.dispose()\n"
                     "return^ w.done()\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // 16.3改2: keys^ and values^ are the two projections of that same walk,
    // reached without a call being written. The order is the walk's -- the
    // dense half by index, then the keyed half -- so the nth key and the nth
    // value are the two halves of one step.
    LHAT_TEST("keys^ and values^ walk the whole table");
    run_checked_text(&r,
                     "var^ t = { 10, 20, a := 9 }\n"
                     "var^ ks = \"\"\n"
                     "for^ k in^ t.keys^ { ks := ks .. k.tostring^() }\n"
                     "var^ vs = \"\"\n"
                     "for^ v in^ t.values^ { vs := vs .. v.tostring^() }\n"
                     "return^ ks .. \"|\" .. vs\n");
    CHECK_STRING(&r, "12a|10209");
    run_dispose(&r);

    // The two projections step together: reading them side by side gives the
    // pairs iterate^ would have handed over.
    LHAT_TEST("and step in the same order");
    run_checked_text(&r,
                     "var^ t = { 10, 20, a := 9 }\n"
                     "var^ vs = t.values^\n"
                     "var^ v = vs.start()\n"
                     "var^ out = \"\"\n"
                     "for^ k in^ t.keys^ {\n"
                     "  out := out .. k.tostring^() .. \"=\" ..\n"
                     "         (v ?? 0).tostring^() .. \" \"\n"
                     "  v := vs.resume(nil^)\n"
                     "}\n"
                     "return^ out\n");
    CHECK_STRING(&r, "1=10 2=20 a=9 ");
    run_dispose(&r);

    // 16.3: the single-name loop is the sequence reading and stays what it
    // was -- the dense half alone. values^ is the mapping reading.
    LHAT_TEST("the single-name loop is still the dense half alone");
    run_checked_text(&r,
                     "var^ t = { 10, 20, a := 9 }\n"
                     "var^ n = 0\n"
                     "for^ v in^ t { n := n + v }\n"
                     "return^ n\n");
    CHECK_INTEGER(&r, 30);
    run_dispose(&r);

    LHAT_TEST("a projection over an empty table takes no turn");
    run_checked_text(&r,
                     "var^ t = { }\n"
                     "var^ n = 0\n"
                     "for^ k in^ t.keys^ { n := n + 1 }\n"
                     "return^ n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 16.3改2 with 14.18: the hat is not optional on a table, so the bare
    // word is a member like any other and holds nothing until written.
    LHAT_TEST("a bare keys on a table is the writer's name");
    run_text(&r, "var^ t = { 1 }\nreturn^ t.keys isa^ nil^\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("and a written keys^ is what answers");
    run_text(&r,
             "var^ t = { 1, 2, keys^ := 7 }\n"
             "return^ t.keys^\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("break^ leaves a walk like any other loop");
    run_text(&r,
             "var^ t = { 1, 2, 3, 4 }\n"
             "var^ n = 0\n"
             "for^ k, v in^ t { n := n + 1 if^ n = 2 { break^ } }\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    LHAT_TEST("the clauses of 9 章 apply to a walk too");
    run_text(&r,
             "var^ t = { 1, 2, 3 }\n"
             "var^ log = { s := 0 }\n"
             "for^ k, v in^ t {\n"
             "  main^:\n"
             "    log.s := log.s + v\n"
             "  epilog^:\n"
             "    log.s := log.s * 10\n"
             "}\n"
             "return^ log.s\n");
    CHECK_INTEGER(&r, 60);
    run_dispose(&r);

    LHAT_TEST("the focus is gone after the walk");
    run_text(&r, "var^ t = { 1 }\nfor^ k, v in^ t { }\nreturn^ v\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    LHAT_TEST("walking something with no iterate is refused at run time");
    run_text(&r, "var^ n = 1\nfor^ v in^ n { }\nreturn^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    // 16.3 and 16.1: this one does not repeat. It is the do^ block written
    // without the extra nesting.
    LHAT_TEST("if^ uses the focus once and does not repeat");
    run_text(&r,
             "var^ x = 0\n"
             "for^ var^ i := 1, var^ j := 2 if^ i + j < 10 { x := i + j }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    LHAT_TEST("and its focus does not escape either");
    run_text(&r, "for^ var^ i := 1 if^ i > 0 { }\nreturn^ i\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    // 5.2: '{' opens the statement form and ':' the expression one, here as
    // anywhere -- 16.1 has for^ take the form its clause does.
    LHAT_TEST("and it answers a value when written with ':'");
    run_text(&r,
             "var^ x = for^ var^ i := 1, var^ j := 2 if^ i + j < 10: i + j el^: 0 ;\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 16.7 is not against it: the name still does not leave, only the value
    // built from it, which is what an expression is.
    LHAT_TEST("and the focus still does not escape");
    run_text(&r,
             "var^ x = for^ var^ i := 1 if^ i > 0: i el^: 0 ;\n"
             "return^ i\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    // 16.3: do^: makes the focus and answers with what follows it. 13.8改
    // keeps a tuple out of an argument list, so this is the shape that opens
    // one straight into the next call.
    // 16.3: do^: makes the focus and answers with what follows it. 13.8改
    // keeps a tuple out of an argument list, so this is the shape that opens
    // one straight into the next call. The width comes off the signature, so
    // these run the checker first as every other tuple test does.
    LHAT_TEST("do^: answers with its body");
    run_checked_text(&r,
                     "var^ f = f^ -> (number^, number^) { return^ 3, 4 }\n"
                     "var^ g = f^ a:number^, b:number^ -> number^ "
                     "{ return^ a * b }\n"
                     "var^ x = for^ let^ p, q = f() do^: g(p, q);\n"
                     "return^ x\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    // Each focus is scoped to the one after it and to the body.
    LHAT_TEST("a run of for^ opens several at once");
    run_checked_text(&r,
                     "var^ f = f^ -> (number^, number^) { return^ 3, 4 }\n"
                     "var^ x = for^ let^ p, q = f()\n"
                     "         for^ let^ n = p + q\n"
                     "         do^: n * 2;\n"
                     "return^ x\n");
    CHECK_INTEGER(&r, 14);
    run_dispose(&r);

    LHAT_TEST("and a run may end on an if^ expression");
    run_checked_text(&r,
                     "var^ f = f^ -> (number^, number^) { return^ 3, 4 }\n"
                     "var^ x = for^ let^ p, q = f()\n"
                     "         for^ let^ n = p + q\n"
                     "         if^ n > 5: n el^: 0;\n"
                     "return^ x\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("a run of for^ works as a statement too");
    run_text(&r,
             "var^ x = 0\n"
             "for^ let^ a = 1 for^ let^ b = 2 if^ a < b { x := a + b }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    LHAT_TEST("and the focus of a do^: does not escape either");
    run_text(&r,
             "var^ x = for^ let^ p = 3 do^: p + 1;\n"
             "return^ p\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    // 14 章 の table with 16: what a loop is mostly for.
    LHAT_TEST("a loop fills a table");
    run_text(&r,
             "var^ t = { }\n"
             "for^ i from^ 1 to^ 4 { t[i] := i * i }\n"
             "return^ t.3\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);
}

// 9 章: the five clauses, and 9.8's three strengths of exit.
static void test_loop_clauses(void)
{
    Run r;

    // 9.4: what prolog^ declares lives as long as the loop, without leaking
    // out of it.
    LHAT_TEST("prolog^ runs once and its names last the whole loop");
    run_text(&r,
             "var^ out = 0\n"
             "repeat^ 4 {\n"
             "  prolog^:\n"
             "    var^ total = 0\n"
             "  main^:\n"
             "    total := total + 1\n"
             "  epilog^:\n"
             "    out := total\n"
             "}\n"
             "return^ out\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);

    // 9.1: prolog^ runs whether the condition ever holds or not.
    LHAT_TEST("prolog^ and epilog^ run even when the body does not");
    run_text(&r,
             "var^ seen = 0\n"
             "repeat^ 0 {\n"
             "  prolog^:\n"
             "    seen := seen + 1\n"
             "  main^:\n"
             "    seen := seen + 100\n"
             "  epilog^:\n"
             "    seen := seen + 10\n"
             "}\n"
             "return^ seen\n");
    CHECK_INTEGER(&r, 11);
    run_dispose(&r);

    // 9.10: pre^ runs at the head of every turn, before the condition is
    // tested -- so the body runs once however the condition comes out. This
    // is the shape C spells do ... while.
    LHAT_TEST("pre^ runs even when the condition never holds");
    run_text(&r,
             "var^ n = 0\n"
             "repeat^ while^ false^ {\n"
             "  pre^:\n"
             "    n := n + 1\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("where main^ under the same condition never does");
    run_text(&r,
             "var^ n = 0\n"
             "repeat^ while^ false^ {\n"
             "  main^:\n"
             "    n := n + 1\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // With both, the test sits between them: pre^ takes one more turn than
    // main^, since the turn whose test failed still ran its pre^.
    LHAT_TEST("pre^ and main^ straddle the condition");
    run_text(&r,
             "var^ n = 0\n"
             "var^ i = 0\n"
             "repeat^ while^ i < 3 {\n"
             "  pre^:\n"
             "    n := n + 10\n"
             "    i := i + 1\n"
             "  main^:\n"
             "    n := n + 1\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 32);  // three pre^, two main^
    run_dispose(&r);

    LHAT_TEST("premain^ is the same clause");
    run_text(&r,
             "var^ n = 0\n"
             "repeat^ while^ false^ {\n"
             "  premain^:\n"
             "    n := n + 7\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // 9.1 is unchanged: first^ and last^ belong to the turns the condition
    // accepted, and pre^ does not make one of those.
    LHAT_TEST("a turn that only ran pre^ is not one first^ or last^ counts");
    run_text(&r,
             "var^ n = 0\n"
             "repeat^ while^ false^ {\n"
             "  pre^:\n"
             "    n := n + 1\n"
             "  first^:\n"
             "    n := n + 100\n"
             "  last^:\n"
             "    n := n + 1000\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 9.8: break^ leaves from inside pre^ like from anywhere else.
    LHAT_TEST("break^ leaves a loop from inside pre^");
    run_text(&r,
             "var^ n = 0\n"
             "repeat^ {\n"
             "  pre^:\n"
             "    n := n + 1\n"
             "    if^ n = 3 { break^ }\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // The declaration goes in prolog^, which is what makes reading before the
    // test writable at all -- 9.4 gives prolog^ names the whole loop.
    LHAT_TEST("prolog^ and pre^ together are the loop-and-a-half");
    run_text(&r,
             "var^ out = 0\n"
             "var^ i = 0\n"
             "repeat^ while^ i < 3 {\n"
             "  prolog^:\n"
             "    var^ seen = 0\n"
             "  pre^:\n"
             "    i := i + 1\n"
             "  main^:\n"
             "    seen := seen + i\n"
             "  epilog^:\n"
             "    out := seen\n"
             "}\n"
             "return^ out\n");
    CHECK_INTEGER(&r, 3);  // 1 + 2; the turn that read 3 failed the test
    run_dispose(&r);

    LHAT_TEST("first^ runs at the head of the first iteration only");
    run_text(&r,
             "var^ n = 0\n"
             "repeat^ 3 {\n"
             "  first^:\n"
             "    n := n + 100\n"
             "  main^:\n"
             "    n := n + 1\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 103);
    run_dispose(&r);

    // 9.1: first^ and last^ do not run when the condition never holds.
    LHAT_TEST("first^ and last^ stay away when nothing ran");
    run_text(&r,
             "var^ n = 0\n"
             "repeat^ 0 {\n"
             "  first^:\n"
             "    n := n + 1\n"
             "  main^:\n"
             "    n := n + 1\n"
             "  last^:\n"
             "    n := n + 1\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 9.7: the whole reason last^ keeps a copy. Naively it would see 5.
    LHAT_TEST("last^ sees the value the condition last accepted");
    run_text(&r,
             "var^ seen = 0\n"
             "for^ i from^ 1 to^ 4 {\n"
             "  last^:\n"
             "    seen := i\n"
             "}\n"
             "return^ seen\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);

    // 9.8: break^ is a normal end, so last^ and epilog^ both run, and the
    // copy taken at the head of the iteration is the right one.
    LHAT_TEST("break^ leaves last^ looking at the current iteration");
    run_text(&r,
             "var^ seen = 0\n"
             "for^ i from^ 1 to^ 10 {\n"
             "  main^:\n"
             "    if^ i = 5 { break^ }\n"
             "  last^:\n"
             "    seen := i\n"
             "}\n"
             "return^ seen\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    LHAT_TEST("break^ runs epilog^ too");
    run_text(&r,
             "var^ n = 0\n"
             "repeat^ 10 {\n"
             "  main^:\n"
             "    break^\n"
             "  epilog^:\n"
             "    n := 7\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // 9.8: return^ is not a normal end for the loop, so neither runs.
    LHAT_TEST("return^ runs neither last^ nor epilog^");
    run_text(&r,
             "var^ out = { n := 0 }\n"
             "var^ go = f^ {\n"
             "  repeat^ 3 {\n"
             "    main^:\n"
             "      return^ 1\n"
             "    last^:\n"
             "      out.n := out.n + 10\n"
             "    epilog^:\n"
             "      out.n := out.n + 100\n"
             "  }\n"
             "  return^ 0\n"
             "}\n"
             "go()\n"
             "return^ out.n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 9.4: a name made in main^ lives one iteration, so the next one starts
    // over rather than seeing the last.
    LHAT_TEST("what main^ declares lasts one iteration");
    run_text(&r,
             "var^ out = 0\n"
             "repeat^ 3 {\n"
             "  var^ each = 0\n"
             "  each := each + 1\n"
             "  out := each\n"
             "}\n"
             "return^ out\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("the clauses of a loop do not leak out of it");
    run_text(&r,
             "repeat^ 1 {\n"
             "  prolog^:\n"
             "    var^ total = 0\n"
             "  main^:\n"
             "    total := 1\n"
             "}\n"
             "return^ total\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    // 5.4 with 9.4: the focus is one place per loop, not per iteration, so a
    // closure made inside sees where it ended up.
    LHAT_TEST("a closure made in a loop captures the place, not the moment");
    run_text(&r,
             "var^ get = f^ { return^ 0 }\n"
             "for^ i from^ 1 to^ 3 {\n"
             "  get := f^ { return^ i }\n"
             "}\n"
             "return^ get()\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);
}

// 02 の 17 章. 17.9 makes a match sugar over an if-chain, so what is worth
// pinning is that the chain comes out right -- not the instructions.
static void test_patterns(void)
{
    Run r;

    LHAT_TEST("a value pattern picks its arm");
    run_text(&r,
             "var^ n = 2\n"
             "var^ x = 0\n"
             "for^ n { when^ 1: x := 10 when^ 2: x := 20 other^: x := 30 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 20);
    run_dispose(&r);

    LHAT_TEST("other^ takes what is left");
    run_text(&r,
             "var^ n = 9\n"
             "var^ x = 0\n"
             "for^ n { when^ 1: x := 10 other^: x := 30 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 30);
    run_dispose(&r);

    // 17.3: to^ includes both ends, as 16.4 has it.
    LHAT_TEST("a range pattern includes both ends");
    run_text(&r,
             "var^ seen = 0\n"
             "for^ i from^ 1 to^ 5 {\n"
             "  for^ i { when^ 2 to^ 4: seen := seen + 1 other^: }\n"
             "}\n"
             "return^ seen\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 17.3: several on one arm, which 17.9 makes an or^.
    LHAT_TEST("patterns separated by commas share an arm");
    run_text(&r,
             "var^ hits = 0\n"
             "for^ i from^ 1 to^ 6 {\n"
             "  for^ i { when^ 2, 3, 5: hits := hits + 1 other^: }\n"
             "}\n"
             "return^ hits\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 17.2: the subject is evaluated once, which is the point of for^ being
    // where a focus is defined (16.1).
    LHAT_TEST("the subject is evaluated once");
    run_text(&r,
             "var^ calls = { n := 0 }\n"
             "var^ get = f^ { calls.n := calls.n + 1 return^ 2 }\n"
             "for^ get() { when^ 1: when^ 2: when^ 3: other^: }\n"
             "return^ calls.n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 16.2 and 17.2: unnamed, the subject is it^; named, it is the name.
    LHAT_TEST("the subject is reached through it^");
    run_text(&r,
             "var^ x = 0\n"
             "for^ 7 { when^ 7: x := it^ other^: }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("and through a name when one is written");
    run_text(&r,
             "var^ x = 0\n"
             "for^ var^ n := 7 { when^ 7: x := n other^: }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("the subject is gone after the match");
    run_text(&r, "for^ var^ n := 1 { when^ 1: other^: }\nreturn^ n\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    // 8.6改・16.3改: without var^, the subject's name reaches an existing
    // outer one instead of a fresh one -- the pattern itself still has to
    // compile against it, not just the body.
    LHAT_TEST("a bare name reuses an outer subject, patterns included");
    run_text(&r, "var^ n = 7\nfor^ n := 7 { when^ 7: n := 100 other^: n := 200 }\nreturn^ n\n");
    CHECK_INTEGER(&r, 100);
    run_dispose(&r);

    // 17.4: a type pattern writes isa^, since a bare name could be either.
    LHAT_TEST("a type pattern uses isa^");
    run_text(&r,
             "errordef^ E { A, B }\n"
             "var^ x = 0\n"
             "for^ error^E.B{ } {\n"
             "  when^ isa^ E.A: x := 1\n"
             "  when^ isa^ E.B: x := 2\n"
             "  other^: x := 3\n"
             "}\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    // 17.2 の 式形. 17.6: only the ':' of for^ opens, and ';' closes it all.
    LHAT_TEST("the expression form answers a value");
    run_text(&r,
             "var^ n = 2\n"
             "var^ r = for^ n: when^ 1: 10 when^ 2: 20 other^: 30 ;\n"
             "return^ r\n");
    CHECK_INTEGER(&r, 20);
    run_dispose(&r);

    LHAT_TEST("and reaches its subject the same way");
    run_text(&r,
             "var^ r = for^ 3: when^ 1 to^ 2: 0 other^: it^ * 100 ;\n"
             "return^ r\n");
    CHECK_INTEGER(&r, 300);
    run_dispose(&r);

    // 17.8: no guards. The arm's body is where a further test goes.
    LHAT_TEST("a further test goes inside the arm");
    run_text(&r,
             "var^ x = 0\n"
             "for^ var^ n := 5 {\n"
             "  when^ 1 to^ 9:\n"
             "    if^ n > 3 { x := 1 else^: x := 2 }\n"
             "  other^:\n"
             "    x := 3\n"
             "}\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);
}

int main(void)
{
    test_repeat();
    test_for();
    test_loop_clauses();
    test_patterns();
    return lhat_test_report("test_vm_loop");
}
