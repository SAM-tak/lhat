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

// 03 の 4 章: a REPL is one machine answering many inputs, so the machine has
// to be an object a caller keeps rather than something a run owns.
static void test_machine(void)
{
    LHAT_TEST("a machine answers more than one input");
    {
        LhatMachine *m = lhat_machine_new();
        LHAT_CHECK(m != NULL, "a machine");
        Run first, second;
        compile_text(&first, "return^ 6 * 7\n");
        compile_text(&second, "return^ \"and\" .. \" again\"\n");
        LhatRunResult a = lhat_run(m, first.proto);
        LhatRunResult b = lhat_run(m, second.proto);
        LHAT_CHECK_EQ_INT(a.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(a.value), 42);
        LHAT_CHECK_EQ_INT(b.status, LHAT_RUN_OK);
        LHAT_CHECK(lhat_is_object_kind(b.value, LHAT_OBJECT_STRING),
                   "a string came back");
        // What each run allocated is still on the machine when the next one
        // starts, which is what a REPL needs of it.
        LHAT_CHECK(b.live >= a.live, "the heap carried over");
        lhat_machine_dispose(m);
        compiled_dispose(&first);
        compiled_dispose(&second);
    }

    // The static one could serve only one caller and never nest.
    LHAT_TEST("and two machines stand side by side");
    {
        LhatMachine *one = lhat_machine_new();
        LhatMachine *two = lhat_machine_new();
        Run text;
        compile_text(&text, "return^ 5\n");
        LhatRunResult a = lhat_run(one, text.proto);
        LhatRunResult b = lhat_run(two, text.proto);
        LHAT_CHECK_EQ_INT(lhat_as_integer(a.value), 5);
        LHAT_CHECK_EQ_INT(lhat_as_integer(b.value), 5);
        lhat_machine_dispose(one);
        lhat_machine_dispose(two);
        compiled_dispose(&text);
    }

    // 02 の 13.7 with 05 の 3.2: a script's top level is 'p^...', and what
    // lhat_run_arguments hands over is its '...'. With nothing handed over
    // the collector is there and empty.
    LHAT_TEST("a script's top level collects what it is run with");
    {
        LhatMachine *m = lhat_machine_new();
        Run text;
        compile_text(&text,
                     "let^ args = ...\n"
                     "var^ sum = 0\n"
                     "for^ a in^ args { if^ a isa^ number^ { sum := sum + a } }\n"
                     "return^ args.count^ * 100 + sum\n");
        LhatValue handed[3] = {lhat_integer(5), lhat_integer(7), lhat_nil()};
        handed[2] = lhat_integer(9);
        LhatRunResult with = lhat_run_arguments(m, text.proto, handed, 3);
        LHAT_CHECK_EQ_INT(with.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(with.value), 300 + 21);
        LhatRunResult without = lhat_run(m, text.proto);
        LHAT_CHECK_EQ_INT(without.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(without.value), 0);
        lhat_machine_dispose(m);
        compiled_dispose(&text);
    }

    // 03 の 4.3: the top-level names of one input are still there for the
    // next, which is what a session is for.
    LHAT_TEST("a session carries top-level names between inputs");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two, three;
        compile_next_text(&one, s, "var^ x = 40\nreturn^ x\n");
        compile_next_text(&two, s, "var^ y = 2\nreturn^ x + y\n");
        compile_next_text(&three, s, "x := x + y\nreturn^ x\n");
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, one.proto).value), 40);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, two.proto).value), 42);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, three.proto).value), 42);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
        compiled_dispose(&three);
    }

    LHAT_TEST("and a subroutine one input made is callable in the next");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two;
        compile_next_text(
            &one, s,
            "var^ greet = f^ n:string^ -> string^ { return^ \"hi \" .. n }\n");
        compile_next_text(&two, s, "return^ greet(\"there\")\n");
        lhat_run(m, one.proto);
        LhatRunResult r = lhat_run(m, two.proto);
        LHAT_CHECK_EQ_INT(r.status, LHAT_RUN_OK);
        LHAT_CHECK(lhat_is_object_kind(r.value, LHAT_OBJECT_STRING),
                   "the subroutine survived");
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
    }

    // 03 の 4.3: a name written again keeps the slot it had, so a prompt does
    // not run out of registers however many times a line is rewritten.
    LHAT_TEST("a name written again keeps its slot");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run turns[300];
        size_t taken = 0;
        bool all_compiled = true;
        for (size_t i = 0; i < 300; i++) {
            char text[64];
            snprintf(text, sizeof text, "var^ x = %zu\nreturn^ x\n", i);
            compile_next_text(&turns[taken], s, text);
            if (turns[taken].compiled != LHAT_COMPILE_OK) {
                all_compiled = false;
                compiled_dispose(&turns[taken]);
                break;
            }
            taken++;
        }
        LHAT_CHECK(all_compiled, "300 rewrites of one name compiled");
        if (taken > 0) {
            LHAT_CHECK_EQ_INT(
                lhat_as_integer(lhat_run(m, turns[taken - 1].proto).value),
                (int64_t)(taken - 1));
        }
        for (size_t i = 0; i < taken; i++) {
            compiled_dispose(&turns[i]);
        }
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
    }

    // 8.7 keeps a name visible before its var^ runs, and the slot still holds
    // what the last input put there.
    LHAT_TEST("and a redefinition reads what is already in it");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two;
        compile_next_text(&one, s, "var^ x = 1\n");
        compile_next_text(&two, s, "var^ x = x + 10\nreturn^ x\n");
        lhat_run(m, one.proto);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, two.proto).value), 11);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
    }

    // 03 の 4.3: an input answers with the value of its last statement, when
    // that statement is an expression -- so a prompt shows a call's result
    // without a return^ written by hand.
    LHAT_TEST("an input answers with its last expression");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two, three, four;
        compile_asked_text(&one, s, "2 + 3\n");
        compile_asked_text(
            &two, s,
            "var^ twice = f^ n:number^ -> number^ { return^ n * 2 }\n");
        compile_asked_text(&three, s, "twice(21)\n");
        // Only the last one answers; the statements before it still run.
        compile_asked_text(&four, s, "var^ k = 7\nk + 1\n");
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, one.proto).value), 5);
        LhatRunResult made = lhat_run(m, two.proto);
        LHAT_CHECK(lhat_is_nil(made.value), "a var^ answers with nothing");
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, three.proto).value), 42);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, four.proto).value), 8);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
        compiled_dispose(&three);
        compiled_dispose(&four);
    }

    // The value is the answer, not a return^ -- a call answering nothing is
    // still a statement, and what follows it still runs.
    LHAT_TEST("and a call answering nothing does not stop the input");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one;
        compile_asked_text(&one, s,
                           "var^ n = 0\n"
                           "var^ bump = p^ { n := n + 1 }\n"
                           "bump()\n"
                           "bump()\n"
                           "n\n");
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, one.proto).value), 2);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
    }

    // 02 の 14.2 and 04 の 2.4: what a def^ composes onto and what an
    // errordef^ declared are worked out while compiling and never reach the
    // machine, so an input that needs them needs what an earlier one found.
    LHAT_TEST("a def^ from an earlier input can be composed onto");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two, three;
        compile_next_text(&one, s, "var^ A = def^{ self^{ x = 1 } }\n");
        compile_next_text(&two, s,
                          "var^ B = A .. def^{ bar := f^self^ -> number^ "
                          "{ return^ 2 } }\n");
        compile_next_text(&three, s,
                          "var^ b = B.new()\n"
                          "return^ b.x * 10 + b.bar()\n");
        LHAT_CHECK_EQ_INT(one.compiled, LHAT_COMPILE_OK);
        LHAT_CHECK_EQ_INT(two.compiled, LHAT_COMPILE_OK);
        LHAT_CHECK_EQ_INT(three.compiled, LHAT_COMPILE_OK);
        lhat_run(m, one.proto);
        lhat_run(m, two.proto);
        // 12 is the field from the first input and the method from the
        // second: a node read through the wrong input's lexer would name a
        // different field and answer nil^.
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, three.proto).value), 12);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
        compiled_dispose(&three);
    }

    LHAT_TEST("and an errordef^ from an earlier input is still declared");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two;
        compile_next_text(&one, s, "errordef^ E { Bad, Worse }\n");
        compile_next_text(&two, s,
                          "var^ e = error^ E.Worse { }\n"
                          "if^ e isa^ E.Worse { return^ 1 }\n"
                          "return^ 0\n");
        LHAT_CHECK_EQ_INT(one.compiled, LHAT_COMPILE_OK);
        LHAT_CHECK_EQ_INT(two.compiled, LHAT_COMPILE_OK);
        lhat_run(m, one.proto);
        // The second kind, so a lookup reading the declaration through this
        // input's lexer would not find it by name at all.
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, two.proto).value), 1);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
    }

    LHAT_TEST("and an overload^ added in a later input is callable");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two, three;
        compile_next_text(&one, s,
                          "var^ Foo = def^{ self^{}, "
                          "foo := f^self^ -> number^ { return^ 1 } }\n");
        compile_next_text(&two, s,
                          "var^ Bar = Foo .. def^{ overload^ "
                          "foo := f^self^, s:string^ -> number^ "
                          "{ return^ 2 } }\n");
        compile_next_text(&three, s,
                          "var^ b = Bar.new()\n"
                          "return^ b.foo() * 10 + b.foo(\"x\")\n");
        lhat_run(m, one.proto);
        lhat_run(m, two.proto);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, three.proto).value), 12);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
        compiled_dispose(&three);
    }

    // 03 の 4.3 with 5.4: the session's top level goes on holding its slots
    // between inputs, so a place a closure captured in one input is the very
    // place a later one reads and writes. Which is what makes a prompt agree
    // with a file: the same three lines in one unit answer 2 as well.
    LHAT_TEST("a ':=' inside a closure reaches a later input");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two, three, four;
        compile_next_text(&one, s, "var^ a = 1\n");
        compile_next_text(&two, s,
                          "var^ f = f^ -> number^ { a := 2 return^ 1 }\n");
        compile_next_text(&three, s, "return^ f()\n");
        compile_next_text(&four, s, "return^ a\n");
        lhat_run(m, one.proto);
        lhat_run(m, two.proto);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, three.proto).value), 1);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, four.proto).value), 2);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
        compiled_dispose(&three);
        compiled_dispose(&four);
    }

    // Severing one name's sharing is severing that name's, not the slot's
    // neighbours' -- every var^ of a name at a session's top level goes back
    // in the one slot, so a CLOSE over the whole frame would take the
    // bindings above it with it.
    LHAT_TEST("and writing another name over does not sever it");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two, three, four;
        compile_next_text(&one, s, "var^ a = 1\nvar^ b = 2\n");
        compile_next_text(&two, s,
                          "var^ g = f^ -> number^ { b := 9 return^ 0 }\n");
        compile_next_text(&three, s, "var^ a = 5\nreturn^ g()\n");
        compile_next_text(&four, s, "return^ b\n");
        lhat_run(m, one.proto);
        lhat_run(m, two.proto);
        lhat_run(m, three.proto);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, four.proto).value), 9);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
        compiled_dispose(&three);
        compiled_dispose(&four);
    }

    // 5.4's sharing is of one binding, not of the slot it happens to sit in.
    // A var^ writing the name again is a new binding -- it reuses the slot,
    // so what captured the earlier one has to stop sharing it there. That is
    // the difference from the ':=' above, which writes the same binding.
    LHAT_TEST("a closure keeps what it captured when its input ended");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two, three, four;
        compile_next_text(&one, s, "var^ x = 1\n");
        compile_next_text(&two, s,
                          "var^ show = f^ -> number^ { return^ x }\n");
        compile_next_text(&three, s, "var^ x = 2\nreturn^ x\n");
        compile_next_text(&four, s, "return^ show()\n");
        lhat_run(m, one.proto);
        lhat_run(m, two.proto);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, three.proto).value), 2);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, four.proto).value), 1);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
        compiled_dispose(&three);
        compiled_dispose(&four);
    }

    // Which is what keeps a redefinition to another type from making an
    // earlier closure's result type a lie.
    LHAT_TEST("so redefining to another type does not reach it");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two, three, four;
        compile_next_text(&one, s, "var^ x = 1\n");
        compile_next_text(&two, s,
                          "var^ show = f^ -> number^ { return^ x }\n");
        compile_next_text(&three, s, "var^ x = \"now\"\n");
        compile_next_text(&four, s, "return^ show() + 1\n");
        lhat_run(m, one.proto);
        lhat_run(m, two.proto);
        lhat_run(m, three.proto);
        LhatRunResult r = lhat_run(m, four.proto);
        LHAT_CHECK_EQ_INT(r.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(r.value), 2);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
        compiled_dispose(&three);
        compiled_dispose(&four);
    }

    // One proto, several runs: 5.2 and 5.4 make the registers and the frames
    // the run's, so nothing of one is left over in the next.
    LHAT_TEST("and one proto may be run again");
    {
        LhatMachine *m = lhat_machine_new();
        Run text;
        compile_text(&text, "var^ n = 0\nn := n + 1\nreturn^ n\n");
        LhatRunResult a = lhat_run(m, text.proto);
        LhatRunResult b = lhat_run(m, text.proto);
        LHAT_CHECK_EQ_INT(lhat_as_integer(a.value), 1);
        LHAT_CHECK_EQ_INT(lhat_as_integer(b.value), 1);
        lhat_machine_dispose(m);
        compiled_dispose(&text);
    }
}

