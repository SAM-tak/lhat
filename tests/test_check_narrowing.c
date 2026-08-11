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

    // Per link, not per chain (Kotlin reads the same way): 'a?.b' is still a
    // T|nil^, and 11.4 refuses to reach through one, so every link carries
    // its own '?'. Nothing has to know where a chain begins or ends.
    LHAT_TEST("a chain marks every link");
    check_text(&u,
               "var^ f = f^ -> t^{ a : t^{ b : number^ } }|nil^ { return^ nil^ }\n"
               "var^ t = f()\n"
               "var^ n : number^ = t?.a.b ?? 0\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
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
               "var^ c : c^{ p^number^ -> (number^, string^);, "
               "(bool^, number^) } = 0\n");
    CHECK_NOT_REPORTED(&u, LHAT_CHECK_ERR_TUPLE_MISPLACED);
    unit_dispose(&u);

    LHAT_TEST("but a resume sends one value");
    check_text(&u,
               "var^ c : c^{ p^(number^, string^) -> number^;, number^ } = 0\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TUPLE_MISPLACED);
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

int main(void)
{
    test_narrowing();
    test_relaxed_nil_reference();
    test_nil_propagation();
    test_stacked_hats();
    test_tuple_positions();
    return lhat_test_report("test_check_narrowing");
}
