// L^ (lhat) -- tests for the type checking stage.
//
// Section numbers refer to DesignDocuments/02-syntax.md unless prefixed with
// "03" or "04". The cases pinned here are the ones a decision in the
// specification produces and that would otherwise be invisible: the scope
// rule of 8.7, the result inference of 03 の 3.4, and the way catch^, ?? and
// try^ each drop one arm of a union.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture.h"

// 02 の 15.1: f^ may call only f^, never p^ -- a p^ may run side effects an
// f^ is not allowed to.
static void test_purity(void)
{
    Unit u;

    LHAT_TEST("an f^ may not call a p^");
    check_text(&u,
               "var^ log = p^ x:any^ { }\n"
               "var^ f = f^ { log(1) }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_CALLS_PROCEDURE);
    unit_dispose(&u);

    LHAT_TEST("a p^ may call another p^");
    check_text(&u,
               "var^ log = p^ x:any^ { }\n"
               "var^ g = p^ { log(1) }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("top level may call a p^");
    check_text(&u,
               "var^ log = p^ x:any^ { }\n"
               "log(1)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("an f^ may call another f^");
    check_text(&u,
               "var^ inc = f^ n:number^ -> number^ { return^ n + 1 }\n"
               "var^ f = f^ n:number^ -> number^ { return^ inc(n) }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.5: calling a yieldable p^ makes a coroutine and runs none of the
    // body yet, so this one call stays referentially transparent even
    // inside an f^ -- what would actually run the body (start()) is itself
    // a p^, caught the same as any other below.
    LHAT_TEST("an f^ may call a yieldable p^, since the call alone runs nothing");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "var^ f = f^ { return^ gen() }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("but an f^ may not start the coroutine that call makes");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "var^ f = f^ { return^ gen().start() }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_CALLS_PROCEDURE);
    unit_dispose(&u);

    LHAT_TEST("a p^ nested inside an f^ may call a p^ again");
    check_text(&u,
               "var^ log = p^ x:any^ { }\n"
               "var^ outer = f^ {\n"
               "    var^ inner = p^ { log(1) }\n"
               "    return^ 0\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("an f^ nested inside a p^ still may not call a p^");
    check_text(&u,
               "var^ log = p^ x:any^ { }\n"
               "var^ outer = p^ {\n"
               "    var^ inner = f^ { log(1) }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_CALLS_PROCEDURE);
    unit_dispose(&u);

    LHAT_TEST("the same rule reaches through a member");
    check_text(&u,
               "var^ t = { log := p^self^, x:any^ { } }\n"
               "var^ f = f^ { t.log(1) }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_CALLS_PROCEDURE);
    unit_dispose(&u);

    // 15.3改: an f^ may yield^. What it may not do is let the coroutine out,
    // since advancing one is only unobservable while it stays inside.
    LHAT_TEST("an f^ may yield^ and walk what it made");
    check_text(&u,
               "var^ gen = f^ { yield^ 1 }\n"
               "var^ sum = f^ -> number^ {\n"
               "    var^ co = gen()\n"
               "    var^ a = co.start()\n"
               "    return^ 0\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.5: a yieldable body answers a coroutine whatever else it does, so
    // 13.2 is satisfied without a return^ reaching the end.
    LHAT_TEST("a yieldable f^ needs no value-returning exit");
    check_text(&u, "var^ gen = f^ { yield^ 1 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 16.3: the built-in walk of a table changes nothing, so it is an f^
    // coroutine -- which is the case 15.3改 was reopened for.
    LHAT_TEST("an f^ may walk a table with for^ in^");
    check_text(&u,
               "var^ count = f^ t:t^{ ...:number^ } -> number^ {\n"
               "    var^ n = 0\n"
               "    for^ k, v in^ t { n := n + 1 }\n"
               "    return^ n\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.6改: start()/resume()/dispose() carry the body's kind, so 15.1's
    // calling rule is what refuses a p^ coroutine here. Nothing about yield^
    // has to be said again.
    LHAT_TEST("but not a p^ coroutine, which is an ordinary p^ call");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "var^ f = f^ -> number^ {\n"
               "    var^ co = gen()\n"
               "    var^ a = co.start()\n"
               "    return^ 0\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_CALLS_PROCEDURE);
    unit_dispose(&u);

    // The kind alone would let a body advance one it was handed. A nested f^
    // measures against its own body, the same way 15.1改 does for a table.
    LHAT_TEST("a nested f^ may not advance what the enclosing one made");
    check_text(&u,
               "var^ gen = f^ { yield^ 1 }\n"
               "var^ outer = f^ -> number^ {\n"
               "    var^ co = gen()\n"
               "    var^ inner = f^ -> number^ { var^ a = co.start() return^ 0 }\n"
               "    return^ 0\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ADVANCES_OUTSIDE);
    unit_dispose(&u);

    LHAT_TEST("and an f^ coroutine may not be returned");
    check_text(&u,
               "var^ gen = f^ { yield^ 1 }\n"
               "var^ f = f^ { return^ gen() }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_COROUTINE_ESCAPES);
    unit_dispose(&u);

    // The result type is read through, since a coroutine reaches the outside
    // in a table member as readily as on its own.
    LHAT_TEST("nor carried out inside a table");
    check_text(&u,
               "var^ gen = f^ { yield^ 1 }\n"
               "var^ f = f^ {\n"
               "    var^ co = gen()\n"
               "    return^ { c := co }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_COROUTINE_ESCAPES);
    unit_dispose(&u);

    // 13.9 with 15.3改: the front half is a signature, and its kind is the
    // body's. Written out, an f^ coroutine can be taken as an argument --
    // which is the one thing 15.3改 allows without letting it be advanced.
    LHAT_TEST("an f^ coroutine may be taken as an argument");
    check_text(&u,
               "var^ hold = f^ co:c^{ f^nil^ -> number^;, nil^ } -> number^ {\n"
               "    return^ 0\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("but not advanced, since the caller shares its state");
    check_text(&u,
               "var^ take = f^ co:c^{ f^nil^ -> number^;, nil^ } -> number^ {\n"
               "    var^ a = co.start()\n"
               "    return^ 0\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ADVANCES_OUTSIDE);
    unit_dispose(&u);

    LHAT_TEST("and the two kinds are different types");
    check_text(&u,
               "var^ gen = f^ { yield^ 1 }\n"
               "var^ f = f^ -> number^ {\n"
               "    var^ c : c^{ p^nil^ -> number^;, nil^ } = gen()\n"
               "    return^ 0\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 15.8 with 15.3改: delegating runs the inner body, which is what makes
    // its yield^ reach out here -- so it advances, and the same two questions
    // apply. No start()/resume() is written for 15.1 to catch, so the rule
    // reaches through yieldall^ itself.
    LHAT_TEST("yieldall^ to a p^ coroutine is an f^ running a p^");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "var^ f = f^ { yieldall^ gen() }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_CALLS_PROCEDURE);
    unit_dispose(&u);

    LHAT_TEST("but to one this body made it is fine");
    check_text(&u,
               "var^ gen = f^ { yield^ 1 }\n"
               "var^ f = f^ { yieldall^ gen() }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a bound one is the body's just the same");
    check_text(&u,
               "var^ gen = f^ { yield^ 1 }\n"
               "var^ f = f^ {\n"
               "    var^ co = gen()\n"
               "    yieldall^ co\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("delegating to one that arrived is refused too");
    check_text(&u,
               "var^ f = f^ co:c^{ f^nil^ -> number^;, nil^ } "
               "{ yieldall^ co }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ADVANCES_OUTSIDE);
    unit_dispose(&u);

    LHAT_TEST("a p^ delegates to either kind");
    check_text(&u,
               "var^ gen = f^ { yield^ 1 }\n"
               "var^ g = p^ { yieldall^ gen() }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.7改: not because an operator is an f^ -- 15.3改 lets one suspend --
    // but because 11.8's signature answers T and a yieldable call answers a
    // coroutine (15.5).
    LHAT_TEST("an operator may not be yieldable");
    check_text(&u,
               "var^ V = def^{\n"
               "    self^{ x := 0 },\n"
               "    op^+ := f^self^, o:number^ -> number^ "
               "{ yield^ 1 return^ 0 },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_BAD_OPERATOR);
    unit_dispose(&u);

    // A p^ coroutine leaving a p^ is what 15.5 is for; nothing here applies.
    LHAT_TEST("a p^ hands its coroutines out as before");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "var^ make = p^ { return^ gen() }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.1's other half: assignment reaches local variables only. 10.6 reads
    // it twice over -- a body with nothing writable outside it has nothing a
    // finally^ could restore.
    LHAT_TEST("an f^ may not assign to a name bound outside it");
    check_text(&u,
               "var^ total = 0\n"
               "var^ f = f^ -> number^ {\n"
               "    total := 1\n"
               "    return^ total\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_WRITES_OUT);
    unit_dispose(&u);

    LHAT_TEST("a compound assignment is the same write");
    check_text(&u,
               "var^ total = 0\n"
               "var^ f = f^ -> number^ {\n"
               "    total += 1\n"
               "    return^ total\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_WRITES_OUT);
    unit_dispose(&u);

    // 01 の 8 章: a specifier says where to start looking, so reaching out
    // with one is the same write as reaching out without one.
    LHAT_TEST("a scope specifier does not open a way out");
    check_text(&u,
               "var^ total = 0\n"
               "var^ f = f^ -> number^ {\n"
               "    $^total := 1\n"
               "    return^ 0\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_WRITES_OUT);
    unit_dispose(&u);

    LHAT_TEST("but its own local is fine");
    check_text(&u,
               "var^ f = f^ -> number^ {\n"
               "    var^ n = 0\n"
               "    n := 1\n"
               "    return^ n\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A block inside the body is still inside it, and the parameters are the
    // body's own names -- what arrives in them is a copy of the argument, so
    // writing there is not observable from outside either.
    LHAT_TEST("and so are a nested block's and a parameter's");
    check_text(&u,
               "var^ f = f^ n:number^ -> number^ {\n"
               "    var^ m = 0\n"
               "    do^{ m := 1 }\n"
               "    n := 2\n"
               "    return^ n + m\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a p^ writes wherever it likes");
    check_text(&u,
               "var^ total = 0\n"
               "var^ g = p^ { total := 1 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The rule is about the body being written in, not the one around it --
    // the same way the call rule is.
    LHAT_TEST("a p^ inside an f^ may write out again");
    check_text(&u,
               "var^ total = 0\n"
               "var^ outer = f^ -> number^ {\n"
               "    var^ inner = p^ { total := 1 }\n"
               "    return^ 0\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("an f^ inside a p^ still may not");
    check_text(&u,
               "var^ total = 0\n"
               "var^ outer = p^ {\n"
               "    var^ inner = f^ -> number^ { total := 1 return^ 0 }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_WRITES_OUT);
    unit_dispose(&u);

    // An f^ nested in another f^ measures against its own body, so what the
    // outer one made is outside the inner one.
    LHAT_TEST("a nested f^ measures against its own body");
    check_text(&u,
               "var^ outer = f^ -> number^ {\n"
               "    var^ n = 0\n"
               "    var^ inner = f^ -> number^ { n := 1 return^ 0 }\n"
               "    return^ n\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_WRITES_OUT);
    unit_dispose(&u);

    // 15.1改: the same rule for a table. What the body made is its own;
    // what arrived belongs to whoever passed it, and writing there is what
    // makes the call observable from outside.
    LHAT_TEST("an f^ may change a table it made itself");
    check_text(&u,
               "var^ f = f^ -> number^ {\n"
               "    var^ u = { x := 0 }\n"
               "    u.x := 1\n"
               "    return^ u.x\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and one a new answered with");
    check_text(&u,
               "var^ P = def^{ self^{ x := 0 } }\n"
               "var^ f = f^ -> number^ {\n"
               "    var^ u = P.new()\n"
               "    u.x := 1\n"
               "    return^ u.x\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 8.8: adding a member changes the table as much as writing over one.
    LHAT_TEST("adding a member to its own table is fine too");
    check_text(&u,
               "var^ f = f^ -> number^ {\n"
               "    var^ u = { }\n"
               "    var^ u.y := 5\n"
               "    return^ u.y\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("but not one that arrived as an argument");
    check_text(&u,
               "var^ f = f^ t:t^{ x:number^ } -> number^ {\n"
               "    t.x := 1\n"
               "    return^ t.x\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_CHANGES_TABLE);
    unit_dispose(&u);

    // The shape of the initialiser is what decides, which is what closes the
    // way round: naming a table again does not make a new one.
    LHAT_TEST("and binding it to a new name does not make it the body's");
    check_text(&u,
               "var^ f = f^ t:t^{ x:number^ } -> number^ {\n"
               "    var^ v = t\n"
               "    v.x := 1\n"
               "    return^ v.x\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_CHANGES_TABLE);
    unit_dispose(&u);

    // 05 の 8.6: L^ is the machine's own table, which no body made.
    LHAT_TEST("L^ is not a table any body made");
    check_text(&u,
               "var^ f = f^ -> number^ {\n"
               "    L^.modules := { }\n"
               "    return^ 0\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_CHANGES_TABLE);
    unit_dispose(&u);

    // 14.4: the receiver is the instance the caller handed over.
    LHAT_TEST("nor is the receiver of an f^ method");
    check_text(&u,
               "var^ P = def^{\n"
               "    self^{ x := 0 },\n"
               "    bump := f^self^ -> number^ { self^.x := 1 return^ 0 },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_CHANGES_TABLE);
    unit_dispose(&u);

    LHAT_TEST("a p^ changes whatever it is given");
    check_text(&u,
               "var^ g = p^ t:t^{ x:number^ } { t.x := 1 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // An f^ inside another measures against its own body here as well.
    LHAT_TEST("a nested f^ may not change what the enclosing one made");
    check_text(&u,
               "var^ outer = f^ -> number^ {\n"
               "    var^ u = { x := 0 }\n"
               "    var^ inner = f^ -> number^ { u.x := 1 return^ 0 }\n"
               "    return^ u.x\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_CHANGES_TABLE);
    unit_dispose(&u);

    // 14.12: an overloaded member is an intersection of signatures, and
    // whichever arm the call resolves to still answers to 15.1.
    LHAT_TEST("and through whichever overload^ arm the call resolves to");
    check_text(&u,
               "var^ A = def^{\n"
               "    self^{},\n"
               "    bar := p^self^ { },\n"
               "}\n"
               "var^ B = A .. def^{\n"
               "    self^{},\n"
               "    overload^ bar := p^self^, n:number^ { },\n"
               "}\n"
               "var^ o = B.new()\n"
               "var^ f = f^ { o.bar() }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_CALLS_PROCEDURE);
    unit_dispose(&u);
}

// 8.9: var^ binds a name a ':=' may reach and let^ one it may not. The rule is
// about the name and not about the value, which is what keeps it apart from
// 15.1改's question about a table and 05 の 8.6's about the machine's own.
static void test_immutable_bindings(void)
{
    Unit u;

    LHAT_TEST("var^ takes a reassignment");
    check_text(&u, "var^ x = 1\nx := 2\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and let^ refuses one");
    check_text(&u, "let^ x = 1\nx := 2\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ASSIGN_TO_LET);
    unit_dispose(&u);

    // The diagnostic says which name, since a statement may write several.
    LHAT_TEST("and says which name it was");
    check_text(&u, "let^ total = 0\ntotal := 1\n");
    {
        LHAT_CHECK(u.checked.diagnostic_count > 0, "expected a diagnostic");
        if (u.checked.diagnostic_count > 0) {
            const LhatCheckDiagnostic *d = &u.checked.diagnostics[0];
            LHAT_CHECK_EQ_INT(d->code, LHAT_CHECK_ERR_ASSIGN_TO_LET);
            LHAT_CHECK(d->name != NULL && d->name_length == 5, "total");
        }
    }
    unit_dispose(&u);

    // 7.4: a compound assignment is a ':=' spelled shorter, so it meets the
    // same rule rather than slipping past it.
    LHAT_TEST("a compound assignment is a reassignment too");
    check_text(&u, "let^ n = 1\nn += 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ASSIGN_TO_LET);
    unit_dispose(&u);

    // 8.7 makes the name visible throughout the scope, so a write standing
    // before the definition is judged by the same rule as one after it.
    LHAT_TEST("a write before the definition is refused as well");
    check_text(&u, "do^{ x := 2\nlet^ x = 1 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ASSIGN_TO_LET);
    unit_dispose(&u);

    // Shadowing is a second name, not a write to the first (8.7).
    LHAT_TEST("but shadowing in a nested scope is not a write");
    check_text(&u, "let^ i = 0\ndo^{ var^ i = 1\ni := 2 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 8.9: what let^ holds is still the value it was. A table it names keeps
    // its members writable -- that axis is 15.1改's and 05 の 8.6's.
    LHAT_TEST("a let^ table still has writable members");
    check_text(&u, "let^ t = { a := 1 }\nt.a := 2\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("while the name itself is not");
    check_text(&u, "let^ t = { a := 1 }\nt := { a := 2 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ASSIGN_TO_LET);
    unit_dispose(&u);

    // 15.1: a parameter is a name the body made, and 8.9 leaves it a var^ --
    // writing it is closed inside the frame and the caller never sees it.
    LHAT_TEST("a parameter is a var^");
    check_text(&u, "var^ f = f^ n:number^ -> number^ { n := n + 1\n"
                   "return^ n }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 01 の 8 章: a specifier says where to start looking, not what the name
    // may be written as.
    LHAT_TEST("a scope specifier does not get round it");
    check_text(&u, "let^ i = 0\ndo^{ $^i := 1 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ASSIGN_TO_LET);
    unit_dispose(&u);

    // 12 章: with^ holds a resource for the life of the block, and 12.2
    // disposes of what the name still holds at the end of it.
    LHAT_TEST("a with^ binding is a let^");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 1 },\n"
               "    dispose := p^self^ { },\n"
               "}\n"
               "with^ c = C.new() { c := C.new() }\n");
    // The writer chose no word here, so the diagnostic offers no var^.
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ASSIGN_TO_FORM);
    unit_dispose(&u);

    // 16.3改2 with 16.4: to^ and downto^ advance the focus themselves, so the
    // header writes neither word -- from^ opens the range and the name it
    // introduces is a let^.
    LHAT_TEST("a from^ focus is a let^");
    check_text(&u, "for^ i from^ 1 to^ 10 { var^ n = i }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the body may not write it");
    check_text(&u, "for^ i from^ 1 to^ 10 { i := 0 }\n");
    // 16.3改2 leaves no var^ to write here either, so this is the diagnostic
    // that does not offer one.
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ASSIGN_TO_FORM);
    unit_dispose(&u);

    // And it says so without pointing at a spelling that is itself refused:
    // 'for^ var^ i = 1 to^ 10' is an error of its own (16.3改2).
    LHAT_TEST("and the message offers no var^");
    check_text(&u, "for^ i from^ 1 to^ 10 { i := 0 }\n");
    if (u.checked.diagnostic_count > 0) {
        char message[256];
        lhat_check_message_write(&u.checked.diagnostics[0], message,
                                 sizeof message);
        LHAT_CHECK(strstr(message, "var^") == NULL,
                   "a from^ focus has no var^ spelling to suggest");
    }
    unit_dispose(&u);

    // 16.3: while^ asks the writer for the step, which a let^ focus refuses
    // -- the diagnostic lands on the next^ that cannot happen.
    LHAT_TEST("a while^ focus has to be a var^");
    check_text(&u, "for^ var^ i = 1 while^ i < 3 next^ i := i + 1 { }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a let^ one is refused at its next^");
    check_text(&u, "for^ let^ i = 1 while^ i < 3 next^ i := i + 1 { }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ASSIGN_TO_LET);
    unit_dispose(&u);

    // 16.3: in^ binds its focus afresh each turn from what the walk yields,
    // which is the walk's to say and not the body's.
    LHAT_TEST("an in^ focus is a let^");
    check_text(&u, "var^ t = { a := 1 }\nfor^ k, v in^ t { v := 2 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ASSIGN_TO_FORM);
    unit_dispose(&u);

    // 05 の 4 章 with 01 の 8.3: a name another unit could see change is the
    // global variable the language does not have.
    LHAT_TEST("public^ binds with let^");
    check_text(&u, "module^ m\npublic^ let^ answer = 42\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and refuses a var^");
    check_text(&u, "module^ m\npublic^ var^ answer = 42\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PUBLIC_IS_IMMUTABLE);
    unit_dispose(&u);

    // 03 の 4.3: a session redefinition is the same place written again, so
    // what the new input says about the name is what holds from there.
    LHAT_TEST("a session redefinition says which it is now");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_asked_text(&u, s, "let^ x = 1\n");
        CHECK_CLEAN(&u);
        unit_dispose(&u);
        check_asked_text(&u, s, "var^ x = 2\n");
        CHECK_CLEAN(&u);
        unit_dispose(&u);
        check_asked_text(&u, s, "x := 3\n");
        CHECK_CLEAN(&u);
        unit_dispose(&u);
        lhat_check_session_dispose(s);
    }

    LHAT_TEST("and the other way round");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_asked_text(&u, s, "var^ x = 1\n");
        CHECK_CLEAN(&u);
        unit_dispose(&u);
        check_asked_text(&u, s, "let^ x = 2\n");
        CHECK_CLEAN(&u);
        unit_dispose(&u);
        check_asked_text(&u, s, "x := 3\n");
        CHECK_REPORTS(&u, LHAT_CHECK_ERR_ASSIGN_TO_LET);
        unit_dispose(&u);
        lhat_check_session_dispose(s);
    }
}

int main(void)
{
    test_purity();
    test_immutable_bindings();
    return lhat_test_report("test_check_purity");
}
