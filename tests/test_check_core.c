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

static void test_names(void)
{
    Unit u;

    LHAT_TEST("a definition binds and a reassignment finds it");
    check_text(&u, "var^ x = 1\nx := 2\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("an unknown name is reported");
    check_text(&u, "y := 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
    unit_dispose(&u);

    // 8.6: the accident var^ was introduced to remove. The inner statement
    // reassigns the outer name rather than making a second one.
    LHAT_TEST("':=' in a nested scope reaches the outer name");
    check_text(&u, "var^ i = 0\ndo^{ i := 1 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("shadowing gets its own binding");
    check_text(&u, "var^ i = 0\ndo^{ var^ i = \"text\" }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 8.7: twice in one scope is an error; the nested case above is not.
    LHAT_TEST("the same scope may not define a name twice");
    check_text(&u, "var^ x = 1\nvar^ x = 2\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_REDEFINED);
    unit_dispose(&u);

    // 8.7: visible throughout the scope, but not readable before its var^
    // has run.
    LHAT_TEST("reading before the var^ has run is an error");
    check_text(&u, "var^ x = y\nvar^ y = 2\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_USED_BEFORE_DEFINED);
    unit_dispose(&u);

    // The same visibility is what lets two subroutines call each other with
    // no forward declaration, since a body does not run where it is written.
    LHAT_TEST("mutual recursion needs no forward declaration");
    check_text(&u,
               "var^ isEven = f^ n:number^ -> bool^ { return^ isOdd(n) }\n"
               "var^ isOdd = f^ n:number^ -> bool^ { return^ isEven(n) }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 03 の 7 章、P6: neither half is annotated, so each infers its result
    // from its own body alone. 'a' is checked first, and its call to the
    // not-yet-checked 'b' is unknown^ -- so 'a' infers bool^|unknown^, not
    // plain bool^. Only that leftover unknown^ arm is what makes the
    // mismatch below fire; before the fix, append_arms mistook it for a
    // duplicate of the bool^ arm already there and silently dropped it,
    // leaving 'a' looking like a clean bool^ when it was not one.
    LHAT_TEST("mutual recursion still needs its own annotation to be checked");
    check_text(&u,
               "var^ a = f^ n:number^ {\n"
               "  if^ n <= 0 { return^ true^ }\n"
               "  return^ b(n - 1)\n"
               "}\n"
               "var^ b = f^ n:number^ {\n"
               "  if^ n <= 0 { return^ 999 }\n"
               "  return^ a(n - 1)\n"
               "}\n"
               "var^ x : bool^ = a(4)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // The same pair, both sides telling the truth about what they return --
    // 'b' really is number^|bool^, and saying so is what makes 'a' check.
    LHAT_TEST("and a correct annotation on both sides checks cleanly");
    check_text(&u,
               "var^ a = f^ n:number^ -> bool^ {\n"
               "  if^ n <= 0 { return^ true^ }\n"
               "  return^ b(n - 1)\n"
               "}\n"
               "var^ b = f^ n:number^ -> number^|bool^ {\n"
               "  if^ n <= 0 { return^ 999 }\n"
               "  return^ a(n - 1)\n"
               "}\n"
               "var^ x : bool^ = a(4)\n"
               "var^ y : number^|bool^ = b(4)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a parameter is in scope in the body");
    check_text(&u, "var^ f = f^ n:number^ -> number^ { return^ n + 1 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 03 の 3.1・3.5: an unannotated parameter is pending^ itself (a
    // constraint nobody wrote), and a body that just hands it back leaves
    // the inferred result pending^ too. That is the same kind of gap P6's
    // unannotated mutual recursion leaves, and strict reports it the same
    // way, once it survives to somewhere a concrete type is wanted.
    LHAT_TEST("an unannotated parameter handed straight back is pending where it lands");
    check_text(&u,
               "var^ f = f^ n {\n"
               "  return^ n\n"
               "}\n"
               "var^ x : number^ = f(4)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // The omitted annotation is a constraint nobody wrote, not a claim that
    // nothing fits -- so calling through it with an ordinary argument stays
    // clean under strict, the same as any^ would.
    LHAT_TEST("but calling through an unannotated parameter is still fine");
    check_text(&u,
               "var^ f = f^ n -> number^ {\n"
               "  return^ 1\n"
               "}\n"
               "var^ x : number^ = f(\"anything\")\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.12: '_^' stands where a name would and binds nothing. What 13.10's
    // destructuring wanted it for is a position nobody needs.
    LHAT_TEST("'_^' takes a position without naming it");
    check_text(&u,
               "var^ f = f^ -> (number^, number^, number^) {\n"
               "  return^ 1, 2, 3 }\n"
               "var^ a, _^, c = f()\n"
               "var^ n : number^ = a + c\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // "may be written as often as it likes" -- a second one is not a
    // redefinition, since there is nothing to read back either way.
    LHAT_TEST("and may be written more than once in a run");
    check_text(&u,
               "var^ f = f^ -> (number^, number^, number^) {\n"
               "  return^ 1, 2, 3 }\n"
               "var^ _^, _^, c = f()\n"
               "var^ n : number^ = c\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("reading a '_^' is refused");
    check_text(&u, "var^ _^ = 5\nvar^ n = _^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_DISCARD_READ);
    unit_dispose(&u);

    // 15.11's own motive: an annotation is written to say R, and a name
    // written to carry it would hold nil^ and claim otherwise.
    LHAT_TEST("'_^' carries an annotation without carrying a value");
    check_text(&u,
               "var^ gen = p^ -> number^ {\n"
               "  var^ _^ : string^ = _yield^ 1\n"
               "  return^ 0 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.1: a parameter written to match a signature and not used. The
    // annotation is still what the call is judged against.
    LHAT_TEST("'_^' stands as a parameter");
    check_text(&u,
               "var^ g = f^ _^:number^, b:number^ -> number^ { return^ b }\n"
               "var^ n : number^ = g(1, 2)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 16.3: the walk binds two, and one of them may be unwanted.
    LHAT_TEST("'_^' stands as the focus of a walk");
    check_text(&u,
               "var^ total = 0\n"
               "for^ _^, v in^ { 10, 20 } { total := total + v }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 12.5: the name is not wanted but 12.6's disposal is, which is the one
    // place a '_^' is written for what happens rather than for a value.
    LHAT_TEST("'_^' stands as a with^ target");
    check_text(&u,
               "var^ C = def^{ self^{ v := 1 }, dispose := p^self^ { } }\n"
               "with^ _^ = C.new() { }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.12 refuses `_x` on purpose: the underscore prefix means discard in
    // L^, and a name that merely begins with one is an ordinary name.
    LHAT_TEST("a name beginning with '_' is an ordinary name");
    check_text(&u, "var^ _x = 5\nvar^ n : number^ = _x\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

static void test_expressions(void)
{
    Unit u;

    LHAT_TEST("arithmetic needs numbers");
    check_text(&u, "var^ s = \"a\"\nvar^ n = s + 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    LHAT_TEST("a comparison is a bool");
    check_text(&u, "var^ b : bool^ = 1 < 2\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and^ takes bools");
    check_text(&u, "var^ b = 1 and^ 2\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_BOOL);
    unit_dispose(&u);

    LHAT_TEST("an annotation has to be satisfied");
    check_text(&u, "var^ x : number^ = \"text\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("a reassignment keeps the type of the name");
    check_text(&u, "var^ x = 1\nx := \"text\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 7.4: 'x += 1' checks the same way 'x := x + 1' would -- the operator
    // it stands for has to accept the right side, and the result has to fit
    // the name.
    LHAT_TEST("compound assignment checks like the operator it stands for");
    check_text(&u, "var^ x : number^ = 1\nx += 1\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and refuses what the operator would refuse");
    check_text(&u, "var^ s = \"hi\"\ns += 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    LHAT_TEST("a call checks its arguments and arity");
    check_text(&u,
               "var^ f = f^ n:number^ -> number^ { return^ n }\n"
               "var^ a = f(1)\n"
               "var^ b = f(\"text\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("too few arguments is reported");
    check_text(&u,
               "var^ f = f^ n:number^ -> number^ { return^ n }\n"
               "var^ a = f()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    LHAT_TEST("calling something that is not a subroutine");
    check_text(&u, "var^ x = 1\nvar^ y = x(1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_CALLABLE);
    unit_dispose(&u);

    // 14.10: a structure asks for at least its members, so reading one it
    // does not have is where the report belongs.
    LHAT_TEST("a member has to exist");
    check_text(&u, "var^ t = { a := 1 }\nvar^ x = t.a\nvar^ y = t.b\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    LHAT_TEST("a table literal keeps its members' types");
    check_text(&u, "var^ t = { a := 1 }\nvar^ n : number^ = t.a\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.7: any^ admits every value, so passing one on is fine.
    LHAT_TEST("any^ accepts anything");
    check_text(&u,
               "var^ log = p^ x:any^ { }\n"
               "log(1)\n"
               "log(\"text\")\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 11 章: unary '-' has no member form, so a wrong operand is refused by
    // its own name rather than as a generic mismatch.
    LHAT_TEST("unary '-' on a string is refused by name");
    check_text(&u, "var^ x = -\"a\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_NUMBER);
    unit_dispose(&u);
}

// 03 の 3.4.
static void test_results(void)
{
    Unit u;

    LHAT_TEST("the result type is inferred from return^");
    check_text(&u,
               "var^ f = f^ { return^ 0 }\n"
               "var^ n : number^ = f()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("several return^ make a union");
    check_text(&u,
               "var^ f = f^ b:bool^ {\n"
               "    if^ b { return^ 0 }\n"
               "    return^ nil^\n"
               "}\n"
               "var^ n : number^ = f(true^)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("a declared result is checked against return^");
    check_text(&u, "var^ f = f^ -> number^ { return^ \"text\" }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 3.4: the exits that do not go through the subroutine itself settle the
    // result between them, so recursion needs nothing written.
    LHAT_TEST("recursion is inferred from the exits that are not recursive");
    check_text(&u,
               "var^ fact = f^ n:number^ {\n"
               "    if^ n <= 1 { return^ 1 }\n"
               "    return^ n * fact(n - 1)\n"
               "}\n"
               "var^ n : number^ = fact(5)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the recursive exit does not widen it");
    check_text(&u,
               "var^ fact = f^ n:number^ {\n"
               "    if^ n <= 1 { return^ 1 }\n"
               "    return^ fact(n - 1)\n"
               "}\n"
               "var^ s : string^ = fact(5)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 3.4: a recursive exit is dropped only when its type is the one being
    // worked out. Here the call sits inside something whose answer does not
    // depend on it, so the arm it contributes has to be kept.
    LHAT_TEST("a recursive call inside a larger expression still counts");
    check_text(&u,
               "var^ tag = f^ v:any^ -> string^ { return^ \"t\" }\n"
               "var^ f = f^ x:number^ {\n"
               "    if^ x > 1 { return^ tag(f(x - 1)) }\n"
               "    return^ 1\n"
               "}\n"
               "var^ v : number^|string^ = f(3)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the union really is both arms");
    check_text(&u,
               "var^ tag = f^ v:any^ -> string^ { return^ \"t\" }\n"
               "var^ f = f^ x:number^ {\n"
               "    if^ x > 1 { return^ tag(f(x - 1)) }\n"
               "    return^ 1\n"
               "}\n"
               "var^ v : number^ = f(3)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 12.8 and 03 の 5.6 leave no other way out, so a body every exit of
    // which calls itself can never produce a value.
    LHAT_TEST("a body whose every exit is recursive is reported");
    check_text(&u, "var^ f = f^ n:number^ { return^ f(n) }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NEVER_RETURNS);
    unit_dispose(&u);

    LHAT_TEST("falling out of the body is an exit, so this one is not that");
    check_text(&u,
               "var^ f = p^ n:number^ {\n"
               "    if^ n > 0 { return^ f(n - 1) }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.2 keeps "returns nothing" apart from "returns nil^". A body that
    // returns a value on one path and falls out on another does produce a
    // value, so the missing one is nil^ (04 の 11.3).
    LHAT_TEST("falling out of the body puts nil^ in the result");
    check_text(&u,
               "var^ f = p^ b:bool^ { if^ b { return^ 1 } }\n"
               "var^ n : number^ = f(true^)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and the union is what the caller has to handle");
    check_text(&u,
               "var^ f = p^ b:bool^ { if^ b { return^ 1 } }\n"
               "var^ n : number^ = f(true^) ?? 0\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.2: a function answers on every path, so an f^ that can reach its
    // end has one with nothing to answer with. No result type fixes that.
    LHAT_TEST("a function that can reach its end is reported");
    check_text(&u, "var^ f = f^ b:bool^ { if^ b { return^ true^ } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    unit_dispose(&u);

    LHAT_TEST("and writing the result does not excuse it");
    check_text(&u,
               "var^ f = f^ b:bool^ -> bool^|nil^ { if^ b { return^ true^ } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    unit_dispose(&u);

    LHAT_TEST("covering every path is what it takes");
    check_text(&u,
               "var^ f = f^ b:bool^ {\n"
               "    if^ b { return^ true^ }\n"
               "    return^ false^\n"
               "}\n"
               "var^ x : bool^ = f(true^)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("an empty function body is reported too");
    check_text(&u, "var^ f = f^ { }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    unit_dispose(&u);

    // 02 の 12.1: a with^ runs its block once and always, so a block that
    // answers on every path leaves the with^ answering on every path. 04 の
    // 5.1 writes a whole subroutine in this shape.
    LHAT_TEST("a with^ whose block always answers answers too");
    check_text(&u,
               "var^ C = def^{ self^{ v := 3 }, dispose := p^self^ { } }\n"
               "var^ f = f^ -> number^ { with^ c = C.new() { return^ c.v } }\n"
               "var^ n : number^ = f()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and one whose block may fall through still falls out");
    check_text(&u,
               "var^ C = def^{ self^{ v := 3 }, dispose := p^self^ { } }\n"
               "var^ f = f^ b:bool^ -> number^ {\n"
               "    with^ c = C.new() { if^ b { return^ c.v } }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    unit_dispose(&u);

    // 04 の 5.1's own example: the error leaves through try^ and the value
    // through the block, and 5.2 runs dispose() on both ways out.
    LHAT_TEST("a with^ over a try^ answers on both ways out");
    check_text(&u,
               "errordef^ IOError { NotFound }\n"
               "var^ H = def^{ self^{ v := 7 }, dispose := p^self^ { },\n"
               "  read := f^self^ -> number^ { return^ self^.v } }\n"
               "var^ open = f^ -> H|IOError.NotFound { return^ H.new() }\n"
               "var^ read = f^ -> number^|IOError.NotFound {\n"
               "    with^ h = try^ open() { return^ h.read() }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The same exit under a p^ that wrote a result which does not admit it.
    LHAT_TEST("a written result has to admit the value-less exit");
    check_text(&u,
               "var^ f = p^ b:bool^ -> number^ { if^ b { return^ 1 } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FALLS_OUT_OF_RESULT);
    unit_dispose(&u);

    LHAT_TEST("and admitting it is enough");
    check_text(&u,
               "var^ f = p^ b:bool^ -> number^|nil^ { if^ b { return^ 1 } }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 03 の 3.4 counts every exit, and a bare return^ is one that produces
    // no value -- the same exit reaching the end of the body is, and it
    // leaves the same nil^ behind at run time.
    LHAT_TEST("a bare return^ is an exit that produces no value");
    check_text(&u,
               "var^ f = p^ b:bool^ { if^ b { return^ 1 } return^ }\n"
               "var^ v : number^ = f(true^)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("so nil^ joins the result beside the value the other exit makes");
    check_text(&u,
               "var^ f = p^ b:bool^ { if^ b { return^ 1 } return^ }\n"
               "var^ v : number^|nil^ = f(true^)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("an f^ may not take that exit either");
    check_text(&u, "var^ f = f^ -> number^ { return^ }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    unit_dispose(&u);

    LHAT_TEST("and a written result has to admit it");
    check_text(&u, "var^ f = p^ -> number^ { return^ }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FALLS_OUT_OF_RESULT);
    unit_dispose(&u);

    // 16.5: a repeat^ with no bound and no break^ of its own never ends, so
    // the end of the body is not somewhere control can arrive.
    LHAT_TEST("an endless repeat^ is not a way to the end of a body");
    check_text(&u, "var^ f = f^ -> number^ { repeat^ { } }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("nor when every turn of it returns");
    check_text(&u, "var^ f = f^ -> number^ { repeat^ { return^ 1 } }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a p^ with a written result is not made to admit nil^");
    check_text(&u, "var^ g = p^ -> number^ { repeat^ { yield^ 1 } }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 9.8: break^ leaves the loop, so the end is reachable again.
    LHAT_TEST("a break^ puts the end of the body back within reach");
    check_text(&u, "var^ f = f^ -> number^ { repeat^ { break^ } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    unit_dispose(&u);

    LHAT_TEST("wherever the break^ is written inside the loop");
    check_text(&u,
               "var^ f = f^ -> number^ { repeat^ { if^ true^ { break^ } } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    unit_dispose(&u);

    // A break^ of an inner loop leaves that one, not this one.
    LHAT_TEST("but a nested loop keeps its own break^");
    check_text(&u,
               "var^ f = f^ -> number^ { repeat^ { repeat^ { break^ } } }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 16.3: the if^ form of for^ does not iterate, so a break^ in it is the
    // outer loop's.
    LHAT_TEST("and the if^ form of for^ is not a loop to break out of");
    check_text(&u,
               "var^ f = f^ -> number^ {\n"
               "    repeat^ { for^ var^ x := 1 if^ true^ { break^ } }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    unit_dispose(&u);

    // 17 章: nor is the when^ form.
    LHAT_TEST("nor is the when^ form");
    check_text(&u,
               "var^ f = f^ -> number^ {\n"
               "    repeat^ { for^ 1 { when^ 1: break^ other^: } }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    unit_dispose(&u);

    // Only the endless form. The others can run out on their own.
    LHAT_TEST("a bounded repeat^ still reaches the end of the body");
    check_text(&u, "var^ f = f^ -> number^ { repeat^ 3 { } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    unit_dispose(&u);

    LHAT_TEST("and so does a conditional one");
    check_text(&u, "var^ f = f^ -> number^ { repeat^ while^ true^ { } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    unit_dispose(&u);

    // 13.11: a branch that ends in an endless loop is a branch that leaves.
    LHAT_TEST("a branch ending in one covers its path");
    check_text(&u,
               "var^ f = f^ -> number^ {\n"
               "    if^ true^ { repeat^ { } el^: return^ 1 }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A body with no return^ at all asked for no value. 13.2 has a form for
    // it, and nil^ is not it.
    LHAT_TEST("a body that returns nothing stays returning nothing");
    check_text(&u,
               "var^ log = p^ n:number^ { var^ x = n }\n"
               "log(1)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("every path returning means no nil^ joins in");
    check_text(&u,
               "var^ f = p^ b:bool^ { if^ b { return^ 1 else^: return^ 2 } }\n"
               "var^ n : number^ = f(true^)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.10: this^ is a self-call like a call by name, so 3.4 counts it the
    // same way and a body with no name can recurse.
    LHAT_TEST("this^ recursion is inferred the same way");
    check_text(&u,
               "var^ fact = f^ n:number^ {\n"
               "    if^ n <= 1 { return^ 1 }\n"
               "    return^ n * this^(n - 1)\n"
               "}\n"
               "var^ n : number^ = fact(5)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a body whose every exit is this^ is reported");
    check_text(&u, "var^ f = f^ n:number^ { return^ this^(n) }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NEVER_RETURNS);
    unit_dispose(&u);

    // 15.10: this^ has the signature of the body it is in, so the arguments
    // are checked -- which a call by name cannot do while the name is still
    // being bound.
    LHAT_TEST("this^ checks its arguments");
    check_text(&u,
               "var^ f = f^ n:number^ {\n"
               "    if^ n <= 1 { return^ 1 }\n"
               "    return^ this^(\"text\")\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("this^ outside any body is reported");
    check_text(&u, "var^ x = this^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_THIS_OUTSIDE);
    unit_dispose(&u);

    LHAT_TEST("with the result written, recursion is fine");
    check_text(&u, "var^ f = f^ n:number^ -> number^ { return^ f(n) }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

// 03 の 3.4: a parameter written without a type is what the body demands of
// it. Not unification -- the demands are collected and intersected, which is
// why 11.3's structural reading is what makes this work rather than what makes
// it hard.
static void test_parameter_inference(void)
{
    Unit u;

    // 11.8 with 14.8: number^ is the one type carrying the arithmetic, so an
    // arithmetic use says which type arrived.
    LHAT_TEST("arithmetic on a parameter decides it");
    check_text(&u,
               "var^ f = f^ x -> number^ { return^ x + 1 }\n"
               "var^ y : number^ = f(2)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the call site is checked against what was demanded");
    check_text(&u,
               "var^ f = f^ x -> number^ { return^ x + 1 }\n"
               "var^ y : number^ = f(\"a\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 14.4 puts the left operand in self^ and leaves the right one the
    // argument, so both sides of 'x + y' demand what number^'s operator takes.
    LHAT_TEST("both operands of an arithmetic operator are demands");
    check_text(&u,
               "var^ f = f^ x, y -> number^ { return^ x + y }\n"
               "var^ n : number^ = f(1, \"a\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 14.10's "at least these" is the demand a member read makes, written as
    // a type. Nothing has to be named for this to work.
    LHAT_TEST("a member read demands a structure carrying it");
    check_text(&u,
               "var^ f = p^ x { x.write() }\n"
               "f(5)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and a table carrying that member is accepted");
    check_text(&u,
               "var^ f = p^ x { x.write() }\n"
               "var^ t = { write := p^ { } }\n"
               "f(t)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.1: a call says what the arguments are and nothing about the rest of
    // the signature, so it is not a demand -- see 03 の 3.4.
    LHAT_TEST("calling a parameter demands nothing");
    check_text(&u,
               "var^ f = p^ x { x() }\n"
               "f(5)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.11: the demand belongs to the narrowed value, not to what arrived.
    // Counting it would undo the narrowing the writer put there.
    LHAT_TEST("a use under a narrowing is not a demand");
    check_text(&u,
               "var^ f = p^ x {\n"
               "    if^ x isa^ number^ { var^ n : number^ = x + 1 }\n"
               "}\n"
               "f(\"a\")\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 3.4: what always runs decides; a branch is added only when it agrees.
    LHAT_TEST("an unconditional demand wins over a branch that disagrees");
    check_text(&u,
               "var^ f = p^ b:bool^, x {\n"
               "    var^ n : number^ = x + 1\n"
               "    if^ b { x.write() }\n"
               "}\n"
               "f(true^, \"a\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // Two branches ruling each other out leave nothing decided. Reporting
    // here would be reporting the same thing twice: each use says so itself.
    LHAT_TEST("branches that disagree decide nothing");
    check_text(&u,
               "var^ f = p^ b:bool^, x {\n"
               "    if^ b { var^ n : number^ = x + 1 else^: x.write() }\n"
               "}\n"
               "f(true^, \"a\")\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // Two branches that agree decide it between them.
    LHAT_TEST("branches that agree decide it");
    check_text(&u,
               "var^ f = p^ b:bool^, x {\n"
               "    if^ b { var^ n : number^ = x + 1 else^: var^ m : number^ = x * 2 }\n"
               "}\n"
               "f(true^, \"a\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 3.4: a nested body demands of the enclosing parameter too. Whether it
    // ever runs is not the question -- what it needs when it does, the value
    // handed in has to carry.
    LHAT_TEST("a demand written in a nested body still reaches the parameter");
    check_text(&u,
               "var^ f = p^ x {\n"
               "    var^ inner = p^ { var^ n : number^ = x + 1 }\n"
               "    inner()\n"
               "}\n"
               "f(\"a\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 3.4: what the name holds after a reassignment is not what was passed in.
    LHAT_TEST("a reassignment ends the collection");
    check_text(&u,
               "var^ f = p^ x {\n"
               "    x := \"z\"\n"
               "    var^ n : number^ = x + 1\n"
               "}\n"
               "f(\"a\")\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A demand made where the argument type is known travels the same way.
    LHAT_TEST("passing a parameter on demands what the callee takes");
    check_text(&u,
               "var^ g = p^ n:number^ { }\n"
               "var^ f = p^ x { g(x) }\n"
               "f(\"a\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // Two member reads are one demand for a table carrying both (14.10),
    // rather than two demands standing side by side.
    LHAT_TEST("two member reads merge into one structure");
    check_text(&u,
               "var^ f = p^ x { x.a() x.b() }\n"
               "var^ t = { a := p^ { } }\n"
               "f(t)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // L^ has no generic types (02 の 13.7, 04 の 7 章), so a parameter handed
    // straight back demands nothing and stays what nobody wrote -- which
    // strict reports where it lands, exactly as it did before 3.4 read
    // anything off a body at all.
    LHAT_TEST("a parameter nothing demands stays undecided");
    check_text(&u,
               "var^ id = f^ x { return^ x }\n"
               "var^ y : number^ = id(4)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 05 の 4.3: what leaves the unit is not read off a body.
    LHAT_TEST("a public^ subroutine has to write its parameter types");
    check_text(&u, "public^ let^ f = p^ x { x.write() }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PUBLIC_NEEDS_TYPE);
    unit_dispose(&u);

    LHAT_TEST("written out, the same one is fine");
    check_text(&u,
               "public^ let^ f = p^ x:t^{ write : p^; } { x.write() }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 4.3: telling what leaves the unit from what stays would take more than
    // the syntax, so everything written inside the declaration is included.
    LHAT_TEST("a subroutine nested in a public^ one is included");
    check_text(&u,
               "public^ let^ make = p^ -> number^ {\n"
               "    var^ step = p^ y { }\n"
               "    return^ 1\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PUBLIC_NEEDS_TYPE);
    unit_dispose(&u);

    LHAT_TEST("and one that is not published is not");
    check_text(&u, "var^ helper = p^ x { x.write() }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.4: a default is what an editor writes into a call site, so it is held
    // to what that position takes -- a call carrying it has to be a call that
    // would have checked.
    LHAT_TEST("a default has to fit the parameter it stands in for");
    check_text(&u, "var^ f = p^ x:number^ = \"a\" { }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and one that fits is accepted");
    check_text(&u, "var^ f = p^ x:number^ = 1 { }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 03 の 3.4 reads the body's demands and nothing else. A default is a value
    // the call carries, not a use the body makes, so it decides nothing --
    // this is the case above it with the annotation taken away.
    LHAT_TEST("a default is not a demand");
    check_text(&u,
               "var^ id = f^ x = 1 { return^ x }\n"
               "var^ y : number^ = id(4)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 13.4: it does not make the parameter optional either.
    LHAT_TEST("a default does not let the call leave the argument out");
    check_text(&u,
               "var^ f = p^ x:number^ = 1 { }\n"
               "f()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    // The expression will stand at the call, where the parameters of the body
    // it belongs to are not in scope.
    LHAT_TEST("a default may not name another parameter");
    check_text(&u, "var^ f = p^ a:number^, b:number^ = a { }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
    unit_dispose(&u);
}

// 03 の 3.1: a result the body did not decide. The gap is reported at the
// definition -- a result type is a promise to every caller -- and again
// wherever it reaches a place a concrete type is wanted.
static void test_undecided_results(void)
{
    Unit u;

    // 3.1's own example. Whichever of the two is walked first reads a partner
    // that has not been walked, so the union it answers carries a gap.
    LHAT_TEST("a mutually recursive pair says its result did not come out");
    check_text(&u,
               "let^ a = f^ n:number^ {\n"
               "    if^ n <= 0 { return^ true^ }\n"
               "    return^ b(n - 1)\n"
               "}\n"
               "let^ b = f^ n:number^ {\n"
               "    if^ n <= 0 { return^ false^ }\n"
               "    return^ a(n - 1)\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_RESULT_UNDECIDED);
    LHAT_CHECK_EQ_INT(u.checked.diagnostic_count, 2);
    unit_dispose(&u);

    // Writing one of them closes the ring: the one written no longer asks,
    // and the other reads it rather than a gap.
    LHAT_TEST("and writing one of the two settles both");
    check_text(&u,
               "let^ a = f^ n:number^ -> bool^ {\n"
               "    if^ n <= 0 { return^ true^ }\n"
               "    return^ b(n - 1)\n"
               "}\n"
               "let^ b = f^ n:number^ {\n"
               "    if^ n <= 0 { return^ false^ }\n"
               "    return^ a(n - 1)\n"
               "}\n"
               "let^ x : bool^ = a(4)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 3.5: relaxed leaves an undecided type to the machine, so it says
    // nothing here -- the same program, the same types, a different setting.
    LHAT_TEST("relaxed forgives the whole of it");
    check_relaxed_text(&u,
                       "let^ a = f^ n:number^ {\n"
                       "    if^ n <= 0 { return^ true^ }\n"
                       "    return^ b(n - 1)\n"
                       "}\n"
                       "let^ b = f^ n:number^ { return^ a(n - 1) }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The gap is in a union arm, where lhat_type_conforms's leniency used to
    // hide it: 'bool^|pending^' is not a bool^ where one is wanted.
    LHAT_TEST("a gap buried in a union is reported where it lands as well");
    check_text(&u,
               "let^ a = f^ n:number^ {\n"
               "    if^ n <= 0 { return^ true^ }\n"
               "    return^ b(n - 1)\n"
               "}\n"
               "let^ b = f^ n:number^ -> bool^ { return^ a(n - 1) }\n"
               "let^ x : bool^ = a(4)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 02 の 13.7 and 04 の 7 章: no generic types, so a parameter handed
    // straight back leaves the result undecided too. 3.4's own reading of
    // this case -- the writer ends up writing any^ or a concrete type -- is
    // what the report asks for.
    LHAT_TEST("a parameter handed straight back leaves the result undecided");
    check_text(&u, "var^ id = f^ x { return^ x }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_RESULT_UNDECIDED);
    unit_dispose(&u);

    LHAT_TEST("and writing any^ is what settles it");
    check_text(&u, "var^ id = f^ x:any^ { return^ x }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The parameters are not read this way. A gap there is a constraint
    // nobody wrote, which is what any^ already means (3.1).
    LHAT_TEST("a parameter nothing demands is not itself reported");
    check_text(&u, "var^ f = p^ x { }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 03 の 3.4改: a signature written on the binding is written, wherever it
    // stands. The body decides nothing here, so there is nothing to report.
    LHAT_TEST("a result the binding wrote is not inferred");
    check_text(&u,
               "let^ a : f^number^ -> bool^; = f^ n {\n"
               "    if^ n <= 0 { return^ true^ }\n"
               "    return^ b(n - 1)\n"
               "}\n"
               "let^ b = f^ n:number^ -> bool^ { return^ a(n - 1) }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 03 の 3.4改2 walks a def^'s members again from what the last walk
    // inferred, so a ring of them settles without anything being written --
    // and the first walk's report goes with the first walk.
    LHAT_TEST("a ring of def^ members is settled by the walks, not reported");
    check_text(&u,
               "var^ R = def^{\n"
               "    self^{ n := 0 },\n"
               "    expr := p^self^ { return^ self^.term() },\n"
               "    term := p^self^ {\n"
               "        if^ self^.n > 0 { return^ self^.expr() }\n"
               "        return^ 1\n"
               "    },\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

// 04.
static void test_errors(void)
{
    Unit u;

    LHAT_TEST("a kind and its set become types");
    check_text(&u,
               "errordef^ IOError { NotFound, Denied }\n"
               "var^ e : IOError = error^IOError.NotFound{ }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 04 の 8.1: the whole detection mechanism for an unhandled error is
    // ordinary conformance.
    LHAT_TEST("an unhandled error cannot be used as the value");
    check_text(&u,
               "errordef^ IOError { NotFound }\n"
               "var^ f = f^ -> number^|IOError { return^ 0 }\n"
               "var^ n : number^ = f()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("catch^ drops the error arm");
    check_text(&u,
               "errordef^ IOError { NotFound }\n"
               "var^ f = f^ -> number^|IOError { return^ 0 }\n"
               "var^ n : number^ = f() catch^ 0\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 04 の 4.1: a catch^ that can never fire is an error, since it says
    // something about a failure that cannot happen.
    LHAT_TEST("catch^ on what cannot fail is reported");
    check_text(&u, "var^ n = 1 catch^ 0\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_CANNOT_FAIL);
    unit_dispose(&u);

    // 04 の 4.2: catch^ names the error it^ inside its right side. Both
    // halves have to know it -- the machine binds it (compile_catch) and the
    // checker resolves it.
    LHAT_TEST("catch^ binds it^ in its right side");
    check_text(&u,
               "errordef^ IOError { NotFound }\n"
               "var^ f = f^ -> number^|IOError { return^ 0 }\n"
               "var^ n : number^ = f() catch^ if^ it^ isa^ IOError.NotFound:"
               " 0 el^: 1 ;\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 2.3 gives every kind message and cause, so the error half of the left
    // side is enough for it^.message to answer through.
    LHAT_TEST("and it^ is typed as the error half of the left");
    check_text(&u,
               "errordef^ IOError { NotFound }\n"
               "var^ f = f^ -> number^|IOError { return^ 0 }\n"
               "var^ s : string^|number^ = f() catch^ it^.message\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 03 の 3.4改: a signature written on the binding is what the value is
    // expected to have, so a parameter nothing was written on takes its type
    // from there rather than waiting for the body to demand one.
    LHAT_TEST("a written signature fills in the parameters");
    check_text(&u,
               "var^ f : p^string^ -> number^; = p^ x { return^ x.length }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the body is checked against what it filled in");
    check_text(&u,
               "var^ f : p^string^ -> number^; = p^ x { return^ x + 1 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    // 05 の 4.3 asks that what leaves the unit not be read off a body. A
    // signature on the binding says it as plainly as one in the parameter
    // list, so the demand is met and the report belongs to neither.
    LHAT_TEST("and satisfies what a public^ declaration asks for");
    check_text(&u,
               "public^ let^ f : p^string^ -> string^; = p^ x { return^ x }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("but a public^ with nothing written anywhere is still refused");
    check_text(&u, "public^ let^ f = p^ x { return^ x }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PUBLIC_NEEDS_TYPE);
    unit_dispose(&u);

    // 03 の 3.4改: a literal called where it is written is expected by that
    // call. Reading it leaves nothing -- the call is part of the same
    // expression, which is what keeps 3.4's line about callers elsewhere.
    LHAT_TEST("a literal called where it is written takes the argument's type");
    check_text(&u,
               "var^ n : number^ = p^ x { return^ x.length }(\"abc\")\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // An argument is inferred once. Reading it a second time to seed the
    // parameter would report whatever is wrong with it twice.
    LHAT_TEST("an argument is reported once");
    check_text(&u, "var^ a = p^ x { return^ x }(nowhere)\n");
    LHAT_CHECK_EQ_INT(u.checked.diagnostic_count, 1);
    unit_dispose(&u);

    // The expectation is the one literal's. A body written inside it is
    // expected by nothing, so its own parameters wait on their own demands.
    LHAT_TEST("an expectation does not reach a body written inside");
    check_text(&u,
               "var^ f : p^string^ -> number^; = p^ x {\n"
               "    var^ g = p^ y { return^ y + 1 }\n"
               "    return^ g(1) + x.length\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 2.3 again, where the call can fail two ways: it^ is then a union of
    // kinds from two declarations, and what a union answers is what every
    // arm answers -- which for message and cause is every kind there is.
    LHAT_TEST("and answers through a union of kinds from two declarations");
    check_text(&u,
               "errordef^ IOError { NotFound }\n"
               "errordef^ Memory { Out }\n"
               "var^ f = f^ -> number^|IOError.NotFound|Memory.Out {\n"
               "    return^ 0\n"
               "}\n"
               "var^ s : string^|number^ = f() catch^ it^.message\n"
               "var^ c = f() catch^ it^.cause\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 2.2's fields are the leaf's own, so they still want the narrowing --
    // what every arm answers is message and cause and no more.
    LHAT_TEST("but a leaf's field still wants narrowing to that leaf");
    check_text(&u,
               "errordef^ IOError { NotFound { path : string^ } }\n"
               "errordef^ Memory { Out }\n"
               "var^ f = f^ -> number^|IOError.NotFound|Memory.Out {\n"
               "    return^ 0\n"
               "}\n"
               "var^ s = f() catch^ it^.path\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // A union with an arm that is not an error is not this reading: what
    // every arm answers is then whatever those two have in common, which is
    // nothing a member access reaches.
    LHAT_TEST("and a union with a value arm answers neither");
    check_text(&u,
               "errordef^ IOError { NotFound }\n"
               "var^ x : string^|IOError.NotFound = \"s\"\n"
               "var^ s = x.message\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // 11.7: ?? asks about nil^, and there is nothing in a nil^ to name --
    // it^ is catch^'s alone.
    LHAT_TEST("?? binds no it^");
    check_text(&u,
               "var^ f = f^ -> number^|nil^ { return^ 0 }\n"
               "var^ n = f() ?? it^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
    unit_dispose(&u);

    LHAT_TEST("?? drops the nil arm");
    check_text(&u,
               "var^ f = f^ -> number^|nil^ { return^ 0 }\n"
               "var^ n : number^ = f() ?? 0\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("?? on what cannot be nil is reported");
    check_text(&u, "var^ n = 1 ?? 0\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_CANNOT_BE_NIL);
    unit_dispose(&u);

    LHAT_TEST("try^ unwraps and the result is the success type");
    check_text(&u,
               "errordef^ IOError { NotFound }\n"
               "var^ open = f^ -> number^|IOError { return^ 0 }\n"
               "var^ read = f^ -> number^|IOError { return^ try^ open() }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 04 の 5.3: reported where try^ is written, not at the caller.
    LHAT_TEST("try^ may not let out an error the result excludes");
    check_text(&u,
               "errordef^ IOError { NotFound, Denied }\n"
               "var^ open = f^ -> number^|IOError { return^ 0 }\n"
               "var^ read = f^ -> number^|IOError.NotFound {\n"
               "    return^ try^ open()\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TRY_OUTSIDE);
    unit_dispose(&u);

    // 04 の 2.4: identity is the declaration, so two identical ones differ.
    LHAT_TEST("kinds from different declarations do not mix");
    check_text(&u,
               "errordef^ IOError { NotFound }\n"
               "errordef^ UserError { NotFound }\n"
               "var^ e : IOError = error^UserError.NotFound{ }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 04 の 2.2: a field may carry a default, written with ':=' as 14.6's
    // template already writes a named field with an initial value.
    LHAT_TEST("a default stands in for the type");
    check_text(&u,
               "errordef^ ParseError { Syntax { line := 0 } }\n"
               "var^ e = error^ParseError.Syntax{ }\n"
               "var^ n : number^ = e.line\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a field may carry both a type and a default");
    check_text(&u,
               "errordef^ ParseError { Syntax { line : number^ := 0 } }\n"
               "var^ e = error^ParseError.Syntax{ }\n"
               "var^ n : number^ = e.line\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a default has to fit the declared type");
    check_text(&u,
               "errordef^ ParseError { Syntax { line : number^ := \"text\" } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 04 の 2.5: without a default there is nothing to fall back to.
    LHAT_TEST("a field with no default has to be written");
    check_text(&u,
               "errordef^ ParseError { Syntax { line : number^ } }\n"
               "var^ e = error^ParseError.Syntax{ }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISSING_FIELD);
    unit_dispose(&u);

    LHAT_TEST("a default may still be overridden at the construction");
    check_text(&u,
               "errordef^ ParseError { Syntax { line := 0 } }\n"
               "var^ e = error^ParseError.Syntax{ line := 3 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 04 の 6.1: a declared field is reached through the kind.
    LHAT_TEST("a kind's declared field is visible");
    check_text(&u,
               "errordef^ ParseError { Syntax { line : number^ } }\n"
               "var^ e = error^ParseError.Syntax{ line := 3 }\n"
               "var^ n : number^ = e.line\n"
               "var^ m : string^ = e.message\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

static void test_annotations(void)
{
    Unit u;

    LHAT_TEST("an unknown type name is reported");
    check_text(&u, "var^ x : Nonesuch = 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNKNOWN_TYPE);
    unit_dispose(&u);

    // 13 章 has no such type as self^ or class^, and outside a def^ that is
    // already the answer, there being no binding of either to find. Inside
    // one there is, and 05 の 2.2's one environment would otherwise hand it
    // over -- answering with a type that holds itself, which the relations walk
    // until the stack is gone. The spelling means the same thing in both places.
    LHAT_TEST("self^ is not a type, inside a def^ as well as out");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{ n := 0 },\n"
               "  m := f^self^, r:self^ -> number^ { return^ r.n },\n"
               "}\n"
               "var^ x = V.new().m(V.new())\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNKNOWN_TYPE);
    unit_dispose(&u);

    LHAT_TEST("nor is class^");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^+ := f^self^, r:class^ -> number^ { return^ 0 },\n"
               "}\n"
               "var^ x = V.new() + V.new()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNKNOWN_TYPE);
    unit_dispose(&u);

    // 05 の 2.2: a name stands for a value, a type, or both, and 'let^ x = 1'
    // is the first of those. Being in scope is not enough to be written as a
    // type -- that would make every binding one meaning "what this value is".
    LHAT_TEST("a name bound to a plain value is not a type");
    check_text(&u, "var^ x = 1\nvar^ y : x = 2\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNKNOWN_TYPE);
    unit_dispose(&u);

    LHAT_TEST("nor is one bound to a subroutine");
    check_text(&u,
               "var^ g = f^ -> number^ { return^ 1 }\n"
               "var^ y : g = g\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNKNOWN_TYPE);
    unit_dispose(&u);

    // 11.3 keeps identity structural, so what a table names is a shape that
    // can be asked for -- a def^, a host type (05 の 8.8), what require^
    // answers (05 の 6.1), and a table written by hand alike. The line is
    // drawn at values with no structure to ask about, not at def^.
    LHAT_TEST("a table's name still says the shape it has");
    check_text(&u,
               "var^ t = { a := 1 }\n"
               "var^ y : t = { a := 2 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // What the two of them were reached for is written with the binding's own
    // name, which 8.7 makes visible throughout the scope it is defined in, or
    // structurally where the definition has no name to use.
    LHAT_TEST("a definition's own name says the same thing and works");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^+ := f^self^, r:V -> number^ { return^ self^.n + r.n },\n"
               "}\n"
               "var^ x : number^ = V.new() + V.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.9: and it asks what it says. The collecting pass leaves a
    // pending^ seed for every let^, which conforms to everything -- so reading
    // that seed would make the annotation ask nothing at all, where the same
    // one outside the def^ asks for the whole structure.
    LHAT_TEST("a definition's own name asks for an instance of it");
    check_text(&u,
               "let^ T = def^{\n"
               "  self^{ n := 0 },\n"
               "  a = f^self^, x:T -> number^ { return^ x.n },\n"
               "}\n"
               "var^ y : number^ = T.new().a(5)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and an instance is what fits there");
    check_text(&u,
               "let^ T = def^{\n"
               "  self^{ n := 0 },\n"
               "  a = f^self^, x:T -> number^ { return^ x.n },\n"
               "}\n"
               "var^ y : number^ = T.new().a(T.new())\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.5: the definition is the right side of the '..', which is where
    // infer_binary reads it too.
    LHAT_TEST("a composed one is written against its name as well");
    check_text(&u,
               "var^ A = def^{ self^{ n := 0 } }\n"
               "var^ B = A..def^{ m = f^self^, y:B -> number^ { return^ 0 } }\n"
               "var^ z : number^ = B.new().m(5)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // Only the definition the name is being bound to. One made deeper inside
    // the value -- in a body, say -- is not what this name will hold.
    LHAT_TEST("but a def^ deeper in the value does not take the name");
    check_text(&u,
               "var^ x = f^ -> t^{} {\n"
               "  return^ def^{ self^{ n := 0 }, "
               "m = f^self^, y:x -> number^ { return^ 0 } }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.13: and a literal with no binding to take a name from says it
    // with Self^. Capital S -- self^ is the receiver, a value, and 13.1 has no
    // type by that name; this is the type, and only ever written as one.
    LHAT_TEST("Self^ names the structure written around it");
    check_text(&u,
               "var^ t : t^{ value : number^, next : Self^|nil^ } = "
               "{ value = 1, next = nil^ }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the shape it stands for is what the nested value must have");
    check_text(&u,
               "var^ t : t^{ value : number^, next : Self^|nil^ } = "
               "{ value = 1, next = { value = \"no\", next = nil^ } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 11.3: identity is structural, so two of these written apart are one
    // type. The walk that decides it meets the same pair twice and answers
    // yes, which is what makes a recursive structure comparable at all.
    LHAT_TEST("two written apart are the same type");
    check_text(&u,
               "var^ a : t^{ value : number^, next : Self^|nil^ } = "
               "{ value = 1, next = nil^ }\n"
               "var^ b : t^{ value : number^, next : Self^|nil^ } = a\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.7: writing a definition's name asks for an instance, and Self^
    // inside one says the same thing without needing the name.
    LHAT_TEST("a def^ says its own type with it");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{ n := 0 },\n"
               "  plus := f^self^, r:Self^ -> Self^ { return^ self^ },\n"
               "}\n"
               "var^ x : number^ = V.new().plus(V.new()).n\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A signature and a coroutine type are written around a type rather than
    // being a structure of their own, so neither binds one -- which is what
    // lets a method say Self^ at all.
    // Reported as a value that does not fit rather than as a Self^ with
    // nothing around it: the signature let the structure through, which is
    // what the def^ above relies on for its methods.
    LHAT_TEST("a signature inside one does not take Self^ from it");
    check_text(&u, "var^ t : t^{ m : f^Self^ -> Self^; } = { m = 1 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("but with nothing written around it there is nothing to name");
    check_text(&u, "var^ x : Self^ = 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_SELF_TYPE_OUTSIDE);
    unit_dispose(&u);

    LHAT_TEST("and a c^ on its own binds none either");
    check_text(&u, "var^ c : c^{ p^nil^ -> Self^;, nil^ } = 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_SELF_TYPE_OUTSIDE);
    unit_dispose(&u);

    // 01 の 2.3: the fifth word to count levels with a second hat.
    LHAT_TEST("Self^^ reaches the literal one further out");
    check_text(&u,
               "var^ t : t^{ a : t^{ b : Self^^|nil^ } } = "
               "{ a = { b = nil^ } }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and counting past the outermost is refused");
    check_text(&u,
               "var^ t : t^{ a : t^{ b : Self^^^ } } = { a = { b = 1 } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_SELF_TYPE_OUTSIDE);
    unit_dispose(&u);

    // 14.10: at least the listed members.
    LHAT_TEST("a structural annotation asks for at least its members");
    check_text(&u,
               "var^ t : t^{ a : number^ } = { a := 1, b := 2 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a missing member fails the annotation");
    check_text(&u, "var^ t : t^{ a : number^ } = { b := 2 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 14.10: bare, with nothing listed, it asks for nothing in particular --
    // the top of tables, which 13.7 notes is not the top of every value.
    LHAT_TEST("a bare table type takes any table");
    check_text(&u,
               "var^ x : table^ = { a := 1 }\n"
               "var^ y : t^ = { 1, 2 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and nothing that is not one");
    check_text(&u, "var^ x : table^ = 5\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("a bare one joins a union like any other type");
    check_text(&u,
               "var^ x : table^|nil^ = { a := 1 }\n"
               "var^ y : nil^|t^ = nil^\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a union annotation accepts either arm");
    check_text(&u,
               "var^ x : number^|string^ = 1\n"
               "var^ y : number^|string^ = \"text\"\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a union annotation refuses a third type");
    check_text(&u, "var^ x : number^|string^ = true^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 11.6改: is^ is a comparison like '=', so the same disjointness check
    // applies and it answers bool^ either way.
    LHAT_TEST("is^ answers bool^");
    check_text(&u, "var^ x : bool^ = 1 is^ 2\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("is^ on types that can never meet is reported");
    check_text(&u, "return^ 1 is^ \"text\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_INCOMPARABLE);
    unit_dispose(&u);

    // 13.11: isa^ reads a type, so an unknown one is reported there too.
    LHAT_TEST("isa^ resolves its right side as a type");
    check_text(&u, "var^ x = 1\nvar^ b : bool^ = x isa^ number^\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.7: any^ holds of every value, so the question is empty whatever is
    // on the left. 13.11 decides this from the right side alone -- it never
    // reads the left's inferred type against the right.
    LHAT_TEST("asking isa^ any^ asks nothing and is reported");
    check_text(&u, "var^ x : number^ = 1\nvar^ b = x isa^ any^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ISA_ALWAYS_TRUE);
    unit_dispose(&u);

    LHAT_TEST("whatever the left happens to be");
    check_text(&u, "var^ x : any^ = 1\nvar^ b = x isa^ any^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ISA_ALWAYS_TRUE);
    unit_dispose(&u);

    // 13.5 collapses a union with any^ to any^, so the written form does not
    // let it through.
    LHAT_TEST("and however the any^ is spelled");
    check_text(&u, "var^ x : number^ = 1\nvar^ b = x isa^ any^|nil^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ISA_ALWAYS_TRUE);
    unit_dispose(&u);

    // 13.11: an answer the left's inferred type fixes is not refused. isa^ is
    // there to be asked at run time, and the checker narrowing that from what
    // it thinks it knows is what 13.7 introduced any^ to avoid.
    LHAT_TEST("but an answer fixed by the left is left alone");
    check_text(&u,
               "var^ x : number^ = 1\n"
               "var^ a = x isa^ string^\n"
               "var^ b = x isa^ number^\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("which is what makes any^ usable at all");
    check_text(&u, "var^ f = p^ x:any^ { var^ b = x isa^ string^ }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

int main(void)
{
    test_names();
    test_expressions();
    test_results();
    test_parameter_inference();
    test_undecided_results();
    test_errors();
    test_annotations();
    return lhat_test_report("test_check_core");
}
