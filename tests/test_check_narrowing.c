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

// 13.11, and 04 の 7 章 which rests entirely on it.
static void test_narrowing(void)
{
    Unit u;

    LHAT_TEST("the true branch keeps only the arms that fit");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ r = f()\n"
               "if^ r isa^ number^ { var^ n : number^ = r }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("the false branch keeps the rest");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ r = f()\n"
               "if^ r isa^ number^ {\n"
               "    else^:\n"
               "        var^ s : string^ = r\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("narrowing does not reach past what was tested");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ r = f()\n"
               "if^ r isa^ number^ { var^ s : string^ = r }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 13.11 with 04 の 11.3: a comparison against the nil^ literal narrows the
    // way isa^ does. Without it 'if^ t != nil^ { … }' passed the condition and
    // then reported against the *use* inside the branch -- a diagnostic
    // nowhere near its cause, on the first form anyone reaches for.
    LHAT_TEST("'!= nil^' narrows the true branch");
    check_text(&u,
               "var^ t : number^|nil^ = nil^\n"
               "if^ t != nil^ { var^ n : number^ = t }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("'= nil^' narrows the false branch");
    check_text(&u,
               "var^ t : number^|nil^ = nil^\n"
               "if^ t = nil^ {\n"
               "    else^:\n"
               "        var^ n : number^ = t\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // Either side may carry the literal.
    LHAT_TEST("the nil^ literal narrows from the left as well");
    check_text(&u,
               "var^ t : number^|nil^ = nil^\n"
               "if^ nil^ != t { var^ n : number^ = t }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // nil^ has one value, so 13.11's closing rule makes 'is^' and '=' agree
    // -- leaving is^ out would be a gap with nothing behind it.
    LHAT_TEST("'is^ nil^' narrows the same as '= nil^'");
    check_text(&u,
               "var^ t : number^|nil^ = nil^\n"
               "if^ t is^ nil^ {\n"
               "    else^:\n"
               "        var^ n : number^ = t\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a nil^ narrowing does not reach past what was tested");
    check_text(&u,
               "var^ t : number^|nil^ = nil^\n"
               "if^ t != nil^ { var^ n : nil^ = t }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 11.7改2: 'x?' is '!(x isa^ nil^)' written short, so it narrows
    // to exactly what that would -- the same machinery, one more spelling.
    LHAT_TEST("'?' narrows the true branch");
    check_text(&u,
               "var^ t : number^|nil^ = nil^\n"
               "if^ t? { var^ n : number^ = t }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and leaves the nil^ on the false side");
    check_text(&u,
               "var^ t : number^|nil^ = nil^\n"
               "if^ t? {\n"
               "    else^:\n"
               "        var^ n : nil^ = t\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("'!' over it swaps the sides as it does for isa^");
    check_text(&u,
               "var^ t : number^|nil^ = nil^\n"
               "if^ !(t?) { var^ n : nil^ = t }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("'?' answers bool^ wherever a value is wanted");
    check_text(&u,
               "var^ t : number^|nil^ = nil^\n"
               "var^ b : bool^ = t?\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 11.7改 takes the same posture for a '?.' whose target cannot be
    // absent: the answer is known, and saying so is not this checker's job.
    LHAT_TEST("'?' on something that cannot be absent is let through");
    check_text(&u,
               "var^ n : number^ = 1\n"
               "var^ b : bool^ = n?\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 04 の 6.1: narrowing to a kind is what makes its declared field visible.
    LHAT_TEST("a narrowed error kind shows its fields");
    check_text(&u,
               "errordef^ ParseError { Syntax { line : number^ }, Eof }\n"
               "var^ parse = f^ -> number^|ParseError { return^ 0 }\n"
               "var^ r = parse()\n"
               "if^ r isa^ ParseError.Syntax { var^ n : number^ = r.line }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("the field is not visible without narrowing");
    check_text(&u,
               "errordef^ ParseError { Syntax { line : number^ }, Eof }\n"
               "var^ parse = f^ -> number^|ParseError { return^ 0 }\n"
               "var^ r = parse()\n"
               "var^ n = r.line\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // 04 の 6.1 is written in the early-return style, so a branch that never
    // falls through has to leave its narrowing behind.
    LHAT_TEST("an exiting branch narrows what follows it");
    check_text(&u,
               "errordef^ ParseError { Syntax { line : number^ }, Eof }\n"
               "var^ parse = f^ -> number^|ParseError { return^ 0 }\n"
               "var^ use = f^ -> number^ {\n"
               "    var^ r = parse()\n"
               "    if^ r isa^ ParseError.Syntax { return^ 0 }\n"
               "    if^ r isa^ ParseError.Eof { return^ 0 }\n"
               "    return^ r + 1\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a branch that falls through leaves nothing behind");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ g = f^ -> number^ {\n"
               "    var^ r = f()\n"
               "    if^ r isa^ string^ { }\n"
               "    return^ r + 1\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    // 04 の 7 章: handling every kind is ordinary narrowing, so the success
    // type is what is left in the last clause.
    LHAT_TEST("an exhausted union leaves the success type");
    check_text(&u,
               "errordef^ IOError { NotFound, Denied }\n"
               "var^ open = f^ -> number^|IOError { return^ 0 }\n"
               "var^ use = f^ -> number^ {\n"
               "    var^ r = open()\n"
               "    if^ r isa^ IOError.NotFound {\n"
               "        return^ 0\n"
               "        elseif^ r isa^ IOError.Denied:\n"
               "            return^ 0\n"
               "        else^:\n"
               "            return^ r\n"
               "    }\n"
               "    return^ 0\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a union not exhausted still carries its errors");
    check_text(&u,
               "errordef^ IOError { NotFound, Denied }\n"
               "var^ open = f^ -> number^|IOError { return^ 0 }\n"
               "var^ use = f^ -> number^ {\n"
               "    var^ r = open()\n"
               "    if^ r isa^ IOError.NotFound {\n"
               "        return^ 0\n"
               "        else^:\n"
               "            return^ r\n"
               "    }\n"
               "    return^ 0\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("'!' turns the branches around");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ r = f()\n"
               "if^ !(r isa^ number^) { var^ s : string^ = r }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.11: and^ tells us both held; or^ says nothing when it is true.
    LHAT_TEST("and^ narrows both sides");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ g = f^ -> number^|string^ { return^ 0 }\n"
               "var^ a = f()\n"
               "var^ b = g()\n"
               "if^ a isa^ number^ and^ b isa^ number^ {\n"
               "    var^ n : number^ = a + b\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.11's "the right side of and^": it runs only where the left held, so
    // what the left tested is known there. Without it the one condition has
    // to be written as two nested if^, which is what anyone tries first.
    LHAT_TEST("and^ narrows its own right side");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ r = f()\n"
               "if^ r isa^ number^ and^ r <= 0.5 { var^ n : number^ = r }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The other half of the same rule: or^'s right side runs where the left
    // failed, so what holds there is the false side.
    LHAT_TEST("or^ narrows its right side with the false side");
    check_text(&u,
               "var^ t : number^|nil^ = nil^\n"
               "if^ t = nil^ or^ t <= 0.5 { var^ b = true^ }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // Short circuit is what carries it, so it only runs one way: the left is
    // evaluated whatever the right says.
    LHAT_TEST("and^ does not narrow the other way round");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ r = f()\n"
               "if^ r <= 0.5 and^ r isa^ number^ { var^ b = true^ }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_ORDERED);
    unit_dispose(&u);

    // Nothing about this belongs to if^ -- and^ is an expression, and the
    // narrowing lives exactly as long as it does.
    LHAT_TEST("and^ narrows outside a condition too");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ r = f()\n"
               "var^ ok = r isa^ number^ and^ r <= 0.5\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the narrowing does not outlive the expression");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ r = f()\n"
               "var^ ok = r isa^ number^ and^ r <= 0.5\n"
               "var^ n : number^ = r\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 13.11 with 9.2: a conditional loop tests before every turn, so the body
    // runs where the condition held -- the same ground an if^ body stands on.
    LHAT_TEST("a while^ loop narrows its body");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ r = f()\n"
               "repeat^ while^ r isa^ number^ { var^ n : number^ = r }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("an until^ loop narrows it with the false side");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ r = f()\n"
               "repeat^ until^ r isa^ string^ { var^ n : number^ = r }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and so does the for^ form of the same loop");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ r = f()\n"
               "for^ var^ i = 1 while^ r isa^ number^ next^ i := i + 1 {\n"
               "    var^ n : number^ = r\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.11: the write that ends a narrowing is not itself a mistake -- what
    // may be written is what the name holds. 13.11's own example does this,
    // and a loop that advances the value it tested does nothing else.
    LHAT_TEST("the write that ends a narrowing is allowed to widen it");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ r = f()\n"
               "if^ r isa^ number^ { r := f() }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The value stands to the right of the write, so it is read where the
    // narrowing still holds.
    LHAT_TEST("and the value it writes is still narrowed");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ r = f()\n"
               "if^ r isa^ number^ { r := r + 1 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a written member reads the same way");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ t = { a := f() }\n"
               "if^ t.a isa^ number^ { t.a := f() }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.11: a loop advancing the value its own condition tested is the
    // shape all of this has to hold up under.
    LHAT_TEST("which is what lets a loop advance what it tested");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ r = f()\n"
               "repeat^ while^ r isa^ number^ {\n"
               "    var^ n : number^ = r + 1\n"
               "    r := f()\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 9.2: first^ runs after the test, so it stands where main^ does.
    LHAT_TEST("first^ is on the far side of the test too");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ r = f()\n"
               "repeat^ while^ r isa^ number^ { first^: var^ n : number^ = r }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 9.10: pre^ runs before the condition is tested, and runs once even
    // where it never holds -- so the condition says nothing there.
    LHAT_TEST("pre^ runs before the test and is not narrowed");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ r = f()\n"
               "repeat^ while^ r isa^ number^ { pre^: var^ n : number^ = r }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 9.1: last^ runs once the loop is over, which for a while^ is where the
    // condition has just failed.
    LHAT_TEST("nor is last^, which runs once the condition failed");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ r = f()\n"
               "repeat^ while^ r isa^ number^ { last^: var^ n : number^ = r }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 02 の 9.4: the body has two layers of lifetime, and the walk follows
    // 9.2's order rather than the order the parser stored the clauses in.
    // main^ used to be walked before every clause, so nothing it read could
    // see what prolog^ had declared -- 9.4's own example did not check.
    LHAT_TEST("main^ sees what prolog^ declared");
    check_text(&u,
               "repeat^ 3 { prolog^: var^ total = 0  main^: total += 1 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and what first^ declared");
    check_text(&u,
               "repeat^ 3 { first^: var^ total = 0  main^: total += 1 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 9.4 puts pre^ and main^ in one layer, and 9.2 runs pre^ first.
    LHAT_TEST("and what pre^ declared, which runs in the same turn");
    check_text(&u,
               "repeat^ 3 { pre^: var^ total = 0  main^: total += 1 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 9.4: prolog^ and first^ last the whole loop, so the clauses that run
    // once it is over still reach them.
    LHAT_TEST("last^ and epilog^ see the whole-loop layer");
    check_text(&u,
               "repeat^ 3 { prolog^: var^ a = 0  first^: var^ b = 0\n"
               "  main^: a += 1  last^: a += b  epilog^: a += b }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // And the other half of 9.4: what main^ declares lives one iteration, so
    // it is gone by the time last^ and epilog^ run. The compiler has always
    // refused this; the checker used to let it through.
    LHAT_TEST("but not what main^ declared, which lives one iteration");
    check_text(&u,
               "repeat^ 3 { main^: var^ a = 0  epilog^: var^ b = a }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
    unit_dispose(&u);

    LHAT_TEST("nor from last^");
    check_text(&u,
               "repeat^ 3 { main^: var^ a = 0  last^: var^ b = a }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
    unit_dispose(&u);

    LHAT_TEST("and pre^ is in that same one-iteration layer");
    check_text(&u,
               "repeat^ 3 { pre^: var^ a = 0  main^: a += 1"
               "  epilog^: var^ b = a }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
    unit_dispose(&u);

    // 01 の 8 章: the writer put down one '{', so '$^' finds one step out of
    // the body whether or not clauses were written in it. The layer holding
    // prolog^ is not a scope anyone wrote, and compile.c counts one too.
    LHAT_TEST("'$^' counts the body once, clauses or not");
    check_text(&u,
               "var^ x = 1\n"
               "repeat^ 2 { prolog^: var^ p = 0  main^: var^ x = 2\n"
               "  var^ n : number^ = $^x }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 9.8: a break^ leaves from anywhere, so what ended the loop is not
    // known after it.
    LHAT_TEST("and the narrowing does not outlive the loop");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ r = f()\n"
               "repeat^ while^ r isa^ number^ { var^ b = true^ }\n"
               "var^ n : number^ = r\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // Narrowing reads the path a second time, after the condition was
    // inferred in full. What is wrong with it was said there.
    LHAT_TEST("a mistake in a narrowing condition is reported once");
    check_text(&u, "if^ nowhere isa^ number^ { var^ b = true^ }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
    LHAT_CHECK_EQ_INT(u.checked.diagnostic_count, 1);
    unit_dispose(&u);

    // 13.11: only a name or a dot path from one, since a call may give a
    // different value the second time.
    LHAT_TEST("a call is not narrowed");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ g = f^ -> number^ {\n"
               "    if^ f() isa^ number^ { return^ f() + 1 }\n"
               "    return^ 0\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    LHAT_TEST("a dot path is narrowed");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ t = { a := f() }\n"
               "if^ t.a isa^ number^ { var^ n : number^ = t.a }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.11: reassigning ends it, since the claim was about what was examined.
    LHAT_TEST("a reassignment ends the narrowing");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ r = f()\n"
               "if^ r isa^ number^ {\n"
               "    r := f()\n"
               "    var^ n : number^ = r\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("narrowing works in the if expression too");
    check_text(&u,
               "var^ f = f^ -> number^|string^ { return^ 0 }\n"
               "var^ r = f()\n"
               "var^ n : number^ = if^ r isa^ number^: r el^: 0 ;\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 11.6改: as^ is sound -- a pair no value can inhabit both sides of is
    // refused at check time, not left to panic.
    LHAT_TEST("as^ between disjoint types is refused at check time");
    check_text(&u,
               "var^ n = 1\n"
               "var^ s = n as^ string^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_AS_IMPOSSIBLE);
    unit_dispose(&u);

    // 11.6改2: the safe form answers T|nil^, so what it hands back has the
    // nil^ arm to deal with -- '??' is the spelling, as for every other
    // T|nil^ (11.7).
    LHAT_TEST("as^? answers T|nil^");
    check_text(&u,
               "var^ f = f^ -> any^ { return^ 1 }\n"
               "var^ n : number^ = f() as^? number^ ?? 0\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the nil^ arm is not skipped");
    check_text(&u,
               "var^ f = f^ -> any^ { return^ 1 }\n"
               "var^ n : number^ = f() as^? number^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // Disjoint is disjoint either way: the safe form written there always
    // answers nil^, which is as dead as the stopping form is impossible.
    LHAT_TEST("as^? between disjoint types is refused too");
    check_text(&u,
               "var^ n = 1\n"
               "var^ s = n as^? string^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_AS_IMPOSSIBLE);
    unit_dispose(&u);
}

// 04 の 11.4 with 03 の 3.5: relaxed steps past nil^ in a union and lets
// the machine's own instruction check answer at run time; strict keeps
// refusing, since narrowing is the spelling there. No check is inserted --
// 3.5 withdrew that -- so what these pin is only the checker's posture.
static void test_relaxed_nil_reference(void)
{
    Unit u;

    LHAT_TEST("relaxed lets a T|nil^ member reference through");
    check_relaxed_text(&u,
                       "var^ f = f^ -> t^{ a : number^ }|nil^ { return^ nil^ }\n"
                       "var^ t = f()\n"
                       "var^ x : number^ = t.a\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and strict still refuses the same reference");
    check_text(&u,
               "var^ f = f^ -> t^{ a : number^ }|nil^ { return^ nil^ }\n"
               "var^ t = f()\n"
               "var^ x : number^ = t.a\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // Only nil^ is stepped past: two real types still have no one member
    // type to answer with, whichever the strictness.
    LHAT_TEST("relaxed does not step past a union of two real types");
    check_relaxed_text(&u,
                       "var^ f = f^ -> t^{ a : number^ }|number^ { return^ 1 }\n"
                       "var^ t = f()\n"
                       "var^ x = t.a\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);
}

// 04 の 11.4 with 01 の 7.1: the written spelling for reaching through
// a T|nil^, which 11.4 names beside narrowing. The target is read without
// its nil^ arm and the answer gains one, so what these pin is both halves --
// the access is admitted under strict, and its result still says it may be
// absent.
static void test_nil_propagation(void)
{
    Unit u;

    LHAT_TEST("'?.' reaches through a T|nil^ under strict");
    check_text(&u,
               "var^ f = f^ -> t^{ a : number^ }|nil^ { return^ nil^ }\n"
               "var^ t = f()\n"
               "var^ x : number^|nil^ = t?.a\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the answer carries the nil^ it may produce");
    check_text(&u,
               "var^ f = f^ -> t^{ a : number^ }|nil^ { return^ nil^ }\n"
               "var^ t = f()\n"
               "var^ x : number^ = t?.a\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 11.7: which is what '??' is for -- the pair is the whole idiom.
    LHAT_TEST("'??' takes the nil^ back off again");
    check_text(&u,
               "var^ f = f^ -> t^{ a : number^ }|nil^ { return^ nil^ }\n"
               "var^ t = f()\n"
               "var^ x : number^ = t?.a ?? 0\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("'?[' reaches through one the same way");
    check_text(&u,
               "var^ f = f^ -> t^{ ...:string^ }|nil^ { return^ nil^ }\n"
               "var^ t = f()\n"
               "var^ s : string^ = t?[1] ?? \"\"\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("'?(' reaches through an absent callee");
    check_text(&u,
               "var^ f : (f^number^ -> number^;)|nil^ = nil^\n"
               "var^ n : number^ = f?(1) ?? 0\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 11.7改2: the first '?' guards the rest of the run, so the links after
    // it are written plain. The nil^ arm goes on once, at the end.
    LHAT_TEST("one '?' guards the rest of the chain");
    check_text(&u,
               "var^ f = f^ -> t^{ a : t^{ b : number^ } }|nil^ { return^ nil^ }\n"
               "var^ t = f()\n"
               "var^ n : number^ = t?.a.b ?? 0\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A call belongs to the run the same way a member does, which is what
    // makes 'a?.b()' enough: the member it reached is always there, and the
    // one nil^ in play is the one the '?' already accounted for.
    LHAT_TEST("and a call written on it needs no '?' of its own");
    check_text(&u,
               "var^ f = f^ -> t^{ a : (f^ -> number^;) }|nil^ { return^ nil^ }\n"
               "var^ t = f()\n"
               "var^ n : number^ = t?.a() ?? 0\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // What the guard covers is the nil^ it was written for. One the writer's
    // own type carries is met further in and still wants a '?' of its own.
    LHAT_TEST("but a nil^ met further in is refused as ever");
    check_text(&u,
               "var^ f = f^ -> t^{ a : t^{ b : number^ }|nil^ }|nil^ "
               "{ return^ nil^ }\n"
               "var^ t = f()\n"
               "var^ n : number^ = t?.a.b ?? 0\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // '(' makes no node of its own, so a bracket a writer put there to group
    // is not a place a chain ends -- unlike JavaScript, where '(a?.b).c' is a
    // second reading of the same text.
    LHAT_TEST("and a bracket does not end the chain");
    check_text(&u,
               "var^ f = f^ -> t^{ a : t^{ b : number^ } }|nil^ { return^ nil^ }\n"
               "var^ t = f()\n"
               "var^ n : number^ = (t?.a).b ?? 0\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and marking every link is accepted");
    check_text(&u,
               "var^ f = f^ -> t^{ a : t^{ b : number^ } }|nil^ { return^ nil^ }\n"
               "var^ t = f()\n"
               "var^ n : number^ = t?.a?.b ?? 0\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.11: `narrowable` refuses a '?.' path, so a narrowing recorded for
    // one name never leaks onto the nil-safe reach through it.
    LHAT_TEST("a '?.' path is not narrowed");
    check_text(&u,
               "var^ f = f^ -> t^{ a : number^|nil^ }|nil^ { return^ nil^ }\n"
               "var^ t = f()\n"
               "if^ t?.a != nil^ { var^ n : number^ = t?.a }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);
}

// 04 の 11.3 makes everything a key reads out of a table a 'T|nil^', so the
// commonest reason an operator finds nothing is the nil^ arm. What that asks
// of the writer is a narrowing, and 11.3改's rule -- which is what the plain
// refusal recites -- reads as a demand for an operator instead. Every entrance
// to the refusal is pinned here, since one left saying the old thing is one
// the writer will meet.
static void test_operator_on_maybe_nil(void)
{
    Unit u;

    LHAT_TEST("the receiver may be nil^");
    check_text(&u, "var^ f = f^ -> t^{ ...:number^ } { return^ { 1 } }\n"
                   "var^ t = f()\n"
                   "var^ n : number^ = t[1] + 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL);
    unit_dispose(&u);

    // 11.3改 hands this one to the right operand, which is where a built-in
    // on the left leaves it -- the same mistake read from the other end.
    LHAT_TEST("the argument may be nil^");
    check_text(&u, "var^ f = f^ -> t^{ ...:number^ } { return^ { 1 } }\n"
                   "var^ t = f()\n"
                   "var^ n : number^ = 1 + t[1]\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL);
    unit_dispose(&u);

    LHAT_TEST("an ordering says it too, rather than 'nothing orders these'");
    check_text(&u, "var^ f = f^ -> t^{ ...:number^ } { return^ { 1 } }\n"
                   "var^ t = f()\n"
                   "var^ b : bool^ = t[1] < 3\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL);
    unit_dispose(&u);

    LHAT_TEST("and the unary '-', rather than 'arithmetic needs number^'");
    check_text(&u, "var^ f = f^ -> t^{ ...:number^ } { return^ { 1 } }\n"
                   "var^ t = f()\n"
                   "var^ n : number^ = -t[1]\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL);
    unit_dispose(&u);

    // 11.2's '..' is carried by string^ rather than by 14.8's number^, so it
    // reaches the refusal by a different road and has to be asked separately.
    LHAT_TEST("'..' is the same mistake");
    check_text(&u, "var^ f = f^ -> t^{ ...:string^ } { return^ { \"a\" } }\n"
                   "var^ t = f()\n"
                   "var^ s : string^ = t[1] .. \"a\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL);
    unit_dispose(&u);

    // The two ways out the message names.
    LHAT_TEST("'??' is one of them");
    check_text(&u, "var^ f = f^ -> t^{ ...:number^ } { return^ { 1 } }\n"
                   "var^ t = f()\n"
                   "var^ n : number^ = (t[1] ?? 0) + 1\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("binding it to a name and narrowing that is the other");
    check_text(&u, "var^ f = f^ -> t^{ ...:number^ } { return^ { 1 } }\n"
                   "var^ t = f()\n"
                   "var^ v = t[1]\n"
                   "if^ v isa^ number^ { var^ n : number^ = v + 1 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.11 excludes an index from what may be narrowed, which is why the
    // message says to bind it first -- without that line a writer tries this
    // and meets the same refusal a second time.
    LHAT_TEST("narrowing the index where it stands does not do it");
    check_text(&u, "var^ f = f^ -> t^{ ...:number^ } { return^ { 1 } }\n"
                   "var^ t = f()\n"
                   "if^ t[1] isa^ number^ { var^ n : number^ = t[1] + 1 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL);
    unit_dispose(&u);

    // Only where dropping the nil^ is what would have made it work. An
    // operator nobody wrote is still that, and saying "narrow it" of one
    // would send the writer after a fix that is not there.
    LHAT_TEST("with no nil^ in it, the plain refusal stands");
    check_text(&u, "var^ b = true^\n"
                   "var^ n : number^ = b + 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    LHAT_TEST("nor when the rest of the union has no operator either");
    check_text(&u, "var^ f = f^ -> string^|nil^ { return^ \"a\" }\n"
                   "var^ n : number^ = f() + 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);
}

// 01 の 2.3: the stacked reach, typed. The checker walks the same
// bindings the machine resolves, so it^^ carries the outer focus's type and
// this^^ the enclosing subroutine's own.
static void test_stacked_hats(void)
{
    Unit u;

    LHAT_TEST("it^^ carries the enclosing focus's type");
    check_text(&u,
               "for^ 1 to^ 2 {\n"
               "  for^ 10 to^ 11 { var^ n : number^ = it^^ + it^ }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("self^^ carries the enclosing receiver's members");
    check_text(&u,
               "var^ Outer = def^{ self^{ x := 7 },\n"
               "  m := f^self^ -> number^ {\n"
               "    var^ Inner = def^{ self^{ y := 1 },\n"
               "      n := f^self^ -> number^ { return^ self^^.x + self^.y }\n"
               "    }\n"
               "    return^ Inner.new().n()\n"
               "  }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a member the outer receiver lacks is reported");
    check_text(&u,
               "var^ Outer = def^{ self^{ x := 7 },\n"
               "  m := f^self^ -> number^ {\n"
               "    var^ Inner = def^{ self^{ y := 1 },\n"
               "      n := f^self^ -> number^ { return^ self^^.y }\n"
               "    }\n"
               "    return^ Inner.new().n()\n"
               "  }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    LHAT_TEST("this^^ is the enclosing subroutine's own type");
    check_text(&u,
               "var^ outer = p^ {\n"
               "  var^ inner = p^ { var^ q = this^^ }\n"
               "  inner()\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("counting past the bindings there are is reported");
    check_text(&u, "for^ 1 to^ 2 { var^ x = it^^ }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_SCOPE_TOO_FAR);
    unit_dispose(&u);

    LHAT_TEST("and so is this^^ in an unnested body");
    check_text(&u, "var^ f = p^ { var^ q = this^^ }\nf()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_SCOPE_TOO_FAR);
    unit_dispose(&u);
}

// 13.8改: (A, B) is a type. What keeps 13.8's four propagations from coming
// back is where it may be written rather than what it is -- a result and
// nowhere else. These cases are that confinement.
//
// The bodies below are deliberately wrong (a tuple result answered with 0),
// because a right one cannot be written until return^ takes several values.
// What each case asserts is the one refusal it is about.
static void test_tuple_positions(void)
{
    Unit u;

    LHAT_TEST("a tuple may be written as a result");
    check_text(&u, "var^ g : f^ -> (number^, string^); = 0\n");
    CHECK_NOT_REPORTED(&u, LHAT_CHECK_ERR_TUPLE_MISPLACED);
    unit_dispose(&u);

    // 13.7's expansion rule -- Lua's "truncate to one except in tail
    // position" -- is what 13.8 named as the worst of the four. It has
    // nothing to expand once an argument cannot hold a tuple.
    LHAT_TEST("but not as an argument");
    check_text(&u, "var^ g : f^(number^, string^) -> number^; = 0\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TUPLE_MISPLACED);
    unit_dispose(&u);

    LHAT_TEST("nor bound to a name");
    check_text(&u, "var^ t : (number^, string^) = 0\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TUPLE_MISPLACED);
    unit_dispose(&u);

    LHAT_TEST("nor held in a table");
    check_text(&u, "var^ t : t^{ a : (number^, string^) } = { }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TUPLE_MISPLACED);
    unit_dispose(&u);

    // A position is a slot, not a run of slots.
    LHAT_TEST("nor nested in another tuple");
    check_text(&u, "var^ g : f^ -> ((number^, string^), number^); = 0\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TUPLE_MISPLACED);
    unit_dispose(&u);

    // 04 の 3.1: the error goes around the values, never among them.
    LHAT_TEST("a union with an error is how a failing one is written");
    check_text(&u,
               "errordef^ IOError { NotFound }\n"
               "var^ g : f^ -> (number^, string^)|IOError; = 0\n");
    CHECK_NOT_REPORTED(&u, LHAT_CHECK_ERR_TUPLE_UNION);
    unit_dispose(&u);

    // 04 の 8.2: an error among the values is the shape Go's 'v, err := f()'
    // needs, and it stays unwritable.
    LHAT_TEST("and a union with anything else is refused");
    check_text(&u, "var^ g : f^ -> (number^, string^)|number^; = 0\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TUPLE_UNION);
    unit_dispose(&u);

    // Multi-value return brings back the second value 8.2 relies on not
    // existing among the values, so that rule is stated outright.
    LHAT_TEST("an error written among the values is refused");
    check_text(&u,
               "errordef^ IOError { NotFound }\n"
               "var^ g : f^ -> (number^, IOError); = 0\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TUPLE_ERROR_POSITION);
    unit_dispose(&u);

    LHAT_TEST("and so is one buried in a position's union");
    check_text(&u,
               "errordef^ IOError { NotFound }\n"
               "var^ g : f^ -> (number^, string^|IOError); = 0\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TUPLE_ERROR_POSITION);
    unit_dispose(&u);

    // 13.9 with 15.3改: Y and T are results, R is an input.
    LHAT_TEST("a coroutine yields and answers tuples");
    check_text(&u,
               "var^ c : c^{ p^number^ -> (number^, string^) -> "
               "(bool^, number^) } = 0\n");
    CHECK_NOT_REPORTED(&u, LHAT_CHECK_ERR_TUPLE_MISPLACED);
    unit_dispose(&u);

    // 13.8改: and R may be a tuple too -- resume(a, b) sends that many, and
    // the yield^'s own binding takes them apart. Both spellings of R name
    // the same type: '(A, B)' as one parameter, or the parameters written
    // out.
    LHAT_TEST("and a resume may send several -- a tuple R");
    check_text(&u,
               "var^ c : c^{ p^(number^, string^) -> number^ -> number^ } = 0\n");
    CHECK_NOT_REPORTED(&u, LHAT_CHECK_ERR_TUPLE_MISPLACED);
    unit_dispose(&u);

    LHAT_TEST("written either as a tuple or as the parameter list");
    check_text(&u,
               "var^ c : c^{ p^number^, string^ -> number^ -> number^ } = 0\n");
    CHECK_NOT_REPORTED(&u, LHAT_CHECK_ERR_TUPLE_MISPLACED);
    unit_dispose(&u);

    // 04 の 4.1: catch^'s right side is one expression, and 13.8改 leaves no
    // expression that is a tuple. So a failing subroutine answering several
    // values cannot be given a replacement here -- the union of the tuple
    // with whatever was written is what gets reported.
    // 13.8改 (S48): the literal is what a failing multi-value operation is
    // given as a replacement. Before it there was no expression that was a
    // tuple, so catch^ had nothing to offer one.
    LHAT_TEST("catch^ supplies a replacement tuple");
    check_text(&u,
               "errordef^ E { X }\n"
               "var^ f = f^ -> (number^, number^)|E { return^ error^E.X{} }\n"
               "var^ q, r = f() catch^ (0, 1)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("but not one of the wrong width");
    check_text(&u,
               "errordef^ E { X }\n"
               "var^ f = f^ -> (number^, number^)|E { return^ error^E.X{} }\n"
               "var^ q, r = f() catch^ (0, 1, 2)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TUPLE_UNION);
    unit_dispose(&u);

    LHAT_TEST("nor a single value");
    check_text(&u,
               "errordef^ E { X }\n"
               "var^ f = f^ -> (number^, number^)|E { return^ error^E.X{} }\n"
               "var^ q, r = f() catch^ 0\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TUPLE_UNION);
    unit_dispose(&u);

    // 11.7: '??' is the same shape as catch^, so it takes the same
    // replacement -- 04 の 4.1 pairs them, and nothing here tells them apart.
    LHAT_TEST("?? takes a replacement tuple too");
    check_text(&u,
               "var^ f = f^ -> (number^, number^)|nil^ { return^ nil^ }\n"
               "var^ q, r = f() ?? (0, 1)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and refuses the wrong width the same way");
    check_text(&u,
               "var^ f = f^ -> (number^, number^)|nil^ { return^ nil^ }\n"
               "var^ q, r = f() ?? (0, 1, 2)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TUPLE_UNION);
    unit_dispose(&u);

    // 13.8改: two tuples of the same width fold position by position, so the
    // answer stays a tuple and can be taken apart. Where both sides are the
    // same type the arm folding already collapsed them -- these are the
    // cases that showed the fold was missing.
    LHAT_TEST("positions of different types still fold into one tuple");
    check_text(&u,
               "var^ f = f^ -> (number^, number^)|nil^ { return^ nil^ }\n"
               "var^ q, r = f() ?? (nil^, nil^)\n"
               "var^ n : number^|nil^ = q\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the same holds through catch^");
    check_text(&u,
               "errordef^ E { X }\n"
               "var^ f = f^ -> (number^, string^)|E { return^ error^E.X{} }\n"
               "var^ q, r = f() catch^ (nil^, nil^)\n"
               "var^ s : string^|nil^ = r\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The grouping parentheses 13.1's grammar already had. This is what
    // leaves a one-position tuple unwritable without inventing '(T,)'.
    LHAT_TEST("'(T)' is still the grouping it always was");
    check_text(&u, "var^ x : (number^) = 1\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The value side reads '(' the same way, so the same thing holds there.
    LHAT_TEST("and '(x)' is still the grouping on the value side");
    check_text(&u, "var^ x : number^ = (1)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.8改: a literal is a tuple wherever it stands, so every rule about
    // where one may be written applies to it without being restated.
    LHAT_TEST("a literal may not be bound to a name");
    check_text(&u, "var^ t = (1, 2)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TUPLE_MISPLACED);
    unit_dispose(&u);

    // 13.7's expansion rule is what this keeps from arising.
    LHAT_TEST("nor passed as an argument");
    check_text(&u,
               "var^ g = p^ x:number^ { var^ y = x }\n"
               "g((1, 2))\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TUPLE_MISPLACED);
    unit_dispose(&u);

    LHAT_TEST("nor held in a table");
    check_text(&u, "var^ t = { (1, 2) }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TUPLE_MISPLACED);
    unit_dispose(&u);

    LHAT_TEST("nor nested in another literal");
    check_text(&u,
               "var^ f = f^ -> ((number^, number^), number^) { return^ 0 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TUPLE_MISPLACED);
    unit_dispose(&u);

    // 04 の 8.2, asked of a literal the way it is asked of 'return^ a, b'.
    LHAT_TEST("nor carrying an error in a position");
    check_text(&u,
               "errordef^ E { X }\n"
               "var^ f = f^ -> (number^, number^)|E { return^ error^E.X{} }\n"
               "var^ q, r = f() catch^ (0, error^E.X{})\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TUPLE_ERROR_POSITION);
    unit_dispose(&u);

    // 13.8改: the names on the left take every position or the checker
    // refuses -- strict never leaves the count to the machine.
    LHAT_TEST("destructuring must take every position");
    check_text(&u,
               "var^ both = f^ -> (number^, number^) { return^ 1, 2 }\n"
               "var^ a, b, c = both()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TUPLE_ARITY);
    unit_dispose(&u);
}

// 13.11改: a branch knows which whole numbers a name may hold, the way it
// knows which arms of a union it may be. 14.10 makes a table type "at least
// these", so a key that cannot leave the declared positions cannot find
// nothing -- and 04 の 11.3's nil^ does not come with the answer.
//
// Two forms say it and no others: the driven for^ and an ordering against a
// number written out. Nothing carries a bound through arithmetic, which is
// what keeps this a narrowing rather than an analysis of its own.
static void test_bounded_keys(void)
{
    Unit u;

    LHAT_TEST("a driven loop bounds its focus");
    check_text(&u,
               "var^ f = p^ t:t^{ number^[9] } {\n"
               "    for^ i from^1 to^9 { var^ n : number^ = t[i] }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a loop reaching past them does not");
    check_text(&u,
               "var^ f = p^ t:t^{ number^[3] } {\n"
               "    for^ i from^1 to^9 { var^ n : number^ = t[i] }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("nor one starting before the first position");
    check_text(&u,
               "var^ f = p^ t:t^{ number^[9] } {\n"
               "    for^ i from^0 to^9 { var^ n : number^ = t[i] }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("downto^ counts the same two ends");
    check_text(&u,
               "var^ f = p^ t:t^{ number^[9] } {\n"
               "    for^ i from^9 downto^1 { var^ n : number^ = t[i] }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 11.5 の (5): the chain is the two links written as one, and the operand
    // they share is bounded at both ends at once.
    LHAT_TEST("a chain bounds the name between its ends");
    check_text(&u,
               "var^ f = p^ t:t^{ number^[9] }, d:number^ {\n"
               "    if^ 1 <= d <= 9 { var^ n : number^ = t[d] }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // Each half pushes one end, and what the branch knows is what both said.
    LHAT_TEST("and two orderings joined by and^ meet in the middle");
    check_text(&u,
               "var^ f = p^ t:t^{ number^[9] }, d:number^ {\n"
               "    if^ d >= 1 and^ d <= 9 { var^ n : number^ = t[d] }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("one end alone leaves the other open");
    check_text(&u,
               "var^ f = p^ t:t^{ number^[9] }, d:number^ {\n"
               "    if^ 1 <= d { var^ n : number^ = t[d] }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // What an ordering denies is the ordering the other way, so both branches
    // know something -- which is what makes the guard written as an early
    // exit bound the rest of the body.
    LHAT_TEST("a guard that exits bounds what it let through");
    check_text(&u,
               "var^ f = p^ t:t^{ number^[9] }, d:number^ {\n"
               "    if^ d < 1 or^ d > 9 { return^ }\n"
               "    var^ n : number^ = t[d]\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the else of one knows the same");
    check_text(&u,
               "var^ f = p^ t:t^{ number^[9] }, d:number^ {\n"
               "    if^ d < 1 or^ d > 9 { else^: var^ n : number^ = t[d] }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a guard denying one end alone is not enough");
    check_text(&u,
               "var^ f = p^ t:t^{ number^[9] }, d:number^ {\n"
               "    if^ d < 1 { return^ }\n"
               "    var^ n : number^ = t[d]\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("what a branch knew is not known after it");
    check_text(&u,
               "var^ f = p^ t:t^{ number^[9] }, d:number^ {\n"
               "    if^ 1 <= d <= 9 { }\n"
               "    var^ n : number^ = t[d]\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 13.11: a write ends what the branch established, bounds as much as
    // arms -- chk_drop_narrowings_for reaches both by the path.
    LHAT_TEST("nor after the name is written to");
    check_text(&u,
               "var^ f = p^ t:t^{ number^[9] } {\n"
               "    var^ d = 1\n"
               "    if^ 1 <= d <= 9 { d := 40  var^ n : number^ = t[d] }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("a bound does not survive arithmetic");
    check_text(&u,
               "var^ f = p^ t:t^{ number^[9] } {\n"
               "    for^ i from^1 to^8 { var^ n : number^ = t[i + 1] }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // A limit read off a name says no number here. 14.10's width subtyping
    // puts no ceiling on a length either, so there is nothing to read.
    LHAT_TEST("and an end that is not written out is no end");
    check_text(&u,
               "var^ f = p^ t:t^{ number^[9] }, n:number^ {\n"
               "    for^ i from^1 to^ n { var^ x : number^ = t[i] }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // The positions keep their own types, so a bound reaching two of them
    // answers what either may be.
    LHAT_TEST("a bound over positions of two types answers both");
    check_text(&u,
               "var^ f = p^ t:t^{ number^, string^ }, d:number^ {\n"
               "    if^ 1 <= d <= 2 { var^ n : number^|string^ = t[d] }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 8.6.4's spelling is what a place with a nil^ arm needs; a bounded one
    // has none, so the plain compound assignment is written.
    LHAT_TEST("a bounded place takes a plain compound assignment");
    check_text(&u,
               "var^ f = p^ t:t^{ number^[9] }, d:number^ {\n"
               "    if^ 1 <= d <= 9 { t[d] += 1 }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The two readings share one list and neither hides the other.
    LHAT_TEST("a type narrowing and a bound stand together");
    check_text(&u,
               "var^ f = p^ t:t^{ number^[9] }, d:number^|string^ {\n"
               "    if^ d isa^ number^ and^ 1 <= d <= 9 {\n"
               "        var^ n : number^ = t[d]\n"
               "    }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

// 17.2 with 13.11: a match arm is the if-chain it desugars to, so an isa^
// pattern narrows what the arm reads -- the bare-name subject by its own
// name, a wider subject through it^. (Neither narrowed at all before this
// was pinned: the desugar tested a FOCUS node the narrowing machinery did
// not know, and a bare name was wrapped in one besides.)
static void test_match_arm_narrowing(void)
{
    Unit u;

    LHAT_TEST("a match arm narrows its bare-name subject");
    check_text(&u,
               "let^ g = f^ x:number^|t^{ ...:number^ } -> number^ {\n"
               "    return^ for^x:\n"
               "    when^ isa^ t^{}: x.count^\n"
               "    other^: 0\n"
               "    ;\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and in the statement form too");
    check_text(&u,
               "let^ g = f^ x:number^|t^{ ...:number^ } -> number^ {\n"
               "    for^x {\n"
               "    when^ isa^ t^{}:\n"
               "        return^ x.count^\n"
               "    }\n"
               "    return^ 0\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a wider subject narrows through it^");
    check_text(&u,
               "let^ pick = f^ -> number^|t^{ ...:number^ } { return^ 4 }\n"
               "let^ g = f^ -> number^ {\n"
               "    return^ for^ pick():\n"
               "    when^ isa^ t^{}: it^.count^\n"
               "    other^: 0\n"
               "    ;\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

int main(void)
{
    test_narrowing();
    test_match_arm_narrowing();
    test_relaxed_nil_reference();
    test_nil_propagation();
    test_bounded_keys();
    test_operator_on_maybe_nil();
    test_stacked_hats();
    test_tuple_positions();
    return lhat_test_report("test_check_narrowing");
}
