// L^ (lhat) -- tests for the bytecode and the machine.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed. Programs are run end to end, since what is worth pinning is the
// answer rather than the instructions chosen to reach it -- 5.1 expects those
// to be replaced by specialised ones later.

#include <math.h>
#include <stdlib.h>  // malloc: the rendered traceback
#include <string.h>

#include "code.h"
#include "fixture.h"

// 04: errors are values. 5.6 wants no unwinding for any of this, and none of
// it needs any.
static void test_errors(void)
{
    Run r;

    LHAT_TEST("an error is a value like any other");
    run_text(&r,
             "errordef^ IOError { NotFound, Denied }\n"
             "var^ e = error^IOError.NotFound{ message := \"no such file\" }\n"
             "return^ e.message\n");
    CHECK_STRING(&r, "no such file");
    run_dispose(&r);

    // 2.3: every kind gets message and cause without declaring either.
    LHAT_TEST("message defaults to the empty string");
    run_text(&r,
             "errordef^ IOError { NotFound }\n"
             "return^ error^IOError.NotFound{ }.message\n");
    CHECK_STRING(&r, "");
    run_dispose(&r);

    LHAT_TEST("cause defaults to nil^");
    run_text(&r,
             "errordef^ IOError { NotFound }\n"
             "return^ error^IOError.NotFound{ }.cause\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    LHAT_TEST("a declared field comes back");
    run_text(&r,
             "errordef^ ParseError { Syntax { line : number^, column : number^ }, Eof }\n"
             "var^ e = error^ParseError.Syntax{ line := 3, column := 12 }\n"
             "return^ e.line * 100 + e.column\n");
    CHECK_INTEGER(&r, 312);
    run_dispose(&r);

    // 2.2: a default is an expression evaluated at each construction, not a
    // value stored once.
    LHAT_TEST("a field left out takes its default");
    run_text(&r,
             "errordef^ ParseError { Syntax { line := 0, column := 0 } }\n"
             "var^ e = error^ParseError.Syntax{ line := 7 }\n"
             "return^ e.line * 100 + e.column\n");
    CHECK_INTEGER(&r, 700);
    run_dispose(&r);

    LHAT_TEST("the default is evaluated at each construction");
    run_text(&r,
             "var^ n = 0\n"
             "var^ next = f^ { n := n + 1 return^ n }\n"
             "errordef^ E { K { seq := next() } }\n"
             "var^ a = error^E.K{ }\n"
             "var^ b = error^E.K{ }\n"
             "return^ b.seq\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    // 2.4: two declarations may spell a kind the same and still be different
    // kinds. This is the whole reason identity is the declaration site.
    LHAT_TEST("the same spelling in two declarations is two kinds");
    run_text(&r,
             "errordef^ IOError { NotFound }\n"
             "errordef^ UserError { NotFound }\n"
             "var^ e = error^IOError.NotFound{ }\n"
             "return^ e isa^ UserError.NotFound\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    LHAT_TEST("and it is the kind it was made from");
    run_text(&r,
             "errordef^ IOError { NotFound }\n"
             "errordef^ UserError { NotFound }\n"
             "var^ e = error^IOError.NotFound{ }\n"
             "return^ e isa^ IOError.NotFound\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // 2.3: naming the declaration asks about the union of its kinds.
    LHAT_TEST("naming the declaration asks about all of its kinds");
    run_text(&r,
             "errordef^ IOError { NotFound, Denied }\n"
             "var^ e = error^IOError.Denied{ }\n"
             "return^ e isa^ IOError\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("a sibling kind is not the one it is");
    run_text(&r,
             "errordef^ IOError { NotFound, Denied }\n"
             "var^ e = error^IOError.Denied{ }\n"
             "return^ e isa^ IOError.NotFound\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    LHAT_TEST("something that is not an error is not a kind either");
    run_text(&r,
             "errordef^ IOError { NotFound }\n"
             "return^ 1 isa^ IOError\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    LHAT_TEST("a kind that was never declared does not compile");
    run_text(&r, "return^ error^Nowhere.Missing{ }\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    // 2.3: the declaration is the union, not a type to construct.
    LHAT_TEST("the declaration itself cannot be constructed");
    run_text(&r,
             "errordef^ IOError { NotFound }\n"
             "return^ error^IOError{ }\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    // 02 の 12.7改, 04 の 11.6: the program's own panic. The value rides
    // along, the line names where, and op_name stays empty -- it answers
    // only for the eight operator instructions.
    LHAT_TEST("panic^ carries its value out");
    run_text(&r, "panic^ 42\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_OK);
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_PANIC);
    LHAT_CHECK(lhat_is_integer(r.ran.value), "the value rides along");
    LHAT_CHECK_EQ_INT(lhat_as_integer(r.ran.value), 42);
    LHAT_CHECK_EQ_INT(r.ran.line, 1);
    LHAT_CHECK(r.ran.op_name == NULL, "no operator to name");
    run_dispose(&r);

    // 12.7改: not catchable -- catch^ substitutes error values, and a panic
    // is not one.
    LHAT_TEST("panic^ passes catch^ by");
    run_text(&r,
             "var^ f = p^ -> number^ { panic^ 7 }\n"
             "return^ f() catch^ 0\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_OK);
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_PANIC);
    run_dispose(&r);
}

// 04 の 4 章 and 5 章, and 02 の 11.7.
// 04 の 11.6改: a fault leaves its frames standing, and the machine answers
// them until the next run -- lhat_machine_fault_depth/_frame, and the text
// lhat_machine_traceback spells. run_text compiles without a program, so
// `source` stays NULL here; the stamped path is the program tests'.
static void test_traceback(void)
{
    Run r;

    // The statements after each call keep the calls out of tail position --
    // a tail call reuses the frame, and the folded caller leaves no trace
    // (5.3; Lua elides them the same way).
    LHAT_TEST("a fault leaves its frames readable");
    run_text(&r,
             "var^ inner = p^ { panic^ \"boom\" }\n"
             "var^ outer = p^ {\n"
             "    inner()\n"
             "    var^ after = 1\n"
             "}\n"
             "outer()\n"
             "var^ ending = 1\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_PANIC);
    LHAT_CHECK_EQ_INT(lhat_machine_fault_depth(r.machine), 3);
    LhatFrameInfo info;
    LHAT_CHECK(lhat_machine_fault_frame(r.machine, 0, &info), "level 0");
    LHAT_CHECK(info.name != NULL && strcmp(info.name, "inner") == 0,
               "the innermost frame is the binding's name");
    LHAT_CHECK_EQ_INT(info.line, 1);
    LHAT_CHECK(!info.top_level, "a body, not the unit");
    LHAT_CHECK(lhat_machine_fault_frame(r.machine, 1, &info), "level 1");
    LHAT_CHECK(info.name != NULL && strcmp(info.name, "outer") == 0,
               "the caller under its own name");
    LHAT_CHECK_EQ_INT(info.line, 3);
    LHAT_CHECK(lhat_machine_fault_frame(r.machine, 2, &info), "level 2");
    LHAT_CHECK(info.name == NULL, "the unit's frame has no binding name");
    LHAT_CHECK_EQ_INT(info.line, 6);
    LHAT_CHECK(!lhat_machine_fault_frame(r.machine, 3, &info),
               "nothing past the top level");

    LHAT_TEST("and the rendered text names them innermost first");
    {
        size_t needed = lhat_machine_traceback(r.machine, NULL, 0);
        char *text = (char *)malloc(needed + 1);
        LHAT_CHECK(text != NULL, "room for the text");
        if (text != NULL) {
            lhat_machine_traceback(r.machine, text, needed + 1);
            LHAT_CHECK(strstr(text, "in inner") != NULL, "the fault's frame");
            LHAT_CHECK(strstr(text, "in outer") != NULL, "its caller");
            const char *first = strstr(text, "in inner");
            const char *second = strstr(text, "in outer");
            LHAT_CHECK(first != NULL && second != NULL && first < second,
                       "innermost first");
            free(text);
        }
    }
    run_dispose(&r);

    LHAT_TEST("a coroutine's frame says so");
    run_text(&r,
             "var^ gen = p^ { panic^ 1 yield^ 2 }\n"
             "var^ c = gen()\n"
             "c.start()\n"
             "var^ ending = 1\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_PANIC);
    LHAT_CHECK(lhat_machine_fault_frame(r.machine, 0, &info), "level 0");
    LHAT_CHECK(info.coroutine, "the body's frame is the coroutine's");
    run_dispose(&r);

    LHAT_TEST("a clean run leaves nothing to trace");
    run_text(&r, "return^ 1\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
    LHAT_CHECK_EQ_INT(lhat_machine_fault_depth(r.machine), 0);
    LHAT_CHECK_EQ_INT(lhat_machine_traceback(r.machine, NULL, 0), 0);
    run_dispose(&r);
}

static void test_catch_and_try(void)
{
    Run r;

    LHAT_TEST("catch^ replaces an error with its right side");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "var^ fail = f^ { return^ error^E.Bad{ } }\n"
             "return^ fail() catch^ 0\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    LHAT_TEST("and leaves a value that is not an error alone");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "var^ ok = f^ { return^ 7 }\n"
             "return^ ok() catch^ 0\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // 4.2: the error is it^ inside the right side, the same word 02 の 16.2
    // uses for a focus.
    LHAT_TEST("the caught error is it^");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "var^ fail = f^ { return^ error^E.Bad{ message := \"oh\" } }\n"
             "return^ fail() catch^ it^.message\n");
    CHECK_STRING(&r, "oh");
    run_dispose(&r);

    LHAT_TEST("it^ tells the kinds apart");
    run_text(&r,
             "errordef^ E { A, B }\n"
             "var^ fail = f^ { return^ error^E.B{ } }\n"
             "return^ fail() catch^ if^ it^ isa^ E.A: 1 el^: 2 ;\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    // 4.3: catch^ binds tighter than the arithmetic around it, so what fails
    // is the call and not the sum.
    LHAT_TEST("catch^ attaches to the operation, not the sum");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "var^ fail = f^ { return^ error^E.Bad{ } }\n"
             "return^ 10 + fail() catch^ 5\n");
    CHECK_INTEGER(&r, 15);
    run_dispose(&r);

    LHAT_TEST("it^ is gone again after the catch^");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "var^ ok = f^ { return^ 1 }\n"
             "var^ x = ok() catch^ 0\n"
             "return^ it^\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    // 5.1: try^ hands the error to the caller and carries on otherwise.
    LHAT_TEST("try^ returns the error from the procedure that wrote it");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "var^ inner = f^ { return^ error^E.Bad{ message := \"deep\" } }\n"
             "var^ outer = f^ {\n"
             "  var^ v = try^ inner()\n"
             "  return^ v + 1\n"
             "}\n"
             "return^ outer().message\n");
    CHECK_STRING(&r, "deep");
    run_dispose(&r);

    LHAT_TEST("and gets out of the way when there is no error");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "var^ inner = f^ { return^ 41 }\n"
             "var^ outer = f^ { return^ try^ inner() + 1 }\n"
             "return^ outer()\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    // 5.2: try^ leaves the way return^ does, so a loop's last^ and epilog^
    // do not run.
    LHAT_TEST("try^ leaving a loop runs neither last^ nor epilog^");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "var^ out = { n := 0 }\n"
             "var^ fail = f^ { return^ error^E.Bad{ } }\n"
             "var^ go = f^ {\n"
             "  repeat^ 3 {\n"
             "    main^:\n"
             "      var^ v = try^ fail()\n"
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

    LHAT_TEST("try^ stands as a statement when the value is not wanted");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "var^ fail = f^ { return^ error^E.Bad{ message := \"gone\" } }\n"
             "var^ go = f^ { try^ fail() return^ 0 }\n"
             "return^ go().message\n");
    CHECK_STRING(&r, "gone");
    run_dispose(&r);

    // 04 の 4.5: the arms are where a try^ inside the block leaves for, and
    // an arm is chosen by 13.11's judgement -- so the kind decides which one
    // runs, and it^ is narrowed to it there.
    LHAT_TEST("a try^{ } arm takes the kind it names");
    run_text(&r,
             "errordef^ E { Missing { where : string^ }, Denied }\n"
             "var^ fail = f^ n:number^ -> number^|E {\n"
             "  if^ n = 1 { return^ error^E.Missing{ where = \"here\" } }\n"
             "  if^ n = 2 { return^ error^E.Denied{ } }\n"
             "  return^ 10\n"
             "}\n"
             "var^ pick = f^ n:number^ -> string^ {\n"
             "  try^{\n"
             "    var^ v = try^ fail(n)\n"
             "    return^ \"ok\"\n"
             "  catch^ E.Missing:\n"
             "    return^ it^.where\n"
             "  catch^:\n"
             "    return^ \"other\"\n"
             "  }\n"
             "}\n"
             "return^ pick(0) .. pick(1) .. pick(2)\n");
    CHECK_STRING(&r, "okhereother");
    run_dispose(&r);

    // The whole point of the form: a return^ written in an arm is the
    // enclosing subroutine's, not an escape from the block.
    LHAT_TEST("and a return^ in an arm leaves the subroutine");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "var^ fail = f^ { return^ error^E.Bad{ } }\n"
             "var^ go = f^ -> number^ {\n"
             "  try^{\n"
             "    var^ v = try^ fail()\n"
             "  catch^:\n"
             "    return^ 1\n"
             "  }\n"
             "  return^ 2\n"
             "}\n"
             "return^ go()\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 4.5: what no arm takes leaves the way a bare try^ would have sent it.
    LHAT_TEST("and what no arm takes goes on out");
    run_text(&r,
             "errordef^ E { A, B }\n"
             "var^ fail = f^ { return^ error^E.B{ message := \"through\" } }\n"
             "var^ go = f^ {\n"
             "  try^{\n"
             "    var^ v = try^ fail()\n"
             "  catch^ E.A:\n"
             "    return^ \"wrong arm\"\n"
             "  }\n"
             "  return^ \"no error\"\n"
             "}\n"
             "return^ go().message\n");
    CHECK_STRING(&r, "through");
    run_dispose(&r);

    // Reaching an arm is a way out of the block and no further, so what the
    // block opened is drained on the way -- 9.8's reading, one construct over.
    LHAT_TEST("and a with^ inside the block disposes before the arm runs");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "var^ log = { text = \"\" }\n"
             "var^ Res = def^{ self^{ },\n"
             "  dispose = p^self^ { log.text := log.text .. \"d\" } }\n"
             "var^ fail = f^ { return^ error^E.Bad{ } }\n"
             "var^ go = f^ -> string^ {\n"
             "  try^{\n"
             "    with^ r = Res.new() {\n"
             "      var^ v = try^ fail()\n"
             "    }\n"
             "  catch^:\n"
             "    return^ log.text .. \"a\"\n"
             "  }\n"
             "  return^ \"no error\"\n"
             "}\n"
             "return^ go()\n");
    CHECK_STRING(&r, "da");
    run_dispose(&r);

    // 02 の 11.7: '??' is the same shape, asking about nil^.
    // The escape keeps "??'" from being read as a trigraph for '^'.
    LHAT_TEST("'?\?' supplies a value for a missing key");
    run_text(&r, "var^ t = { }\nreturn^ t[\"nowhere\"] ?? 5\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    LHAT_TEST("and leaves a key that is there alone");
    run_text(&r, "var^ t = { a := 1 }\nreturn^ t[\"a\"] ?? 5\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 11.7 asks about nil^ and nothing else, so false^ is a value that is
    // there rather than one that is missing.
    LHAT_TEST("'?\?' asks about nil^, not about being false");
    run_text(&r, "var^ t = { a := false^ }\nreturn^ t[\"a\"] ?? true^\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    // 04 の 11.4 with 01 の 7.1: the '?' access forms answer nil^ for
    // an absent target rather than reaching into one. Before this they were
    // parsed and then compiled as ordinary accesses, so they faulted on the
    // very value they exist to handle.
    LHAT_TEST("'?.' answers nil^ for an absent target");
    run_text(&r,
             "var^ a : t^{ x : number^ }|nil^ = nil^\n"
             "return^ a?.x ?? 7\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("and reads the member when one is there");
    run_text(&r,
             "var^ a : t^{ x : number^ }|nil^ = { x = 41 }\n"
             "return^ a?.x ?? 7\n");
    CHECK_INTEGER(&r, 41);
    run_dispose(&r);

    LHAT_TEST("'?[' answers nil^ for an absent target");
    run_text(&r,
             "var^ t : t^{ ...:number^ }|nil^ = nil^\n"
             "return^ t?[1] ?? 7\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("'?(' answers nil^ for an absent callee");
    run_text(&r,
             "var^ f : (f^number^ -> number^;)|nil^ = nil^\n"
             "return^ f?(20) ?? 7\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("and calls the callee when one is there");
    run_text(&r,
             "var^ f : (f^number^ -> number^;)|nil^ ="
             " f^ n:number^ -> number^ { return^ n * 2 }\n"
             "return^ f?(21) ?? 7\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    // The short circuit is the point: an absent target skips what the access
    // was going to evaluate. Every optional-chaining language reads this way,
    // and a key or an argument with an effect in it would otherwise run for a
    // call that never happened.
    LHAT_TEST("an absent callee evaluates no argument");
    run_text(&r,
             "var^ log = { ran = 0 }\n"
             "var^ f : (f^number^ -> number^;)|nil^ = nil^\n"
             "var^ side = p^ -> number^ { log.ran := 1 return^ 5 }\n"
             "f?(side())\n"
             "return^ log.ran\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 11.7改2: the run is one guard, so a call written on a guarded access is
    // skipped with it. The callee here is read *out of* the absent target,
    // which is the shape the per-link reading could not express.
    LHAT_TEST("a call on a guarded access is skipped with it");
    run_text(&r,
             "var^ a : t^{ b : (f^ -> number^;) }|nil^ = nil^\n"
             "return^ a?.b() ?? 7\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("and runs when the target is there");
    run_text(&r,
             "var^ a : t^{ b : (f^ -> number^;) }|nil^ ="
             " { b = f^ -> number^ { return^ 3 } }\n"
             "return^ a?.b() ?? 7\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    LHAT_TEST("its arguments are skipped too");
    run_text(&r,
             "var^ log = { ran = 0 }\n"
             "var^ a : t^{ b : (f^number^ -> number^;) }|nil^ = nil^\n"
             "var^ side = p^ -> number^ { log.ran := 1 return^ 5 }\n"
             "a?.b(side())\n"
             "return^ log.ran\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // Every link after the guard, not only the one next to it.
    LHAT_TEST("and a member further along the run is skipped as well");
    run_text(&r,
             "var^ t : t^{ x : t^{ y : number^ } }|nil^ = nil^\n"
             "return^ t?.x.y ?? 5\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    LHAT_TEST("reaching the end of the run when the target is there");
    run_text(&r,
             "var^ t : t^{ x : t^{ y : number^ } }|nil^ = { x = { y = 11 } }\n"
             "return^ t?.x.y ?? 5\n");
    CHECK_INTEGER(&r, 11);
    run_dispose(&r);

    // '(' makes no node, so grouping cannot cut a run in half -- which is the
    // one place the chain-wide reading bites in JavaScript.
    LHAT_TEST("a bracket does not cut the run");
    run_text(&r,
             "var^ t : t^{ x : t^{ y : number^ } }|nil^ = nil^\n"
             "return^ (t?.x).y ?? 5\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    // '?[' is guarded the same way, and a call on it belongs to the same run.
    LHAT_TEST("'?[' followed by a call is one guard too");
    run_text(&r,
             "var^ t : t^{ (f^ -> number^;) }|nil^ = nil^\n"
             "return^ t?[1]() ?? 7\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // 11.7改2: the postfix form asks about absence instead of reaching
    // through it, so it answers bool^ and the '?' family is complete.
    LHAT_TEST("'?' answers false for an absent value");
    run_text(&r, "var^ t : number^|nil^ = nil^\nreturn^ t?\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    LHAT_TEST("and true for one that is there");
    run_text(&r, "var^ t : number^|nil^ = 5\nreturn^ t?\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // 11.7 asks about nil^ and nothing else, and so does this -- 5.4 keeps
    // truthiness out, which is what lets false^ be a value that is there.
    LHAT_TEST("'?' asks about nil^, not about being false");
    run_text(&r, "var^ t : bool^|nil^ = false^\nreturn^ t?\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("'?' narrows what the branch reads");
    run_text(&r,
             "var^ t : number^|nil^ = 41\n"
             "if^ t? { return^ t + 1 }\n"
             "return^ 0\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    // 8.2 keeps a bare expression from being a statement, so this one is
    // bound -- a call is a statement (8.3) and an index is not.
    LHAT_TEST("an absent target evaluates no key");
    run_text(&r,
             "var^ log = { ran = 0 }\n"
             "var^ t : t^{ ...:number^ }|nil^ = nil^\n"
             "var^ side = p^ -> number^ { log.ran := 1 return^ 1 }\n"
             "var^ got = t?[side()]\n"
             "return^ log.ran\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);
}

// 02 の 10 章 and 12 章: the cleanups, which 5.5 makes one mechanism.
static void test_cleanups(void)
{
    Run r;

    // 04 の 11.6改2: and the one exit it does not run for. A panic is the
    // fatal layer -- what catch^ cannot take, the language does not tidy up
    // after either; the frames stay standing for the traceback to read, and
    // the host's own resources come back through 05 の 8.8's three
    // occasions rather than through this.
    LHAT_TEST("a panic^ leaves finally^ unrun");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ go = p^ {\n"
             "  do^{\n"
             "    panic^ 1\n"
             "  finally^:\n"
             "    log.n := 7\n"
             "  }\n"
             "}\n"
             "go()\n"
             "var^ ending = 1\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_PANIC);
    LHAT_CHECK_EQ_INT(lhat_machine_fault_depth(r.machine), 2);
    run_dispose(&r);

    LHAT_TEST("and so does a fault the machine itself raises");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ t : t^{ x : number^ }|nil^ = nil^\n"
             "do^{\n"
             "  var^ read = t.x\n"
             "finally^:\n"
             "  log.n := 7\n"
             "}\n"
             "var^ ending = 1\n");
    LHAT_CHECK(r.ran.status != LHAT_RUN_OK, "the read faulted");
    run_dispose(&r);

    LHAT_TEST("finally^ runs when the block ends");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "do^{\n"
             "  log.n := 1\n"
             "finally^:\n"
             "  log.n := log.n + 10\n"
             "}\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 11);
    run_dispose(&r);

    // 10.2: however the block is left.
    LHAT_TEST("finally^ runs when return^ leaves through it");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ go = p^ {\n"
             "  do^{\n"
             "    return^ 1\n"
             "  finally^:\n"
             "    log.n := 7\n"
             "  }\n"
             "}\n"
             "go()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("and when break^ leaves through it");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "repeat^ 5 {\n"
             "  do^{\n"
             "    log.n := log.n + 1\n"
             "    break^\n"
             "  finally^:\n"
             "    log.n := log.n + 10\n"
             "  }\n"
             "}\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 11);
    run_dispose(&r);

    // 04 の 5.2: try^ leaves the way return^ does, so finally^ still runs.
    LHAT_TEST("and when try^ hands an error to the caller");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "var^ log = { n := 0 }\n"
             "var^ fail = f^ { return^ error^E.Bad{ } }\n"
             "var^ go = p^ {\n"
             "  do^{\n"
             "    var^ v = try^ fail()\n"
             "  finally^:\n"
             "    log.n := 3\n"
             "  }\n"
             "  return^ 0\n"
             "}\n"
             "go()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 10.1: a p^ body is a block like any other.
    LHAT_TEST("a procedure body may carry a finally^");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ go = p^ {\n"
             "  return^ 1\n"
             "finally^:\n"
             "  log.n := 5\n"
             "}\n"
             "go()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    // 10.4: innermost first, which is the order the frame drains in.
    LHAT_TEST("nested finally^ run from the inside out");
    run_text(&r,
             "var^ log = { s := 0 }\n"
             "var^ go = p^ {\n"
             "  do^{\n"
             "    do^{\n"
             "      return^ 1\n"
             "    finally^:\n"
             "      log.s := log.s * 10 + 1\n"
             "    }\n"
             "  finally^:\n"
             "    log.s := log.s * 10 + 2\n"
             "  }\n"
             "}\n"
             "go()\n"
             "return^ log.s\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    LHAT_TEST("a finally^ that was already run is not run again");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ go = p^ {\n"
             "  do^{\n"
             "    log.n := log.n + 1\n"
             "  finally^:\n"
             "    log.n := log.n + 10\n"
             "  }\n"
             "  return^ 0\n"
             "}\n"
             "go()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 11);
    run_dispose(&r);

    // 10.5: Java lets a finally return and silently replace the answer. C#
    // refuses; so does this.
    LHAT_TEST("return^ inside a finally^ does not compile");
    run_text(&r, "do^{\n  var^ x = 1\nfinally^:\n  return^ 2\n}\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNSUPPORTED);
    run_dispose(&r);

    // 9.2 and 10.9: finally^ comes after the loop's own clauses.
    LHAT_TEST("a loop's finally^ runs after its epilog^");
    run_text(&r,
             "var^ log = { s := 0 }\n"
             "repeat^ 1 {\n"
             "  main^:\n"
             "    log.s := log.s * 10 + 1\n"
             "  epilog^:\n"
             "    log.s := log.s * 10 + 2\n"
             "  finally^:\n"
             "    log.s := log.s * 10 + 3\n"
             "}\n"
             "return^ log.s\n");
    CHECK_INTEGER(&r, 123);
    run_dispose(&r);

    // 9.8: a return^ out of a loop runs neither epilog^ nor last^, but 10.2
    // still runs the finally^.
    LHAT_TEST("a return^ out of a loop runs the finally^ and nothing else");
    run_text(&r,
             "var^ log = { s := 0 }\n"
             "var^ go = p^ {\n"
             "  repeat^ 3 {\n"
             "    main^:\n"
             "      return^ 1\n"
             "    epilog^:\n"
             "      log.s := log.s * 10 + 2\n"
             "    finally^:\n"
             "      log.s := log.s * 10 + 3\n"
             "  }\n"
             "}\n"
             "go()\n"
             "return^ log.s\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 12.2: dispose() at the end of the block.
    LHAT_TEST("with^ disposes at the end of the block");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ open = f^ { return^ { dispose := p^ { log.n := 1 } } }\n"
             "with^ h = open()\n"
             "{\n"
             "  log.n := 0\n"
             "}\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 12.2: the reverse of the order they were defined in, so a resource that
    // depends on an earlier one goes first.
    LHAT_TEST("several with^ dispose in reverse order");
    run_text(&r,
             "var^ log = { s := 0 }\n"
             "var^ res = f^n { return^ { dispose := p^ { log.s := log.s * 10 + n } } }\n"
             "with^ a = res(1)\n"
             "with^ b = res(2)\n"
             "{\n"
             "  log.s := 0\n"
             "}\n"
             "return^ log.s\n");
    CHECK_INTEGER(&r, 21);
    run_dispose(&r);

    // 12.3: dispose() has the same strength as finally^.
    LHAT_TEST("with^ disposes when return^ leaves through it");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ open = f^ { return^ { dispose := p^ { log.n := 4 } } }\n"
             "var^ go = p^ {\n"
             "  with^ h = open()\n"
             "  {\n"
             "    return^ 1\n"
             "  }\n"
             "}\n"
             "go()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);

    // 12.4: finally^ first, dispose() after, because with^ wraps the block
    // from outside and finally^ is its last clause from inside.
    LHAT_TEST("finally^ runs before the dispose that surrounds it");
    run_text(&r,
             "var^ log = { s := 0 }\n"
             "var^ open = f^ { return^ { dispose := p^ { log.s := log.s * 10 + 2 } } }\n"
             "with^ h = open()\n"
             "{\n"
             "  log.s := 0\n"
             "finally^:\n"
             "  log.s := log.s * 10 + 1\n"
             "}\n"
             "return^ log.s\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    LHAT_TEST("the resource is in scope inside the block and not after it");
    run_text(&r,
             "var^ open = f^ { return^ { dispose := p^ { }, n := 6 } }\n"
             "var^ seen = 0\n"
             "with^ h = open()\n"
             "{\n"
             "  seen := h.n\n"
             "}\n"
             "return^ seen\n");
    CHECK_INTEGER(&r, 6);
    run_dispose(&r);

    run_text(&r,
             "var^ open = f^ { return^ { dispose := p^ { } } }\n"
             "with^ h = open()\n"
             "{\n"
             "}\n"
             "return^ h\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);
}

int main(void)
{
    test_errors();
    test_traceback();
    test_catch_and_try();
    test_cleanups();
    return lhat_test_report("test_vm_error");
}