// 03 の 1.2 keeps Lua's incremental collector as something to borrow later.
// What is pinned here is that the working form reclaims what a program lets
// go of and keeps what it does not.
static void test_collection(void)
{
    Run r;

    // Every turn makes a table and drops it. Without a collector these all
    // pile up until the run ends.
    LHAT_TEST("what the program lets go of is reclaimed");
    run_text(&r,
             "var^ n = 0\n"
             "repeat^ 2000 { var^ t = { a := 1, b := 2 } n := n + 1 }\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 2000);
    LHAT_CHECK(r.ran.collected > 1000, "the collector ran and freed");
    LHAT_CHECK(r.ran.live < 500, "little is left at the end");
    run_dispose(&r);

    // The same loop, holding on to every table. Nothing may be freed.
    //
    // 5.12: this is also where a missing write barrier on a table shows up.
    // The loop runs long enough for `kept` to be black by the time most of
    // these are written into it, so without the barrier they are never
    // reached and the sweep takes them while the program still holds them.
    LHAT_TEST("what the program holds is kept");
    run_text(&r,
             "var^ kept = { }\n"
             "for^ i from^ 1 to^ 2000 { kept[i] := { a := i } }\n"
             "return^ kept[1500].a\n");
    CHECK_INTEGER(&r, 1500);
    LHAT_CHECK(r.ran.live > 2000, "every table held is still there");
    run_dispose(&r);

    // 5.4: a closure keeps the place it captured, so what that place holds
    // has to survive the collection too.
    LHAT_TEST("a captured place keeps its value alive");
    run_text(&r,
             "var^ get = f^ { return^ 0 }\n"
             "do^{\n"
             "  var^ held = { n := 42 }\n"
             "  get := f^ { return^ held.n }\n"
             "}\n"
             "repeat^ 2000 { var^ waste = { a := 1 } }\n"
             "return^ get()\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    // A suspended coroutine holds its registers, and they are roots through
    // it rather than through any frame.
    LHAT_TEST("a suspended coroutine keeps what its registers hold");
    run_text(&r,
             "var^ gen = p^ {\n"
             "  var^ mine = { n := 7 }\n"
             "  yield^ 0\n"
             "  yield^ mine.n\n"
             "}\n"
             "var^ c = gen()\n"
             "c.start()\n"
             "repeat^ 2000 { var^ waste = { a := 1 } }\n"
             "return^ c.resume()\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // 5.12: the same for a coroutine that suspends after the collector has
    // already looked at it. `kept` is what makes that possible -- a heap big
    // enough that the marking cannot finish inside one step, so the cycle is
    // really left half done while the program runs. The coroutine is black
    // by the second yield^, and what that one saves into its registers is
    // reached only through the barrier.
    LHAT_TEST("and ones it takes in after the collector has looked at it");
    run_text(&r,
             "var^ kept = { }\n"
             "var^ gen = p^ {\n"
             "  var^ last = 0\n"
             "  repeat^ 200 {\n"
             "    var^ mine = { n := 11 }\n"
             "    yield^ last\n"
             "    last := mine.n\n"
             "  }\n"
             "}\n"
             "var^ c = gen()\n"
             "c.start()\n"
             "var^ next = 30\n"
             "for^ i from^ 1 to^ 2000 {\n"
             "  kept[i] := { a := i }\n"
             "  if^ i = next { c.resume()  next := next + 30 }\n"
             "}\n"
             "return^ c.resume()\n");
    CHECK_INTEGER(&r, 11);
    run_dispose(&r);

    // 5.12: and a closed upvalue, which is the other place a value goes that
    // no register holds. While the upvalue is open the place is a stack slot
    // and the roots cover it; closed, the upvalue is the only way to what it
    // holds, and a write to a black one needs the forward barrier. `kept` is
    // again what keeps a cycle open long enough for the upvalue to be black
    // when the writes land.
    LHAT_TEST("a closed upvalue keeps what is written through it");
    run_text(&r,
             "var^ kept = { }\n"
             "var^ put = p^ v { }\n"
             "var^ read = f^ { return^ 0 }\n"
             "do^{\n"
             "  var^ box = { n := 0 }\n"
             "  put := p^ v { box := v }\n"
             "  read := f^ { return^ box.n }\n"
             "}\n"
             "for^ i from^ 1 to^ 2000 {\n"
             "  kept[i] := { a := i }\n"
             "  put({ n := i })\n"
             "}\n"
             "return^ read()\n");
    CHECK_INTEGER(&r, 2000);
    run_dispose(&r);

    // 14.12: an overload^ adds an arm to the group already in the definition
    // it composes with, and that group has been sitting in a table since
    // long before. 5.12 puts a barrier on that write for the same reason as
    // the three above -- though unlike them nothing here can make the group
    // be black at the moment the arm arrives, since a composition happens
    // once and not in a loop. This pins the composition, not the barrier.
    LHAT_TEST("an overload group keeps the arms added to an old one");
    run_text(&r,
             "var^ kept = { }\n"
             "var^ Base = def^{ self^{ }, m := f^self^, x:string^ { return^ 1 } }\n"
             "var^ Mid = Base .. def^{\n"
             "  self^{ },\n"
             "  overload^\n"
             "  m := f^self^, x:number^ { return^ x },\n"
             "}\n"
             "for^ i from^ 1 to^ 500 { kept[i] := { a := i } }\n"
             "var^ Sub = Mid .. def^{\n"
             "  self^{ },\n"
             "  overload^\n"
             "  m := f^self^, x:bool^ { return^ 3 },\n"
             "}\n"
             "return^ Sub.new().m(7)\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // 5.12: collectgarbage() answers for the heap as it is when it is
    // called, which a cycle already half run does not. So it finishes that
    // one and then runs another from a standing start -- and 02 の 10.7's
    // holding back happens in the second, where the drop is visible. Without
    // the second cycle this coroutine's finally^ would wait for whenever the
    // collector next came round.
    LHAT_TEST("collectgarbage answers for the heap as it is, mid-cycle");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := 5\n"
             "  }\n"
             "}\n"
             "var^ outer = p^ {\n"
             "  var^ drop = p^ {\n"
             "    var^ c = gen()\n"
             "    c.start()\n"
             "  }\n"
             "  drop()\n"
             "}\n"
             "repeat^ 2000 { var^ waste = { a := 1 } }\n"
             "outer()\n"
             "L^.collectgarbage()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    // A string the answer points at has to outlive every collection between
    // it being made and the run ending.
    LHAT_TEST("the answer survives");
    run_text(&r,
             "var^ s = \"kept\"\n"
             "repeat^ 2000 { var^ waste = { a := 1 } }\n"
             "return^ s .. \"!\"\n");
    CHECK_STRING(&r, "kept!");
    run_dispose(&r);

    // A cycle is unreachable but points at itself, so reference counting
    // would keep it. Marking from the roots does not.
    LHAT_TEST("a cycle the program dropped is reclaimed");
    run_text(&r,
             "var^ n = 0\n"
             "repeat^ 2000 {\n"
             "  var^ a = { }\n"
             "  var^ b = { }\n"
             "  a.other := b\n"
             "  b.other := a\n"
             "  n := n + 1\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 2000);
    LHAT_CHECK(r.ran.live < 500, "the cycles went");
    run_dispose(&r);

    // 14.2's link from an instance to its definition is a reference like any
    // other, and the definition outlives the instances.
    LHAT_TEST("a definition outlives the instances that read it");
    run_text(&r,
             "var^ Foo = def^{ self^{ n := 0 }, get := f^self^ { return^ self^.n } }\n"
             "var^ last = Foo.new()\n"
             "repeat^ 2000 { var^ f = Foo.new() }\n"
             "return^ last.get()\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 05 の 8.6 の host 版: lhat_machine_collectgarbage is the same cycle
    // asked for from C, for a host that has a machine and no L^ code it
    // wants to run to reach one.
    //
    // A run that ended holds nothing: frame_count is back to zero, so the
    // registers that were the only way to `kept` are no longer roots. This
    // is the whole of what a host has to know before calling -- what the
    // machine can reach is L^ and what is on the frames, and after a run
    // there are no frames.
    LHAT_TEST("a host may ask for the cycle itself");
    run_text(&r,
             "var^ kept = { }\n"
             "for^ i from^ 1 to^ 2000 { kept[i] := { a := i } }\n"
             "return^ 1\n");
    CHECK_INTEGER(&r, 1);
    LHAT_CHECK(r.ran.live > 2000, "the run ended holding every table");
    {
        size_t after = lhat_machine_collectgarbage(r.machine);
        LHAT_CHECK(after < 500, "and the host's cycle reclaimed them: %zu",
                   after);
        // The same count LhatRunResult.live carries, and nothing was
        // allocated in between, so asking again answers the same.
        LHAT_CHECK_EQ_INT(lhat_machine_collectgarbage(r.machine), after);
    }
    run_dispose(&r);

    // L^ itself is a root, so a machine that has run nothing still has what
    // 8.6 built -- and collecting it is not an error.
    LHAT_TEST("a machine that ran nothing keeps what L^ carries");
    {
        LhatMachine *m = lhat_machine_new();
        size_t live = lhat_machine_collectgarbage(m);
        LHAT_CHECK(live > 0, "L^ and its members are still there: %zu", live);
        LHAT_CHECK_EQ_INT(lhat_machine_collectgarbage(m), live);
        lhat_machine_dispose(m);
    }

    LHAT_TEST("no machine is nothing to collect");
    LHAT_CHECK_EQ_INT(lhat_machine_collectgarbage(NULL), 0);
}

// 02 の 14.4 の host 版: an instance's members are shared and take self^, so
// without this a host holding an object of the language's could call nothing
// on it. What is pinned here is that the receiver arrives where the member
// asked for it.
static LhatValue member_of(const LhatValue owner, const char *name)
{
    const LhatTable *table = (const LhatTable *)lhat_as_object(owner);
    LhatValue found = lhat_nil();
    for (size_t i = 0; i < table->entry_capacity; i++) {
        const LhatTableEntry *entry = &table->entries[i];
        if (lhat_is_nil(entry->key) ||
            !lhat_is_object_kind(entry->key, LHAT_OBJECT_STRING)) {
            continue;
        }
        const LhatString *key = (const LhatString *)lhat_as_object(entry->key);
        if (strcmp(key->text, name) == 0) {
            found = entry->value;
        }
    }
    return found;
}

static void test_call_member(void)
{
    Run r;

    // The whole of what a host embedding the language does with an object:
    // make one through the definition's `new`, then call its members.
    LHAT_TEST("a method is handed the instance it was reached through");
    run_text(&r,
             "return^ def^{\n"
             "  self^{ n = 7 },\n"
             "  get = f^self^ -> number^ { return^ self^.n },\n"
             "  add = f^self^, by:number^ -> number^ { return^ self^.n + by },\n"
             "  bump = p^self^, by:number^ { self^.n := self^.n + by },\n"
             "  plain = f^ -> number^ { return^ 99 },\n"
             "  total = f^self^, ... -> number^ {\n"
             "    var^ sum = self^.n\n"
             "    for^ i, x in^ ... { sum += x }\n"
             "    return^ sum\n"
             "  },\n"
             "}\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
    {
        // 05 の 8.6: nothing roots what a run answered once its frame is
        // gone, so the definition goes into L^ before anything else runs.
        LHAT_CHECK(lhat_machine_set_global(r.machine, "Held", r.ran.value),
                   "the definition is rooted");
        // 14.9's `new` is an ordinary closure the definition carries, so the
        // plain entry point reaches it and nothing new is needed for that.
        LhatValue make = member_of(r.ran.value, "new");
        LhatRunResult made = lhat_machine_call(r.machine, make, NULL, 0);
        LHAT_CHECK_EQ_INT(made.status, LHAT_RUN_OK);
        LhatValue instance = made.value;
        // The same rooting again, and for the same reason: every call below
        // runs L^ code, and a collection reaches only L^, the open frames and
        // the pending disposals -- a host's own C variable is not a root.
        LHAT_CHECK(lhat_machine_set_global(r.machine, "It", instance),
                   "the instance is rooted");

        LhatRunResult got = lhat_machine_call_member(r.machine, instance,
                                                     "get", 3, NULL, 0);
        LHAT_CHECK_EQ_INT(got.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(got.value), 7);

        LhatValue by = lhat_integer(5);
        LhatRunResult sum = lhat_machine_call_member(r.machine, instance,
                                                     "add", 3, &by, 1);
        LHAT_CHECK_EQ_INT(sum.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(sum.value), 12);

        // A p^ writing through self^ reaches the instance the host holds.
        LhatRunResult wrote = lhat_machine_call_member(r.machine, instance,
                                                       "bump", 4, &by, 1);
        LHAT_CHECK_EQ_INT(wrote.status, LHAT_RUN_OK);
        got = lhat_machine_call_member(r.machine, instance, "get", 3, NULL, 0);
        LHAT_CHECK_EQ_INT(lhat_as_integer(got.value), 12);

        // 14.4: a member that takes no self^ is a static one -- it belongs to
        // the definition, and an instance does not see it. `d.plain()` is a
        // type error written in L^, and this is the same refusal reached
        // from C.
        LhatRunResult plain = lhat_machine_call_member(r.machine, instance,
                                                       "plain", 5, NULL, 0);
        LHAT_CHECK_EQ_INT(plain.status, LHAT_RUN_NOT_CALLABLE);

        // 13.7: the tail collects the same way a compiled call collects it,
        // with the receiver not counted among what was written.
        LhatValue rest[3] = {lhat_integer(1), lhat_integer(2), lhat_integer(3)};
        LhatRunResult all = lhat_machine_call_member(r.machine, instance,
                                                     "total", 5, rest, 3);
        LHAT_CHECK_EQ_INT(all.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(all.value), 18);

        // Reached through the definition it is an ordinary call, and the
        // receiver goes unused because the member asked for none.
        LhatRunResult through = lhat_machine_call_member(
            r.machine, r.ran.value, "plain", 5, NULL, 0);
        LHAT_CHECK_EQ_INT(through.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(through.value), 99);

        // 14.7: what an instance does see through its definition is the
        // shared member it reaches with self^ -- which every call above is.

        LHAT_TEST("and what it refuses, it refuses the way an instruction does");
        LhatRunResult wrong = lhat_machine_call_member(r.machine, instance,
                                                       "add", 3, NULL, 0);
        LHAT_CHECK_EQ_INT(wrong.status, LHAT_RUN_ARITY);

        // 04 の 11.3: a key that is not there answers nil^, and nil^ is not
        // callable.
        LhatRunResult missing = lhat_machine_call_member(r.machine, instance,
                                                         "nope", 4, NULL, 0);
        LHAT_CHECK_EQ_INT(missing.status, LHAT_RUN_NOT_CALLABLE);

        LhatRunResult flat = lhat_machine_call_member(
            r.machine, lhat_integer(1), "get", 3, NULL, 0);
        LHAT_CHECK_EQ_INT(flat.status, LHAT_RUN_TYPE_ERROR);
    }
    run_dispose(&r);

    // 02 の 14.12: one name, several signatures -- the search a call site
    // makes is the search this makes.
    LHAT_TEST("an overloaded member picks the arm that fits");
    run_text(&r,
             "var^ Foo = def^{\n"
             "  self^{ n = 2 },\n"
             "  scale = f^self^, by:number^ -> number^ { return^ self^.n * by },\n"
             "}\n"
             "var^ Bar = Foo .. def^{\n"
             "  overload^ scale := f^self^, by:string^ -> string^ { return^ by },\n"
             "}\n"
             "return^ Bar\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
    {
        LHAT_CHECK(lhat_machine_set_global(r.machine, "Held", r.ran.value),
                   "the definition is rooted");
        LhatValue make = member_of(r.ran.value, "new");
        LhatValue instance = lhat_machine_call(r.machine, make, NULL, 0).value;
        LHAT_CHECK(lhat_machine_set_global(r.machine, "It", instance),
                   "the instance is rooted");

        LhatValue number = lhat_integer(4);
        LhatRunResult by_number = lhat_machine_call_member(
            r.machine, instance, "scale", 5, &number, 1);
        LHAT_CHECK_EQ_INT(by_number.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(by_number.value), 8);

        LhatValue text = lhat_nil();
        LHAT_CHECK(lhat_machine_make_string(r.machine, "hi", 2, &text),
                   "a string to hand over");
        LhatRunResult by_text = lhat_machine_call_member(r.machine, instance,
                                                         "scale", 5, &text, 1);
        LHAT_CHECK_EQ_INT(by_text.status, LHAT_RUN_OK);
        LHAT_CHECK(lhat_is_object_kind(by_text.value, LHAT_OBJECT_STRING),
                   "the string arm ran");

        LhatValue neither = lhat_bool(true);
        LhatRunResult none = lhat_machine_call_member(r.machine, instance,
                                                      "scale", 5, &neither, 1);
        LHAT_CHECK_EQ_INT(none.status, LHAT_RUN_NO_CANDIDATE);
    }
    run_dispose(&r);
}


// 5.12: a host writing into a table the machine already holds is the one
// write the collector never sees coming. A cycle only steps inside the
// interpreter loop, so a run can end with the marking half done -- and a
// table marked before it ended is black. object.h's lhat_table_set knows
// nothing of a machine and can lay no barrier, which is why vm.h has its
// own.
static void test_host_table_write(void)
{
    Run r;

    LHAT_TEST("5.12: what a host writes into a live table survives");
    // A big enough heap that the collector has been round several times,
    // which is the shape a host meets. Which phase a run happens to end in
    // is nothing a caller can choose from out here -- so what this pins is
    // the contract rather than the timing, and the invariant check
    // LHAT_GC_PARANOID makes on every step is what would say the barrier had
    // gone missing.
    run_text(&r,
             "var^ kept = { }\n"
             "for^ i from^ 1 to^ 2000 { kept[i] := { a := i } }\n"
             "return^ { held := kept, check := f^ {\n"
             "  L^.collectgarbage()\n"
             "  return^ kept[5000]\n"
             "} }\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
    {
        LHAT_CHECK(lhat_machine_set_global(r.machine, "Held", r.ran.value),
                   "the pair is rooted");
        LhatValue held = member_of(r.ran.value, "held");
        LHAT_CHECK(lhat_is_object_kind(held, LHAT_OBJECT_TABLE),
                   "and the table is in it");

        // Nothing but the table will hold this, so it is exactly what a
        // missing barrier loses.
        LhatValue mark = lhat_nil();
        LHAT_CHECK(lhat_machine_make_string(r.machine, "the host wrote this",
                                            19, &mark),
                   "a value made outside any instruction");
        bool refused = false;
        LHAT_CHECK(lhat_machine_table_set(r.machine,
                                          (LhatTable *)lhat_as_object(held),
                                          lhat_integer(5000), mark, &refused),
                   "written");
        LHAT_CHECK(!refused, "and taken");

        // Finishes the half-run cycle and then runs one from a standing
        // start, which is where an unreached value would have been swept.
        LhatRunResult after = lhat_machine_call(
            r.machine, member_of(r.ran.value, "check"), NULL, 0);
        LHAT_CHECK_EQ_INT(after.status, LHAT_RUN_OK);
        LHAT_CHECK(lhat_is_object_kind(after.value, LHAT_OBJECT_STRING),
                   "and it is still there afterwards");
        if (lhat_is_object_kind(after.value, LHAT_OBJECT_STRING)) {
            const LhatString *text =
                (const LhatString *)lhat_as_object(after.value);
            LHAT_CHECK_EQ_STR(text->text, text->length, "the host wrote this");
        }
    }
    run_dispose(&r);

    LHAT_TEST("and what it will not write");
    run_text(&r, "return^ { }\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
    {
        LHAT_CHECK(lhat_machine_set_global(r.machine, "Held", r.ran.value),
                   "rooted");
        LhatTable *table = (LhatTable *)lhat_as_object(r.ran.value);
        bool refused = false;
        // 04 の 11.3: nil^ means "not there", so it cannot also be a key.
        LHAT_CHECK(lhat_machine_table_set(r.machine, table, lhat_nil(),
                                          lhat_integer(1), &refused),
                   "a nil^ key is refused rather than failed");
        LHAT_CHECK(refused, "and says so");
        LHAT_CHECK(!lhat_machine_table_set(r.machine, NULL, lhat_integer(1),
                                           lhat_integer(1), &refused),
                   "and no table at all is a failure");
    }
    run_dispose(&r);
}
int main(void)
{
    test_machine();
    test_call_member();
    test_collection();
    test_host_table_write();
    return lhat_test_report("test_vm_machine");
}
