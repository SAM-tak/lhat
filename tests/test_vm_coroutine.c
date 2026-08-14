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

// 02 の 15 章 and 5.11: a coroutine is one suspended frame, which is all
// 15.5 leaves room for.
static void test_coroutines(void)
{
    Run r;

    // 15.5: the call answers a coroutine and the body has not started.
    LHAT_TEST("calling a yieldable procedure runs nothing");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ gen = p^ { log.n := 1 yield^ 0 }\n"
             "var^ c = gen()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 15.5: not "up to the first yield^" -- the body starts at its top when
    // start() comes, so each side of a yield^ runs on its own turn.
    LHAT_TEST("the body starts at its top, not after the first yield^");
    run_text(&r,
             "var^ log = { s := 0 }\n"
             "var^ p = p^ {\n"
             "  log.s := log.s * 10 + 1\n"
             "  yield^\n"
             "  log.s := log.s * 10 + 2\n"
             "}\n"
             "var^ c = p()\n"
             "var^ made = log.s\n"
             "c.start()\n"
             "var^ first = log.s\n"
             "c.resume(nil^)\n"
             "return^ made * 10000 + first * 100 + log.s\n");
    CHECK_INTEGER(&r, 112);  // nothing, then 1, then 12
    run_dispose(&r);

    LHAT_TEST("starting runs the body up to the yield^");
    run_text(&r,
             "var^ gen = p^ { yield^ 7 }\n"
             "var^ c = gen()\n"
             "return^ c.start()\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("each resume carries on from where it stopped");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 yield^ 2 yield^ 3 }\n"
             "var^ c = gen()\n"
             "var^ a = c.start()\n"
             "var^ b = c.resume(nil^)\n"
             "return^ a * 10 + b\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    LHAT_TEST("the arguments of the call reach the body");
    run_text(&r,
             "var^ gen = p^n { yield^ n * 2 }\n"
             "var^ c = gen(21)\n"
             "return^ c.start()\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    // 15.4: bidirectional. Without this there is no await^ to build (15.4).
    LHAT_TEST("yield^ answers what the resume sent");
    run_text(&r,
             "var^ gen = p^ {\n"
             "  var^ got = yield^ 0\n"
             "  yield^ got + 1\n"
             "}\n"
             "var^ c = gen()\n"
             "c.start()\n"
             "return^ c.resume(41)\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    LHAT_TEST("the body keeps its own names between resumes");
    run_text(&r,
             "var^ counter = p^ {\n"
             "  var^ n = 0\n"
             "  repeat^ {\n"
             "    n := n + 1\n"
             "    yield^ n\n"
             "  }\n"
             "}\n"
             "var^ c = counter()\n"
             "c.start()\n"
             "c.resume(nil^)\n"
             "return^ c.resume(nil^)\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    LHAT_TEST("the last resume answers what the body returned");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 return^ 9 }\n"
             "var^ c = gen()\n"
             "c.start()\n"
             "return^ c.resume(nil^)\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    // 15.6改: and nil^ when the body has no return^ to answer with. That is
    // a value the resume really receives, not a stand-in for 03's "returns
    // nothing" -- 15.5 keeps the two apart by never letting the second one
    // reach a caller.
    LHAT_TEST("and nil^ when the body reached its end without one");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 }\n"
             "var^ c = gen()\n"
             "c.start()\n"
             "return^ c.resume(nil^)\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    LHAT_TEST("resuming one that has finished is a fault");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 }\n"
             "var^ c = gen()\n"
             "c.start()\n"
             "c.resume(nil^)\n"
             "c.resume(nil^)\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_DEAD_COROUTINE);
    run_dispose(&r);

    // 8.8 の the checker walks a body whether or not it runs, so a var^
    // on a path that is never taken puts a member in the type that the table
    // does not have. Written down as a test because the hole is known and
    // left open -- 03 の 5.1's checks are what catches it, at the use.
    LHAT_TEST("a path var^ that never runs leaves the member absent");
    run_text(&r,
             "var^ t = { }\n"
             "var^ never = p^ { var^ t.b = 1 }\n"
             "return^ t.b + 1\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_OK);
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    LHAT_TEST("and running it is what puts the member there");
    run_text(&r,
             "var^ t = { }\n"
             "var^ fill = p^ { var^ t.b = 1 }\n"
             "fill()\n"
             "return^ t.b + 1\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    // 05 の 8.6: one table per machine, made with it and rooted by it.
    LHAT_TEST("L^ answers the machine's own table");
    run_text(&r,
             "var^ L^.modules.ns1.mod1 = { greet := 7 }\n"
             "var^ L^.modules.ns1.mod2 = { x := 2 }\n"
             "return^ L^.modules.ns1.mod1.greet * 10 + L^.modules.ns1.mod2.x\n");
    CHECK_INTEGER(&r, 72);
    run_dispose(&r);

    // 8.6: what the registry holds is reachable from the machine, so a
    // collection run by hand does not take it.
    LHAT_TEST("and what it holds survives a collection");
    run_text(&r,
             "var^ L^.modules.ns1.mod1 = { greet := 7 }\n"
             "L^.collectgarbage()\n"
             "return^ L^.modules.ns1.mod1.greet\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // 05 の 8.6: check.c refuses what is written against L^ by name,
    // but a table reaches a p^ through a t^{ … } parameter, which carries no
    // mark of this -- the writer has no spelling for one. So the machine asks
    // again where the write happens. Compiled without the checker here, which
    // is the same thing that path amounts to.
    LHAT_TEST("L^ refuses a write that reached it through a parameter");
    run_text(&r,
             "var^ poke = p^ x { var^ x.zzz := 1 }\n"
             "poke(L^)\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_SEALED);
    run_dispose(&r);

    LHAT_TEST("but an ordinary table takes one");
    run_text(&r,
             "var^ poke = p^ x { var^ x.zzz := 1 }\n"
             "var^ t = { }\n"
             "poke(t)\n"
             "return^ t.zzz\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("and two machines do not share one");
    {
        Run one;
        compile_text(&one, "var^ L^.modules.a = { n := 1 }\nreturn^ 0\n");
        LhatMachine *first = lhat_machine_new();
        LhatMachine *second = lhat_machine_new();
        lhat_run(first, one.proto);
        Run two;
        compile_text(&two, "return^ L^.modules.a.n\n");
        LhatRunResult missing = lhat_run(second, two.proto);
        // 04 の 11.3 makes a key that is not there nil^, and reading through
        // it is where the machine says so.
        LHAT_CHECK_EQ_INT(missing.status, LHAT_RUN_TYPE_ERROR);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(first, two.proto).value), 1);
        lhat_machine_dispose(first);
        lhat_machine_dispose(second);
        compiled_dispose(&one);
        compiled_dispose(&two);
    }

    // 8.8: the tables on the way are made where the path does not reach one
    // yet, and left alone where it does.
    LHAT_TEST("var^ along a path makes the tables it needs");
    run_text(&r,
             "var^ a.b.c = 1\n"
             "return^ a.b.c\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("and a second path through one table does not replace it");
    run_text(&r,
             "var^ a.b.c = 1\n"
             "var^ a.b.d = 2\n"
             "var^ a.z = 3\n"
             "return^ a.b.c * 100 + a.b.d * 10 + a.z\n");
    CHECK_INTEGER(&r, 123);
    run_dispose(&r);

    LHAT_TEST("and a table already there keeps what it had");
    run_text(&r,
             "var^ t = { p := 4 }\n"
             "var^ t.q = 5\n"
             "return^ t.p * 10 + t.q\n");
    CHECK_INTEGER(&r, 45);
    run_dispose(&r);

    // 5.4: the root is reached, not shadowed, so a body writing one reaches
    // the place the enclosing frame holds.
    LHAT_TEST("and a nested body reaches the root it does not own");
    run_text(&r,
             "var^ outer = { }\n"
             "var^ add = p^ { var^ outer.k = 7 }\n"
             "add()\n"
             "return^ outer.k\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // 15.11: _yield^ makes the body a coroutine's without suspending it, so
    // start() runs the whole thing and finishes.
    LHAT_TEST("_yield^ does not suspend");
    run_text(&r,
             "var^ log = { s := 0 }\n"
             "var^ fake = p^ -> number^ {\n"
             "  log.s := 1\n"
             "  _yield^ 7\n"
             "  log.s := 2\n"
             "  return^ 9\n"
             "}\n"
             "var^ c = fake()\n"
             "var^ made = log.s\n"
             "var^ ended = c.start()\n"
             "return^ made * 1000 + log.s * 100 + ended\n");
    CHECK_INTEGER(&r, 209);  // nothing ran until start(), then all of it
    run_dispose(&r);

    LHAT_TEST("and the coroutine it answers is finished after start()");
    run_text(&r,
             "var^ fake = p^ { _yield^ 1 }\n"
             "var^ c = fake()\n"
             "var^ before = c.done()\n"
             "c.start()\n"
             "if^ before { return^ 0 }\n"
             "if^ c.done() { return^ 1 }\n"
             "return^ 2\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // What it would have sent is still worked out -- only the suspending is
    // dropped, so an expression written there still does what it does.
    LHAT_TEST("what it would have sent is still evaluated");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ bump = f^ -> number^ { log.n := log.n + 1  return^ log.n }\n"
             "var^ fake = p^ { _yield^ bump() }\n"
             "var^ c = fake()\n"
             "c.start()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // Nobody resumes it, so nothing comes back. The declared type says
    // otherwise, and 03 の 5.1's checks are what catches the writer whose
    // "this never runs" turned out to be wrong.
    LHAT_TEST("but nothing comes back from it");
    run_text(&r,
             "var^ fake = p^ -> string^ {\n"
             "  var^ got : string^ = _yield^ 1\n"
             "  return^ got .. \"!\"\n"
             "}\n"
             "var^ c = fake()\n"
             "c.start()\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    LHAT_TEST("two coroutines from one procedure are separate");
    run_text(&r,
             "var^ counter = p^ {\n"
             "  var^ n = 0\n"
             "  repeat^ { n := n + 1 yield^ n }\n"
             "}\n"
             "var^ a = counter()\n"
             "var^ b = counter()\n"
             "a.start()\n"
             "a.resume(nil^)\n"
             "return^ b.start()\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 15.5 again: the caller of a yieldable procedure need not be yieldable,
    // so nothing has to be marked on the way up.
    LHAT_TEST("an ordinary procedure may drive a coroutine");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 yield^ 2 }\n"
             "var^ sum = p^ {\n"
             "  var^ c = gen()\n"
             "  return^ c.start() + c.resume(nil^)\n"
             "}\n"
             "return^ sum()\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 02 の 10.7: a coroutine dropped while suspended still runs what is
    // pending, and 12.6 says dispose() is how that is asked for.
    LHAT_TEST("disposing runs the finally^ the body was inside");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := 5\n"
             "  }\n"
             "}\n"
             "var^ c = gen()\n"
             "c.start()\n"
             "c.dispose()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    // 10.7: the same finally^ is never run twice.
    LHAT_TEST("a finally^ already run is not run again at disposal");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := log.n + 1\n"
             "  }\n"
             "  yield^ 2\n"
             "}\n"
             "var^ c = gen()\n"
             "c.start()\n"
             "c.resume(nil^)\n"
             "c.dispose()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("disposing one that never started has nothing to run");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := 5\n"
             "  }\n"
             "}\n"
             "var^ c = gen()\n"
             "c.dispose()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    LHAT_TEST("a disposed coroutine cannot be resumed");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 yield^ 2 }\n"
             "var^ c = gen()\n"
             "c.start()\n"
             "c.dispose()\n"
             "c.resume(nil^)\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_DEAD_COROUTINE);
    run_dispose(&r);

    // 12.6: dispose() is what with^ calls, so a coroutine goes into a with^
    // like any other resource.
    LHAT_TEST("with^ disposes a coroutine at the end of the block");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := 3\n"
             "  }\n"
             "}\n"
             "with^ c = gen()\n"
             "{\n"
             "  c.start()\n"
             "}\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 10.7: there is nothing waiting to resume a coroutine being disposed.
    LHAT_TEST("yield^ during disposal is a fault");
    run_text(&r,
             "var^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    yield^ 2\n"
             "  }\n"
             "}\n"
             "var^ c = gen()\n"
             "c.start()\n"
             "c.dispose()\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_YIELD_OUTSIDE);
    run_dispose(&r);

    // 02 の 10.7: dispose() is how a program asks; the collector is what
    // happens when it never does. Dropping a suspended coroutine has to run
    // what is pending just the same, or a finally^ becomes a promise the
    // language keeps only when it is watched.
    LHAT_TEST("a coroutine dropped while suspended has its finally^ run");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := 5\n"
             "  }\n"
             "}\n"
             "var^ drop = p^ {\n"
             "  var^ c = gen()\n"
             "  c.start()\n"
             "}\n"
             "drop()\n"
             "L^.collectgarbage()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    // The cleanup is L^ code, so it cannot run inside the collection -- it
    // runs at the first instruction boundary after it. That boundary is
    // before the next statement, which is the part worth pinning: the delay
    // is real in the implementation and invisible to the program.
    LHAT_TEST("and has run by the next statement");
    run_text(&r,
             "var^ log = { n := 0, seen := 0 }\n"
             "var^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := 5\n"
             "  }\n"
             "}\n"
             "var^ drop = p^ {\n"
             "  var^ c = gen()\n"
             "  c.start()\n"
             "}\n"
             "drop()\n"
             "L^.collectgarbage()\n"
             "log.seen := log.n\n"
             "return^ log.seen\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    // 10.7 again: never twice. What the program already disposed of has
    // nothing left pending, so becoming unreachable afterwards is quiet.
    LHAT_TEST("one already disposed of is not run again by the collector");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := log.n + 1\n"
             "  }\n"
             "}\n"
             "var^ drop = p^ {\n"
             "  var^ c = gen()\n"
             "  c.start()\n"
             "  c.dispose()\n"
             "}\n"
             "drop()\n"
             "L^.collectgarbage()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 10.7: and the collector does not run one twice either. Nothing marks
    // a coroutine as already done -- what does the work is that its count
    // of pending cleanups is 0 afterwards, so every later collection walks
    // past it. Asked with two collections after the drop.
    LHAT_TEST("the collector does not run the same one on a later cycle");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := log.n + 1\n"
             "  }\n"
             "}\n"
             "var^ drop = p^ {\n"
             "  var^ c = gen()\n"
             "  c.start()\n"
             "}\n"
             "drop()\n"
             "L^.collectgarbage()\n"
             "L^.collectgarbage()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // One that ran its body to the end has nothing pending either, and one
    // that never started never pushed anything.
    LHAT_TEST("one that finished on its own is quiet at the collection");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := log.n + 1\n"
             "  }\n"
             "}\n"
             "var^ drop = p^ {\n"
             "  var^ c = gen()\n"
             "  c.start()\n"
             "  c.resume(nil^)\n"
             "}\n"
             "drop()\n"
             "L^.collectgarbage()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("one dropped before it started has nothing to run");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := 5\n"
             "  }\n"
             "}\n"
             "var^ drop = p^ { var^ c = gen() }\n"
             "drop()\n"
             "L^.collectgarbage()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // The queue takes one per instruction boundary, so several dropped at
    // once all run -- innermost cleanup of each, in the order they come off
    // the list. Only the count is asserted; the order between coroutines is
    // the collector's business and 10.7 promises nothing about it.
    LHAT_TEST("several dropped at once all run");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := log.n + 1\n"
             "  }\n"
             "}\n"
             "var^ drop = p^ {\n"
             "  var^ a = gen()  a.start()\n"
             "  var^ b = gen()  b.start()\n"
             "  var^ c = gen()  c.start()\n"
             "}\n"
             "drop()\n"
             "L^.collectgarbage()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 10.7: the cleanups run innermost first here as anywhere else -- the
    // collector picks when, not what.
    LHAT_TEST("a dropped one's nested cleanups run innermost first");
    run_text(&r,
             "var^ log = { s := 0 }\n"
             "var^ gen = p^ {\n"
             "  do^{\n"
             "    do^{\n"
             "      yield^ 1\n"
             "    finally^:\n"
             "      log.s := log.s * 10 + 1\n"
             "    }\n"
             "  finally^:\n"
             "    log.s := log.s * 10 + 2\n"
             "  }\n"
             "}\n"
             "var^ drop = p^ {\n"
             "  var^ c = gen()\n"
             "  c.start()\n"
             "}\n"
             "drop()\n"
             "L^.collectgarbage()\n"
             "return^ log.s\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    // 10.7: dropped too late for any collection to have noticed -- nothing
    // here fills the heap, so without a collection at the end of the run
    // this finally^ would never run at all. No statement can watch it
    // happen, since the answer was settled before it ran, so the table the
    // run answers with is read from here.
    LHAT_TEST("one dropped too late for a collection still runs at the end");
    run_text(&r,
             "var^ log = { 0 }\n"
             "var^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log[1] := 5\n"
             "  }\n"
             "}\n"
             "var^ outer = p^ {\n"
             "  var^ drop = p^ {\n"
             "    var^ c = gen()\n"
             "    c.start()\n"
             "  }\n"
             "  drop()\n"
             "}\n"
             "outer()\n"
             "return^ log\n");
    LHAT_CHECK(lhat_is_object_kind(r.ran.value, LHAT_OBJECT_TABLE), "a table");
    LHAT_CHECK_EQ_INT(
        lhat_as_integer(lhat_table_get(
            (const LhatTable *)lhat_as_object(r.ran.value), lhat_integer(1))),
        5);
    run_dispose(&r);

    // The other side of the same rule: the run ending is not the machine
    // ending, so what the program is still holding is not garbage and its
    // cleanups do not run. 10.7 promises a dropped coroutine, not a live one.
    LHAT_TEST("one still held when the run ends is left alone");
    run_text(&r,
             "var^ log = { 0 }\n"
             "var^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log[1] := 5\n"
             "  }\n"
             "}\n"
             "var^ c = gen()\n"
             "c.start()\n"
             "return^ log\n");
    LHAT_CHECK(lhat_is_object_kind(r.ran.value, LHAT_OBJECT_TABLE), "a table");
    LHAT_CHECK_EQ_INT(
        lhat_as_integer(lhat_table_get(
            (const LhatTable *)lhat_as_object(r.ran.value), lhat_integer(1))),
        0);
    run_dispose(&r);

    // 16.3: a walk has no body to have left anything pending in, and holds
    // neither closure nor registers -- the collector has to leave it alone.
    LHAT_TEST("a dropped walk is collected without any of this");
    run_checked_text(&r,
                     "var^ drop = p^ {\n"
                     "  var^ t = { 10, 20, 30 }\n"
                     "  var^ w = t.iterate^()\n"
                     "  w.start()\n"
                     "}\n"
                     "drop()\n"
                     "L^.collectgarbage()\n"
                     "return^ 7\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // 15.4 with 5.4: a capture of a coroutine's own local has to survive the
    // suspension. The frame's registers move into the coroutine at a yield^
    // and come back at a resume -- possibly at a different depth, where the
    // old slots belong to somebody else -- so the capture travels with them
    // (retargeted into the saved buffer, and back). Before this, it kept
    // pointing at the old addresses and read whatever frame took them over.
    LHAT_TEST("a capture survives a resume at another depth");
    run_text(&r,
             "var^ result = 0\n"
             "var^ gen = p^ {\n"
             "  var^ x = 1\n"
             "  var^ get = f^ -> number^ { return^ x }\n"
             "  yield^ 1\n"
             "  x := 99\n"
             "  result := get()\n"
             "  yield^ 2\n"
             "}\n"
             "var^ co = gen()\n"
             "co.start()\n"
             "var^ deep = p^ d {\n"
             "  if^ d > 0 { deep(d - 1) return^ 0 }\n"
             "  co.resume(nil^)\n"
             "  return^ 0\n"
             "}\n"
             "deep(6)\n"
             "return^ result\n");
    CHECK_INTEGER(&r, 99);
    run_dispose(&r);

    // The write side of the same sharing: while the frame is suspended, the
    // capture points into the saved buffer, so a ':=' through an escaped
    // closure lands where the resume restores from.
    LHAT_TEST("and a write made while suspended is seen after the resume");
    run_text(&r,
             "var^ setter : p^number^;|nil^ = nil^\n"
             "var^ reader = 0\n"
             "var^ gen = p^ {\n"
             "  var^ x = 1\n"
             "  setter := p^ v:number^ { x := v }\n"
             "  yield^ 1\n"
             "  reader := x\n"
             "  yield^ 2\n"
             "}\n"
             "var^ co = gen()\n"
             "co.start()\n"
             "var^ s = setter ?? p^ v:number^ { }\n"
             "s(50)\n"
             "var^ deep = p^ d {\n"
             "  if^ d > 0 { deep(d - 1) return^ 0 }\n"
             "  co.resume(nil^)\n"
             "  return^ 0\n"
             "}\n"
             "deep(4)\n"
             "return^ reader\n");
    CHECK_INTEGER(&r, 50);
    run_dispose(&r);

    // 5.12: the capture keeps the coroutine reachable (suspended_in). With
    // the handle dropped and the collector run, the escaped closure still
    // reads the saved slot rather than freed memory.
    LHAT_TEST("a capture keeps the suspended coroutine alive");
    run_text(&r,
             "var^ g : (f^ -> number^;)|nil^ = nil^\n"
             "var^ hold = p^ {\n"
             "  var^ gen = p^ {\n"
             "    var^ y = 7\n"
             "    g := f^ -> number^ { return^ y }\n"
             "    yield^ 1\n"
             "    yield^ 2\n"
             "  }\n"
             "  var^ c = gen()\n"
             "  c.start()\n"
             "}\n"
             "hold()\n"
             "L^.collectgarbage()\n"
             "L^.collectgarbage()\n"
             "var^ gf = g ?? f^ -> number^ { return^ -1 }\n"
             "return^ gf()\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // 02 の 15.8: what a plain call does not do, yieldall^ does. The inner
    // one's yields reach the outer one's resumer.
    LHAT_TEST("yieldall^ forwards the inner one's yields");
    run_text(&r,
             "var^ a = p^ { yield^ 1 yield^ 2 }\n"
             "var^ b = p^ { yieldall^ a() yield^ 3 }\n"
             "var^ c = b()\n"
             "var^ x = c.start()\n"
             "var^ y = c.resume(nil^)\n"
             "var^ z = c.resume(nil^)\n"
             "return^ x * 100 + y * 10 + z\n");
    CHECK_INTEGER(&r, 123);
    run_dispose(&r);

    // 15.14: await^ is that same delegation, so a request raised deep inside
    // reaches whoever is driving the outer one -- which is what makes a
    // scheduler possible without the language knowing about one. Here the
    // driving is done by hand: the request comes out of start(), and the
    // answer goes back in through resume().
    LHAT_TEST("a request reaches the driver through await^, and the answer returns");
    run_text(&r,
             "var^ inner = p^ -> number^ {\n"
             "  var^ got : number^ = yield^ 7\n"
             "  return^ got + 1\n"
             "}\n"
             "var^ task = p^ -> number^ {\n"
             "  var^ v : number^ = await^ inner()\n"
             "  return^ v * 10\n"
             "}\n"
             "var^ t = task()\n"
             "var^ asked = t.start()\n"
             "var^ done = t.resume(41)\n"
             "return^ asked * 1000 + done\n");
    CHECK_INTEGER(&r, 7420);
    run_dispose(&r);

    // 15.8 with 13.8改: what the inner body yields may be a tuple, and the
    // loop cannot say its width -- a yieldall^ has no count of names to read
    // one off, the way a for^ does. So the run travels through the frame's
    // answer room and the loop forwards it whole. Before this it was
    // refused: the checker took the inner's produce type as its own, and
    // then the machine faulted on a width nobody had reserved.
    LHAT_TEST("yieldall^ forwards a tuple the inner body yields");
    run_checked_text(&r,
                     "var^ g = p^ { yield^ 1, 2  yield^ 3, 4 }\n"
                     "var^ d = p^ { yieldall^ g() }\n"
                     "var^ total = 0\n"
                     "for^ a, b in^ d() { total := total + a * 10 + b }\n"
                     "return^ total\n");
    CHECK_INTEGER(&r, 12 + 34);
    run_dispose(&r);

    LHAT_TEST("and goes on yielding tuples of its own afterwards");
    run_checked_text(&r,
                     "var^ g = p^ { yield^ 1, 2  yield^ 3, 4 }\n"
                     "var^ d = p^ { yieldall^ g()  yield^ 5, 6 }\n"
                     "var^ total = 0\n"
                     "for^ a, b in^ d() { total := total + a * 10 + b }\n"
                     "return^ total\n");
    CHECK_INTEGER(&r, 12 + 34 + 56);
    run_dispose(&r);

    // A chain hands the run along one answer room at a time, so a middle
    // link is not a special case.
    LHAT_TEST("a chain of delegations forwards it too");
    run_checked_text(&r,
                     "var^ g = p^ { yield^ 1, 2  yield^ 3, 4 }\n"
                     "var^ m = p^ { yieldall^ g() }\n"
                     "var^ d = p^ { yieldall^ m() }\n"
                     "var^ total = 0\n"
                     "for^ a, b in^ d() { total := total + a * 10 + b }\n"
                     "return^ total\n");
    CHECK_INTEGER(&r, 12 + 34);
    run_dispose(&r);

    LHAT_TEST("the width is whatever the inner body said, not two");
    run_checked_text(&r,
                     "var^ g = p^ { yield^ 1, 2, 3 }\n"
                     "var^ d = p^ { yieldall^ g() }\n"
                     "for^ a, b, c in^ d() { return^ a + b + c }\n"
                     "return^ 0\n");
    CHECK_INTEGER(&r, 6);
    run_dispose(&r);

    // 15.8: the value of the delegation is the inner one's return value, the
    // shape PEP 380 gave a generator's return.
    LHAT_TEST("the value of yieldall^ is the inner return");
    run_text(&r,
             "var^ a = p^ { yield^ 1 return^ 9 }\n"
             "var^ b = p^ { var^ r = yieldall^ a() yield^ r }\n"
             "var^ c = b()\n"
             "c.start()\n"
             "return^ c.resume(nil^)\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    // 15.4 through a delegation: what the resume sends reaches the inner one.
    LHAT_TEST("what the resume sends reaches the inner coroutine");
    run_text(&r,
             "var^ a = p^ {\n"
             "  var^ got = yield^ 0\n"
             "  return^ got + 1\n"
             "}\n"
             "var^ b = p^ { var^ r = yieldall^ a() yield^ r }\n"
             "var^ c = b()\n"
             "c.start()\n"
             "return^ c.resume(41)\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    LHAT_TEST("delegations nest");
    run_text(&r,
             "var^ a = p^ { yield^ 1 }\n"
             "var^ b = p^ { yieldall^ a() yield^ 2 }\n"
             "var^ d = p^ { yieldall^ b() yield^ 3 }\n"
             "var^ c = d()\n"
             "var^ x = c.start()\n"
             "var^ y = c.resume(nil^)\n"
             "var^ z = c.resume(nil^)\n"
             "return^ x * 100 + y * 10 + z\n");
    CHECK_INTEGER(&r, 123);
    run_dispose(&r);

    LHAT_TEST("a body that only delegates is still yieldable");
    run_text(&r,
             "var^ a = p^ { yield^ 5 }\n"
             "var^ b = p^ { yieldall^ a() }\n"
             "var^ c = b()\n"
             "return^ c.start()\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    // 15.8 の 15.5: the plain call is what does nothing, which is the whole
    // reason the delegation had to be written.
    LHAT_TEST("a plain call still only makes a coroutine");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ a = p^ { log.n := 1 yield^ 0 }\n"
             "var^ b = p^ { a() yield^ 9 }\n"
             "var^ c = b()\n"
             "c.start()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    LHAT_TEST("yield^ outside a coroutine is a fault");
    run_text(&r, "yield^ 1\nreturn^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_YIELD_OUTSIDE);
    run_dispose(&r);

    // 15.2: start and resume split the two jobs -- the machine holds that
    // split itself (vm.h's opening comment), so each has to be called on the
    // state that makes it mean something.
    LHAT_TEST("resuming one that has never started is a fault");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 }\n"
             "var^ c = gen()\n"
             "c.resume(nil^)\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_COROUTINE_NOT_STARTED);
    run_dispose(&r);

    LHAT_TEST("starting one that is already suspended is a fault");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 yield^ 2 }\n"
             "var^ c = gen()\n"
             "c.start()\n"
             "c.start()\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_COROUTINE_ALREADY_STARTED);
    run_dispose(&r);

    LHAT_TEST("starting one that has already finished is a fault");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 }\n"
             "var^ c = gen()\n"
             "c.start()\n"
             "c.resume(nil^)\n"
             "c.start()\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_COROUTINE_ALREADY_STARTED);
    run_dispose(&r);

    LHAT_TEST("start() takes no arguments");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 }\n"
             "var^ c = gen()\n"
             "c.start(1)\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_ARITY);
    run_dispose(&r);

    LHAT_TEST("resume() needs exactly one argument, not zero");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 }\n"
             "var^ c = gen()\n"
             "c.start()\n"
             "c.resume()\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_ARITY);
    run_dispose(&r);

    LHAT_TEST("resume() needs exactly one argument, not two");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 }\n"
             "var^ c = gen()\n"
             "c.start()\n"
             "c.resume(1, 2)\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_ARITY);
    run_dispose(&r);

    // 15.6改: the two questions, which is how a consumer picks an operation
    // rather than finding out by faulting.
    LHAT_TEST("a fresh coroutine has neither started nor finished");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 }\n"
             "var^ c = gen()\n"
             "return^ c.started() or^ c.done()\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    LHAT_TEST("a suspended one has started and has not finished");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 yield^ 2 }\n"
             "var^ c = gen()\n"
             "c.start()\n"
             "return^ c.started() and^ !c.done()\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("a finished one answers both");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 }\n"
             "var^ c = gen()\n"
             "c.start()\n"
             "c.resume(nil^)\n"
             "return^ c.started() and^ c.done()\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // The reason both exist: what a resume answers is Y|Ret (13.9), and a
    // body that yields nil^ and ends without a value answers nil^ either
    // way. Nothing in the value says which one it was.
    LHAT_TEST("done() tells the end from a yield^ of the same value");
    run_text(&r,
             "var^ gen = p^ { yield^ }\n"
             "var^ c = gen()\n"
             "var^ log = { s := 0 }\n"
             "c.start()\n"
             "if^ c.done() { log.s := log.s + 1 }\n"
             "c.resume(nil^)\n"
             "if^ c.done() { log.s := log.s + 10 }\n"
             "return^ log.s\n");
    CHECK_INTEGER(&r, 10);  // suspended, then finished
    run_dispose(&r);

    // 10.7: disposal ends the coroutine without the body reaching its end.
    LHAT_TEST("disposal finishes it too");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 yield^ 2 }\n"
             "var^ c = gen()\n"
             "c.start()\n"
             "c.dispose()\n"
             "return^ c.done()\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // Neither runs the body, so neither is refused on a coroutine that every
    // other operation would fault on.
    LHAT_TEST("both still answer after the coroutine is dead");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 }\n"
             "var^ c = gen()\n"
             "c.start()\n"
             "c.resume(nil^)\n"
             "return^ c.done() and^ c.started()\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("done() takes no arguments");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 }\n"
             "var^ c = gen()\n"
             "c.done(1)\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_ARITY);
    run_dispose(&r);

    LHAT_TEST("started() takes no arguments");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 }\n"
             "var^ c = gen()\n"
             "c.started(1)\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_ARITY);
    run_dispose(&r);

    // The pair is enough to drive a coroutine that was handed over rather
    // than made here -- which done() alone could not do, since a fresh one
    // and a suspended one both answer false.
    LHAT_TEST("the two together drive one of unknown state");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 yield^ 2 yield^ 3 }\n"
             "var^ drain = p^ c {\n"
             "  var^ n = 0\n"
             "  if^ !c.started() { c.start() n := n + 1 }\n"
             "  repeat^ while^ !c.done() { c.resume(nil^) n := n + 1 }\n"
             "  return^ n\n"
             "}\n"
             "return^ drain(gen())\n");
    CHECK_INTEGER(&r, 4);  // three yields, then the end
    run_dispose(&r);

    // yieldall^ drives a freshly made coroutine on its own, with no start()
    // written anywhere -- 15.8's whole point is that the delegation handles
    // this by itself.
    LHAT_TEST("yieldall^ still starts a fresh coroutine on its own");
    run_text(&r,
             "var^ a = p^ { yield^ 1 }\n"
             "var^ b = p^ { yieldall^ a() }\n"
             "var^ c = b()\n"
             "return^ c.start()\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);
}

int main(void)
{
    test_coroutines();
    return lhat_test_report("test_vm_coroutine");
}
