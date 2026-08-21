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

    // 8.7 exempts what a body reads of a name standing outside it, since the
    // body runs later. A name the body itself bound is not standing outside
    // it: once the body is called its own statements run in order, so the
    // ordering rule holds there exactly as it does at the top level. The
    // shape this catches is a shadow written by mistake -- the writer meant
    // to read the parameter and named the new binding the same thing.
    LHAT_TEST("and inside a body the ordering rule still holds");
    check_text(&u,
               "var^ f = f^ -> number^ {\n"
               "  var^ a = b + 1\n"
               "  var^ b = 2\n"
               "  return^ a\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_USED_BEFORE_DEFINED);
    unit_dispose(&u);

    // 8.7改: a shadow written one scope in reads the name it shadows. A
    // body's parameters and its own top-level names share one scope, so this
    // is where a shadow of a parameter has to be written -- and what it is
    // written for is exactly this: the new name starts from the old one.
    LHAT_TEST("and a shadow one scope in reads what it shadows");
    check_text(&u,
               "var^ f = f^ t:t^{ ...:number^ } -> number^ {\n"
               "  var^ total = 0\n"
               "  for^ i from^1 to^2 {\n"
               "    var^ t = t[i] ?? 0\n"
               "    total := total + t\n"
               "  }\n"
               "  return^ total\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // With nothing outside answering, the read is still the one 8.7 refuses.
    LHAT_TEST("but nothing outside to shadow is still a read too early");
    check_text(&u,
               "var^ f = f^ -> number^ {\n"
               "  do^{\n"
               "    var^ q = q + 1\n"
               "    return^ q\n"
               "  }\n"
               "  return^ 0\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_USED_BEFORE_DEFINED);
    unit_dispose(&u);

    // 8.7改: every name the statement binds reads the old world -- all of
    // them, not only the one whose position is being inferred.
    LHAT_TEST("several names shadowing read what they shadow");
    check_text(&u,
               "var^ pair = f^ a:number^, b:number^ -> (number^, number^)\n"
               "    { return^ a, b }\n"
               "var^ a = 1\n"
               "var^ b = 2\n"
               "do^{\n"
               "    let^ a, b = pair(a, b)\n"
               "    var^ n : number^ = a + b\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 8.7改: a deferred body written in the initialiser reads the old world
    // too -- the binding being made is unreadable anywhere in its own value,
    // so the capture is of what the name meant outside.
    LHAT_TEST("a body in the initialiser captures what it shadows");
    check_text(&u,
               "var^ f = 21\n"
               "do^{\n"
               "    let^ f = f^ -> number^ { return^ f * 2 }\n"
               "    var^ n : number^ = f()\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // And with nothing outside, a body naming its own binding is the same
    // read-too-early -- recursion is this^ (15.10), not the bound name.
    LHAT_TEST("a body naming its own binding with nothing outside is refused");
    check_text(&u,
               "do^{\n"
               "    let^ g = f^ -> number^ { return^ g() }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_USED_BEFORE_DEFINED);
    unit_dispose(&u);

    // What the exemption is actually for, written one body further in: the
    // two bodies read each other, and each is bound outside the one reading
    // it. Losing this to the rule above would be losing 8.7 itself.
    LHAT_TEST("mutual recursion written inside a body keeps working");
    check_text(&u,
               "var^ outer = f^ -> bool^ {\n"
               "  var^ isEven = f^ n:number^ -> bool^ { return^ isOdd(n) }\n"
               "  var^ isOdd = f^ n:number^ -> bool^ { return^ isEven(n) }\n"
               "  return^ isEven(4)\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and so does a body naming itself");
    check_text(&u,
               "var^ outer = f^ -> number^ {\n"
               "  var^ down = f^ n:number^ -> number^ {\n"
               "    if^ n <= 0 { return^ 0 }\n"
               "    return^ this^(n - 1)\n"
               "  }\n"
               "  return^ down(4)\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 03 の 3.4改2: neither half is annotated, so the walk that reads 'b'
    // before it was inferred is walked again from what it learned -- and what
    // it learns is that 'a' can answer a number, since 'b' does. The
    // annotation on the binding below is the one that is wrong.
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

    // The same pair, both sides telling the truth about what they return.
    LHAT_TEST("and a correct annotation on both sides checks cleanly");
    check_text(&u,
               "var^ a = f^ n:number^ -> number^|bool^ {\n"
               "  if^ n <= 0 { return^ true^ }\n"
               "  return^ b(n - 1)\n"
               "}\n"
               "var^ b = f^ n:number^ -> number^|bool^ {\n"
               "  if^ n <= 0 { return^ 999 }\n"
               "  return^ a(n - 1)\n"
               "}\n"
               "var^ x : number^|bool^ = a(4)\n"
               "var^ y : number^|bool^ = b(4)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 3.4改2: the second walk is what checks a forward call against what the
    // body promised. 'a' says bool^ and hands back what 'b' answers, which is
    // wider -- on the first walk 'b' was a mark and the return was forgiven.
    LHAT_TEST("and a promise the forward call breaks is caught by the rewalk");
    check_text(&u,
               "var^ a = f^ n:number^ -> bool^ {\n"
               "  if^ n <= 0 { return^ true^ }\n"
               "  return^ b(n - 1)\n"
               "}\n"
               "var^ b = f^ n:number^ -> number^|bool^ {\n"
               "  if^ n <= 0 { return^ 999 }\n"
               "  return^ a(n - 1)\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
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

    // 03 の 3.1③: nothing in this body says what `n` is, and strict leaves no
    // such hole in a signature -- every caller would be handed it. What is
    // refused is the parameter, not the call through it.
    LHAT_TEST("but an unannotated parameter nothing demands is reported");
    check_text(&u,
               "var^ f = f^ n -> number^ {\n"
               "  return^ 1\n"
               "}\n"
               "var^ x : number^ = f(\"anything\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PARAM_UNDECIDED);
    unit_dispose(&u);

    // 13.7: a position that really does take anything is written any^, which
    // says it where a reader of the signature can see it.
    LHAT_TEST("and any^ is how to say it takes anything");
    check_text(&u,
               "var^ f = f^ n:any^ -> number^ {\n"
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
               "    return^ n * this^(n - 1)\n"
               "}\n"
               "var^ n : number^ = fact(5)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the recursive exit does not widen it");
    check_text(&u,
               "var^ fact = f^ n:number^ {\n"
               "    if^ n <= 1 { return^ 1 }\n"
               "    return^ this^(n - 1)\n"
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
               "    if^ x > 1 { return^ tag(this^(x - 1)) }\n"
               "    return^ 1\n"
               "}\n"
               "var^ v : number^|string^ = f(3)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the union really is both arms");
    check_text(&u,
               "var^ tag = f^ v:any^ -> string^ { return^ \"t\" }\n"
               "var^ f = f^ x:number^ {\n"
               "    if^ x > 1 { return^ tag(this^(x - 1)) }\n"
               "    return^ 1\n"
               "}\n"
               "var^ v : number^ = f(3)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 12.8 and 03 の 5.6 leave no other way out, so a body every exit of
    // which calls itself can never produce a value.
    LHAT_TEST("a body whose every exit is recursive is reported");
    check_text(&u, "var^ f = f^ n:number^ { return^ this^(n) }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NEVER_RETURNS);
    unit_dispose(&u);

    LHAT_TEST("falling out of the body is an exit, so this one is not that");
    check_text(&u,
               "var^ f = p^ n:number^ {\n"
               "    if^ n > 0 { return^ this^(n - 1) }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.2: not writing '-> …' is what says a signature answers nothing.
    // There is no second spelling for it -- '-> ;' is not a way to say the
    // same thing, so a reader meets one form and typeof^ writes one form.
    LHAT_TEST("a signature answering nothing writes no arrow");
    check_text(&u,
               "var^ f : p^; = p^ { }\n"
               "var^ g : p^number^, number^; = p^ a:number^, b:number^ { }\n"
               "var^ h : p^ -> number^; = p^ -> number^ { return^ 1 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and '-> ;' is not a signature");
    check_text(&u, "var^ f : p^ -> ; = p^ { }\n");
    LHAT_CHECK(syntax_errors(&u) > 0, "expected a syntax error");
    unit_dispose(&u);

    LHAT_TEST("nor with arguments before it");
    check_text(&u, "var^ f : p^number^ -> ; = p^ a:number^ { }\n");
    LHAT_CHECK(syntax_errors(&u) > 0, "expected a syntax error");
    unit_dispose(&u);

    // 13.2: an empty argument side is not written by leaving the types out --
    // 'p^number^;' would then be two readings of one form. 13.9's coroutine
    // first slot is where that shape is used instead.
    LHAT_TEST("a signature taking nothing still writes its arrow");
    check_text(&u,
               "var^ f : p^ -> number^; = p^ -> number^ { return^ 1 }\n"
               "var^ n : number^ = f()\n");
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

    // 9.11: a next^ is not a way out. It ends the turn and the loop goes on,
    // so an endless one with nothing but next^ in it is still endless.
    LHAT_TEST("but a next^ is not a way out of the loop");
    check_text(&u, "var^ f = f^ -> number^ { repeat^ { next^ } }\n");
    CHECK_CLEAN(&u);
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
    check_text(&u, "var^ f = f^ n:number^ -> number^ { return^ this^(n) }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

// 03 の 3.4: a parameter written without a type is what the body demands of
// it. Not unification -- the demands are collected and intersected, which is
// why 11.3's structural reading is what makes this work rather than what makes
// it hard.
// 03 の 3.4改3: what an operator demands of a parameter is the union of the
// types that could carry it -- 11.8's built-in, and whatever this unit wrote
// an op^ of the name on. With the built-in alone the demand is number^, which
// is what it has always been; the cases above are all of that kind.
static void test_operator_candidates(void)
{
    Unit u;

    // 11.3改: A writes both orders, but only one of them is this one, so
    // there is one arm to read the right operand's type off.
    LHAT_TEST("one side written settles the other");
    check_text(&u,
               "var^ A = def^{\n"
               "  self^{ a := 0 },\n"
               "  op^* := f^self^, r:number^ -> Self^ { return^ self^ },\n"
               "  overload^op^* := f^l:number^, self^ -> Self^ { return^ self^ },\n"
               "}\n"
               "var^ f = f^ x:A, y { return^ x * y }\n"
               "var^ r = f(A.new(), 2)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the other side is checked against what that arm takes");
    check_text(&u,
               "var^ A = def^{\n"
               "  self^{ a := 0 },\n"
               "  op^* := f^self^, r:number^ -> Self^ { return^ self^ },\n"
               "  overload^op^* := f^l:number^, self^ -> Self^ { return^ self^ },\n"
               "}\n"
               "var^ f = f^ x:A, y { return^ x * y }\n"
               "var^ r = f(A.new(), \"s\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // Two arms for this same order disagree about what they take, so there is
    // nothing to read a demand off.
    LHAT_TEST("but two arms of one order settle nothing");
    check_text(&u,
               "var^ B = def^{\n"
               "  self^{ b := 0 },\n"
               "  op^* := f^self^, r:number^ -> number^ { return^ r },\n"
               "  overload^op^* := f^self^, r:string^ -> number^ "
               "{ return^ 0 },\n"
               "}\n"
               "var^ f = f^ x:B, y { return^ x * y }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PARAM_UNDECIDED);
    unit_dispose(&u);

    // With neither side written the demand is the union, and the body says
    // that nothing here picks between them.
    LHAT_TEST("neither side written demands the candidates");
    check_text(&u,
               "var^ A = def^{\n"
               "  self^{ a := 0 },\n"
               "  op^* := f^self^, r:number^ -> Self^ { return^ self^ },\n"
               "  overload^op^* := f^l:number^, self^ -> Self^ { return^ self^ },\n"
               "}\n"
               "var^ f = f^ x, y { return^ x * y }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OPERATOR_UNSETTLED);
    unit_dispose(&u);

    // The union is what the parameters hold, which is the whole point of
    // making it: a call may hand over either candidate, on either side.
    LHAT_TEST("and the union is what the parameters take");
    check_text(&u,
               "var^ A = def^{\n"
               "  self^{ a := 0 },\n"
               "  op^* := f^self^, r:number^ -> Self^ { return^ self^ },\n"
               "  overload^op^* := f^l:number^, self^ -> Self^ { return^ self^ },\n"
               "}\n"
               "var^ f = f^ x, y { return^ x * y }\n"
               "var^ a = f(A.new(), 2)\n"
               "var^ b = f(2, A.new())\n");
    CHECK_NOT_REPORTED(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("while what carries neither is still refused");
    check_text(&u,
               "var^ A = def^{\n"
               "  self^{ a := 0 },\n"
               "  op^* := f^self^, r:number^ -> Self^ { return^ self^ },\n"
               "  overload^op^* := f^l:number^, self^ -> Self^ { return^ self^ },\n"
               "}\n"
               "var^ f = f^ x, y { return^ x * y }\n"
               "var^ a = f(\"s\", 2)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // Per operator name, not per unit: writing op^* says nothing about '+'.
    LHAT_TEST("a name nobody wrote is the built-in alone");
    check_text(&u,
               "var^ A = def^{\n"
               "  self^{ a := 0 },\n"
               "  op^* := f^self^, r:number^ -> Self^ { return^ self^ },\n"
               "  overload^op^* := f^l:number^, self^ -> Self^ { return^ self^ },\n"
               "}\n"
               "var^ f = f^ x, y -> number^ { return^ x + y }\n"
               "var^ n : number^ = f(1, \"a\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    CHECK_NOT_REPORTED(&u, LHAT_CHECK_ERR_OPERATOR_UNSETTLED);
    unit_dispose(&u);

    // 8.7 makes the name visible over the whole unit, so a body written above
    // the def^ has to read the same candidates one written below it does.
    LHAT_TEST("where the def^ stands does not change the answer");
    check_text(&u,
               "var^ f = f^ x, y { return^ x * y }\n"
               "var^ A = def^{\n"
               "  self^{ a := 0 },\n"
               "  op^* := f^self^, r:number^ -> Self^ { return^ self^ },\n"
               "  overload^op^* := f^l:number^, self^ -> Self^ { return^ self^ },\n"
               "}\n"
               "var^ a = f(A.new(), 2)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OPERATOR_UNSETTLED);
    CHECK_NOT_REPORTED(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 13.11: the other way out. The union is written down and the body picks
    // an arm by narrowing to it, which is what the signature was naming.
    LHAT_TEST("narrowing the union picks an arm");
    check_text(&u,
               "var^ A = def^{\n"
               "  self^{ a := 0 },\n"
               "  op^* := f^self^, r:number^ -> Self^ { return^ self^ },\n"
               "  overload^op^* := f^l:number^, self^ -> Self^ { return^ self^ },\n"
               "}\n"
               "var^ f = f^ x:A|number^ -> number^ {\n"
               "  if^ x isa^ A { return^ 1 }\n"
               "  return^ x * 2\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 11.9: '<=>' is the one comparison a type writes, and both built-ins
    // carry it -- so an ordering's candidates start as two. The other operand
    // is what tells them apart.
    LHAT_TEST("an ordering is decided by what it is compared against");
    check_text(&u,
               "var^ f = f^ x -> bool^ { return^ x < 1 }\n"
               "var^ a : bool^ = f(2)\n"
               "var^ b = f(\"s\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and string^ carries it just as well");
    check_text(&u,
               "var^ f = f^ x -> bool^ { return^ x < \"s\" }\n"
               "var^ a : bool^ = f(\"t\")\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("but neither side written leaves the candidates open");
    check_text(&u, "var^ f = f^ x, y -> bool^ { return^ x < y }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OPERATOR_UNSETTLED);
    unit_dispose(&u);

    // 11.9 with 14.2: equality answers without a '<=>' at all, so what it
    // asks is that the two are not disjoint -- which is not a type to demand.
    LHAT_TEST("equality demands nothing");
    check_text(&u, "var^ f = f^ x -> bool^ { return^ x = 1 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PARAM_UNDECIDED);
    unit_dispose(&u);

    // 11.9改: a type writing op^= says how it compares for equality and
    // nothing about order. The carriers are held per operator name, so it is
    // no candidate for '<'.
    LHAT_TEST("an op^= alone is no candidate for an ordering");
    check_text(&u,
               "var^ S = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^= := f^self^, o:Self^ -> bool^ { return^ true^ },\n"
               "}\n"
               "var^ f = f^ x -> bool^ { return^ x < 1 }\n"
               "var^ a : bool^ = f(2)\n"
               "var^ b = f(S.new())\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    CHECK_NOT_REPORTED(&u, LHAT_CHECK_ERR_OPERATOR_UNSETTLED);
    unit_dispose(&u);

    LHAT_TEST("and ordering two of them is still refused");
    check_text(&u,
               "var^ S = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^= := f^self^, o:Self^ -> bool^ { return^ true^ },\n"
               "}\n"
               "var^ r = S.new() < S.new()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_ORDERED);
    unit_dispose(&u);

    LHAT_TEST("a written op^<=> is a candidate for an ordering");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^<=> := f^self^, o:Self^ -> number^ { return^ 0 },\n"
               "}\n"
               "var^ f = f^ x -> bool^ { return^ x < V.new() }\n"
               "var^ a : bool^ = f(V.new())\n"
               "var^ b = f(2)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 11.9 gives '<=>' to string^ too, which the built-in seed left out.
    LHAT_TEST("'<=>' written directly reaches string^ as well");
    check_text(&u,
               "var^ f = f^ x -> number^ { return^ x <=> \"s\" }\n"
               "var^ n : number^ = f(\"t\")\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The same narrowing on the arithmetic side: an arm that could not take
    // the operand is no candidate, so one carrier is left and no union forms.
    LHAT_TEST("the other operand narrows an arithmetic field too");
    check_text(&u,
               "var^ W = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^* := f^self^, r:string^ -> number^ { return^ 0 },\n"
               "}\n"
               "var^ f = f^ x -> number^ { return^ x * 2 }\n"
               "var^ n : number^ = f(3)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

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
    // the signature, so it is not a demand -- see 03 の 3.4. Nothing else
    // demands anything either, so 3.1③ reports the parameter.
    LHAT_TEST("calling a parameter demands nothing");
    check_text(&u,
               "var^ f = p^ x { x() }\n"
               "f(5)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PARAM_UNDECIDED);
    unit_dispose(&u);

    // 13.11: the demand belongs to the narrowed value, not to what arrived.
    // Counting it would undo the narrowing the writer put there -- so this
    // one is undecided as well.
    LHAT_TEST("a use under a narrowing is not a demand");
    check_text(&u,
               "var^ f = p^ x {\n"
               "    if^ x isa^ number^ { var^ n : number^ = x + 1 }\n"
               "}\n"
               "f(\"a\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PARAM_UNDECIDED);
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

    // Two branches ruling each other out leave nothing decided, which under
    // strict is the same answer as nothing having been demanded at all: the
    // writer has to say which one the position takes.
    LHAT_TEST("branches that disagree decide nothing");
    check_text(&u,
               "var^ f = p^ b:bool^, x {\n"
               "    if^ b { var^ n : number^ = x + 1 else^: x.write() }\n"
               "}\n"
               "f(true^, \"a\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PARAM_UNDECIDED);
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

    // 3.4: what the name holds after a reassignment is not what was passed in,
    // so the demand below it says nothing about the parameter and nothing is
    // left to decide it.
    LHAT_TEST("a reassignment ends the collection");
    check_text(&u,
               "var^ f = p^ x {\n"
               "    x := \"z\"\n"
               "    var^ n : number^ = x + 1\n"
               "}\n"
               "f(\"a\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PARAM_UNDECIDED);
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

    // 05 の 4 章 asks nothing of a signature that 3.1③ does not ask of every
    // signature: what a body decides is decided, published or not. A member
    // read is a demand, so this one is decided and nothing is owed.
    LHAT_TEST("a public^ subroutine is read the same as any other");
    check_text(&u, "public^ let^ f = p^ x { x.write() }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and one nothing decides is reported wherever it stands");
    check_text(&u, "public^ let^ f = p^ x { }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PARAM_UNDECIDED);
    unit_dispose(&u);

    LHAT_TEST("a signature on the binding decides it as plainly");
    check_text(&u,
               "public^ let^ f : p^t^{ write : p^; }; = p^ x { x.write() }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.2's form for answering nothing is what an omitted result already
    // means where no exit produces a value, so nothing has to be written.
    LHAT_TEST("a body answering nothing needs no result written");
    check_text(&u, "public^ let^ f = p^ x:number^ { }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.5 with 13.9: what a call answers is the coroutine, and 15.2 settles
    // its R and Y off the yield^ sites -- publishing it asks for no writing.
    LHAT_TEST("nor does a yielding one have to write its coroutine");
    check_text(&u, "public^ let^ g = p^ n:number^ { yield^ n }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("though writing it out is still read the same way");
    check_text(&u,
               "public^ let^ g = p^ n:number^"
               " -> c^{ p^-> number^} { yield^ n }\n");
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

    // 03 の 3.4改2: 3.1's own example, which the second walk settles -- the
    // one thing the first walk could not read is what the second one starts
    // from. Nothing is written and nothing is reported.
    LHAT_TEST("a mutually recursive pair settles without an annotation");
    check_text(&u,
               "let^ a = f^ n:number^ {\n"
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

    // What the seed drops is only a gap: an arm that really belongs is read
    // off the body again on the walk after it. 'b' answers a string^, so 'a'
    // answers one too, and saying bool^ of it is a mismatch.
    LHAT_TEST("and the ring settles on what the bodies really answer");
    check_text(&u,
               "let^ a = f^ n:number^ {\n"
               "    if^ n <= 0 { return^ true^ }\n"
               "    return^ b(n - 1)\n"
               "}\n"
               "let^ b = f^ n:number^ {\n"
               "    if^ n <= 0 { return^ \"x\" }\n"
               "    return^ a(n - 1)\n"
               "}\n"
               "let^ x : bool^|string^ = a(4)\n"
               "let^ y : bool^|string^ = b(4)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A ring whose other half never settles keeps its gap however often it is
    // walked -- 'b' demands nothing of x, so what it answers stays undecided
    // and so does 'a'. Reported at both definitions, and once more at the x
    // that started it (3.1③).
    LHAT_TEST("a ring that cannot settle says so at both definitions");
    check_text(&u,
               "let^ a = f^ n:number^ {\n"
               "    if^ n <= 0 { return^ true^ }\n"
               "    return^ b(n)\n"
               "}\n"
               "let^ b = f^ x { return^ x }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_RESULT_UNDECIDED);
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PARAM_UNDECIDED);
    LHAT_CHECK_EQ_INT(u.checked.diagnostic_count, 3);
    unit_dispose(&u);

    // 3.5: relaxed leaves an undecided type to the machine, so it says
    // nothing here -- the same program, the same types, a different setting.
    LHAT_TEST("relaxed forgives the whole of it");
    check_relaxed_text(&u,
                       "let^ a = f^ n:number^ {\n"
                       "    if^ n <= 0 { return^ true^ }\n"
                       "    return^ b(n)\n"
                       "}\n"
                       "let^ b = f^ x { return^ x }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The gap is in a union arm, where lhat_type_conforms's leniency would
    // hide it: 'bool^|pending^' is not a bool^ where one is wanted.
    LHAT_TEST("a gap buried in a union is reported where it lands as well");
    check_text(&u,
               "let^ a = f^ n:number^ {\n"
               "    if^ n <= 0 { return^ true^ }\n"
               "    return^ b(n)\n"
               "}\n"
               "let^ b = f^ x { return^ x }\n"
               "let^ y : bool^ = a(4)\n");
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

    // 3.1③: a parameter is read the same way a result is. Nothing demanded
    // anything of this one and nothing was written, so its place in the
    // signature is a hole every caller would be handed.
    LHAT_TEST("a parameter nothing demands is reported on its own");
    check_text(&u, "var^ f = p^ x { }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PARAM_UNDECIDED);
    unit_dispose(&u);

    // 3.1③: a gap that survives into what a name holds is the same hole said
    // of a binding. Every reader of the name would be handed it.
    LHAT_TEST("a gap reaching a binding is reported at the name");
    check_text(&u,
               "let^ a = f^ n:number^ {\n"
               "    if^ n <= 0 { return^ true^ }\n"
               "    return^ b(n)\n"
               "}\n"
               "let^ b = f^ x { return^ x }\n"
               "let^ y = a(4)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TYPE_UNDECIDED);
    unit_dispose(&u);

    // 3.5: the same ones, left to the machine. 3.2 keeps the source identical
    // between the two settings -- only when it is said changes.
    LHAT_TEST("and relaxed forgives that too");
    check_relaxed_text(&u, "var^ f = p^ x { }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the binding as well");
    check_relaxed_text(&u,
                       "let^ a = f^ n:number^ {\n"
                       "    if^ n <= 0 { return^ true^ }\n"
                       "    return^ b(n)\n"
                       "}\n"
                       "let^ b = f^ x { return^ x }\n"
                       "let^ y = a(4)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a published one is no different");
    check_relaxed_text(&u, "public^ let^ f = p^ x { }\n");
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

    // 8.7 makes every name of a list visible throughout it wherever the list
    // stands, so the walks are run over a body's statements the same way.
    LHAT_TEST("a ring inside a body settles the same way");
    check_text(&u,
               "var^ make = p^ -> number^ {\n"
               "    let^ up = f^ n:number^ {\n"
               "        if^ n <= 0 { return^ 0 }\n"
               "        return^ down(n - 1)\n"
               "    }\n"
               "    let^ down = f^ n:number^ { return^ up(n - 1) }\n"
               "    return^ up(4)\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // Each walk says everything the last one said, so what the last one said
    // is all that is kept -- one mistake inside a ring is one diagnostic.
    LHAT_TEST("a mistake inside a ring is reported once");
    check_text(&u,
               "let^ a = f^ n:number^ {\n"
               "    if^ n <= 0 { return^ nowhere }\n"
               "    return^ b(n - 1)\n"
               "}\n"
               "let^ b = f^ n:number^ { return^ a(n - 1) }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
    LHAT_CHECK_EQ_INT(u.checked.diagnostic_count, 1);
    unit_dispose(&u);

    // A statement binds as well as answering -- 8.8's path grows a table --
    // and a second walk over it must not read its own first walk as a second
    // definition.
    LHAT_TEST("what a walk bound is not a redefinition on the next walk");
    check_text(&u,
               "var^ t = { }\n"
               "var^ t.m = 1\n"
               "let^ a = f^ n:number^ {\n"
               "    if^ n <= 0 { return^ true^ }\n"
               "    return^ b(n - 1)\n"
               "}\n"
               "let^ b = f^ n:number^ { return^ a(n - 1) }\n"
               "let^ x : bool^ = a(4)\n"
               "let^ y : number^ = t.m\n");
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

    // 2.3: types and no values, so a kind standing where a value was wanted
    // is a type written in the wrong place. It reads as a member access --
    // the set is a name in scope -- which is why saying "no such member"
    // would send a writer hunting for a spelling mistake instead of for the
    // error^ 2.5 puts in front.
    LHAT_TEST("2.3: a kind written as a value says which it is");
    check_text(&u,
               "errordef^ IOError { NotFound, Denied }\n"
               "var^ e = IOError.NotFound\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_KIND_AS_VALUE);
    unit_dispose(&u);

    LHAT_TEST("and a name the set never declared is still no such member");
    check_text(&u,
               "errordef^ IOError { NotFound, Denied }\n"
               "var^ e = IOError.Nosuch\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // The two the set does answer (2.3's table) are reached through a value,
    // and reaching them through the set itself is not this.
    LHAT_TEST("and message and cause are untouched");
    check_text(&u,
               "errordef^ IOError { NotFound, Denied }\n"
               "var^ e = error^IOError.NotFound{ }\n"
               "var^ s : string^ = e.message\n");
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

    // A signature on the binding decides both sides, so 3.1③ has nothing left
    // to report -- whether or not the name is published.
    LHAT_TEST("and a published one is decided by it just the same");
    check_text(&u,
               "public^ let^ f : p^string^ -> string^; = p^ x { return^ x }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 02 の 13.7 and 04 の 7 章: no generic types, so nothing decides this one
    // wherever it stands. Publishing it neither adds to that nor takes away.
    LHAT_TEST("and with nothing written anywhere it is undecided as ever");
    check_text(&u, "public^ let^ f = p^ x { return^ x }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PARAM_UNDECIDED);
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

    // 03 の 3.4改: the callee's written signature stands beside an argument,
    // so a literal written there takes the parameters nothing was written on
    // from the position it is going into. What decides them is the callee's
    // own signature, not a caller elsewhere -- 3.4's line is about the other
    // direction.
    LHAT_TEST("a literal argument takes its parameters from the position");
    check_text(&u,
               "let^ ff = f^ a:number^, b:string^, g:f^number^, string^ -> "
               "bool^; -> bool^ { g(a, b) }\n"
               "var^ r = ff(1, \"s\", f^ c, d { c > 0 })\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the body is checked against what the position filled in");
    check_text(&u,
               "let^ ff = f^ g:f^number^ -> bool^; -> bool^ { g(1) }\n"
               "var^ r = ff(f^ x { x .. \"s\" })\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    // 14.4: a member call hands the receiver over without writing it, and
    // the parameters line up after it.
    LHAT_TEST("a member call fills in a literal argument too");
    check_text(&u,
               "let^ D = def^ {\n"
               "    run = f^ self^, g:f^number^ -> bool^; -> bool^ { g(1) },\n"
               "}\n"
               "var^ d = D.new()\n"
               "var^ r = d.run(f^ x { true^ })\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.4: written out, the receiver takes the first position and the
    // parameters follow it. The shift is one, and it is read off the way the
    // call is written -- so what comes after still lines up.
    LHAT_TEST("and so does one whose receiver is written out");
    check_text(&u,
               "let^ D = def^ {\n"
               "    run = f^ self^, g:f^number^ -> bool^; -> bool^ { g(1) },\n"
               "}\n"
               "var^ d = D.new()\n"
               "let^ m = D.run\n"
               "var^ r = m(d, f^ x { true^ })\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.12: an overloaded member is an intersection, and which arm a call
    // means is settled by the arguments' own types -- so there is no one
    // position to read an expectation off of. The annotation is still owed
    // here, which is what this pins.
    LHAT_TEST("but an overloaded one has no one position to read");
    check_text(&u,
               "let^ D = def^ {\n"
               "    run = f^ self^, g:f^number^ -> bool^; -> bool^ { g(1) },\n"
               "    overload^run := f^ self^, s:string^ -> bool^ "
               "{ s.length > 0 },\n"
               "}\n"
               "var^ d = D.new()\n"
               "var^ r = d.run(f^ x { true^ })\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PARAM_UNDECIDED);
    unit_dispose(&u);

    // 13.2: a written result stands beside the value the same way, so a
    // literal returned into it is filled in as an argument is.
    LHAT_TEST("a written result fills in a literal returned into it");
    check_text(&u,
               "let^ mk = f^ -> f^number^ -> bool^; { return^ f^ x { x > 0 } }\n"
               "var^ r = mk()(3)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 8.6: and what a name already holds says it at a reassignment, which is
    // the define's own rule read from the other side.
    LHAT_TEST("and what a name holds fills in a reassigned literal");
    check_text(&u,
               "var^ cb : f^number^ -> bool^; = f^ n { n > 0 }\n"
               "cb := f^ y { true^ }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.4: a default is written into a call site, so the position it will
    // be written into is what it is expected to fit.
    LHAT_TEST("and a written parameter type fills in its own default");
    check_text(&u,
               "let^ take = f^ g:f^number^ -> bool^; = f^ z { true^ } "
               "-> bool^ { g(1) }\n"
               "var^ r = take(f^ w { w > 100 })\n");
    CHECK_CLEAN(&u);
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

    // 03 の 3.4: with no result written, a try^ is one of the exits the
    // result is the union of -- an error leaves the body there as plainly as
    // a value leaves it through a return^.
    LHAT_TEST("a try^ is an exit the inferred result takes in");
    check_text(&u,
               "errordef^ IOError { NotFound }\n"
               "var^ open = f^ -> number^|IOError { return^ 0 }\n"
               "var^ read = f^ { return^ try^ open() }\n"
               "var^ n : number^ = read() catch^ 0\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the arm has to be dealt with, like any other");
    check_text(&u,
               "errordef^ IOError { NotFound }\n"
               "var^ open = f^ -> number^|IOError { return^ 0 }\n"
               "var^ read = f^ { return^ try^ open() }\n"
               "var^ n : number^ = read()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // A p^ that makes no value of its own still says it may fail: 3.4 adds
    // nil^ beside the error, since leaving without a value is the other exit.
    LHAT_TEST("a p^ that only lets an error out says so too");
    check_text(&u,
               "errordef^ IOError { NotFound }\n"
               "var^ open = f^ -> number^|IOError { return^ 0 }\n"
               "var^ run = p^ { let^ n = try^ open() }\n"
               "var^ x : nil^ = run() catch^ nil^\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 04 の 4.1 with 5.3: the shape a writer reaches for when the failing
    // work is a block rather than a call -- wrap it, call it where it stands,
    // and catch^ what comes out. 4.5 is what replaces it.
    LHAT_TEST("a body written and called where it stands can be caught");
    check_text(&u,
               "errordef^ IOError { NotFound }\n"
               "var^ open = f^ -> number^|IOError { return^ 0 }\n"
               "var^ r : bool^ = p^ {\n"
               "    let^ n = try^ open()\n"
               "    return^ true^\n"
               "}() catch^ f^ { return^ false^ }()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 04 の 4.5: inside a try^{ }, a try^ hands its errors to the arms rather
    // than to the result -- so a body answering number^ alone is honest here
    // even though what it calls may fail.
    LHAT_TEST("a try^{ } takes the errors off the result");
    check_text(&u,
               "errordef^ IOError { NotFound, Denied }\n"
               "var^ open = f^ -> number^|IOError { return^ 0 }\n"
               "var^ read = f^ -> number^ {\n"
               "    try^{\n"
               "        return^ try^ open()\n"
               "    catch^:\n"
               "        return^ 0\n"
               "    }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 4.2 with 6.1: it^ is the error, narrowed to the arm's own kind, so what
    // that kind declares is visible there.
    LHAT_TEST("and it^ is narrowed to the arm's kind");
    check_text(&u,
               "errordef^ IOError { NotFound { path : string^ }, Denied }\n"
               "var^ open = f^ -> number^|IOError { return^ 0 }\n"
               "var^ read = f^ -> string^ {\n"
               "    try^{\n"
               "        var^ n = try^ open()\n"
               "        return^ \"\"\n"
               "    catch^ IOError.NotFound:\n"
               "        return^ it^.path\n"
               "    catch^:\n"
               "        return^ it^.message\n"
               "    }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 4.5: what no arm takes leaves as a bare try^ would have sent it, so
    // 5.3 asks the result about it -- at the block, where it leaves.
    LHAT_TEST("and what no arm takes is held to 5.3");
    check_text(&u,
               "errordef^ IOError { NotFound, Denied }\n"
               "var^ open = f^ -> number^|IOError { return^ 0 }\n"
               "var^ read = f^ -> number^|IOError.NotFound {\n"
               "    try^{\n"
               "        return^ try^ open()\n"
               "    catch^ IOError.NotFound:\n"
               "        return^ 0\n"
               "    }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TRY_OUTSIDE);
    unit_dispose(&u);

    // And nothing to catch is 4.1's line, one construct over.
    LHAT_TEST("a try^{ } with no try^ inside it catches nothing");
    check_text(&u,
               "var^ go = p^ {\n"
               "    try^{\n"
               "        var^ n = 1\n"
               "    catch^:\n"
               "        var^ m = 2\n"
               "    }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_CATCHES_NOTHING);
    unit_dispose(&u);

    // A body written inside the block is a body: its own try^ belongs to it,
    // and the arms out here never see it.
    LHAT_TEST("and a body inside the block keeps its own try^");
    check_text(&u,
               "errordef^ IOError { NotFound }\n"
               "var^ open = f^ -> number^|IOError { return^ 0 }\n"
               "var^ go = p^ {\n"
               "    try^{\n"
               "        var^ inner = f^ { return^ try^ open() }\n"
               "    catch^:\n"
               "        var^ m = 2\n"
               "    }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_CATCHES_NOTHING);
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
    check_text(&u, "var^ c : c^{ p^nil^ -> Self^ -> nil^ } = 1\n");
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
               "var^ x : t^{} = { a := 1 }\n"
               "var^ y : t^{} = { 1, 2 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and nothing that is not one");
    check_text(&u, "var^ x : t^{} = 5\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("a bare one joins a union like any other type");
    check_text(&u,
               "var^ x : t^{}|nil^ = { a := 1 }\n"
               "var^ y : nil^|t^{} = nil^\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.10: the members come with the word, so the braces are not optional
    // -- a signature followed by a bare t^ would leave the '{' after it
    // reading as the list rather than as the body.
    LHAT_TEST("and the braces are what say it asks for none");
    check_text(&u, "var^ x : t^ = { a := 1 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_BARE_TABLE_TYPE);
    unit_dispose(&u);

    LHAT_TEST("said of a bare one inside a union as well");
    check_text(&u, "var^ y : nil^|t^ = nil^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_BARE_TABLE_TYPE);
    unit_dispose(&u);

    // 14.10改: 'type[n]' is n positions of that type, so it is the same type
    // the run written out is -- 14.10's "at least these" then makes the count
    // a floor, the way a run of positions written by hand already is.
    LHAT_TEST("a count says how many positions the type takes");
    check_text(&u,
               "var^ f = p^ t:t^{ number^[3] } { }\n"
               "f({ 1, 2, 3 })\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("fewer than the count does not fit");
    check_text(&u,
               "var^ f = p^ t:t^{ number^[3] } { }\n"
               "f({ 1 })\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("more than it does");
    check_text(&u,
               "var^ f = p^ t:t^{ number^[3] } { }\n"
               "f({ 1, 2, 3, 4 })\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the positions keep the type they were counted for");
    check_text(&u,
               "var^ f = p^ t:t^{ number^[3] } { }\n"
               "f({ \"a\", \"b\", \"c\" })\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("a count follows the positions written before it");
    check_text(&u,
               "var^ f = p^ t:t^{ string^, number^[2] } { }\n"
               "f({ \"a\", 1, 2 })\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The two spellings are one type, which is what a signature written with
    // either accepting the other says. The run may be written in parts, too:
    // 'type[n]' is n positions, so a run split across entries is that run.
    LHAT_TEST("the count and the run written out are one type");
    check_text(&u,
               "var^ q = p^ t:t^{ number^, number^, number^ } { }\n"
               "var^ r : p^t^{ number^[3] }; = q\n"
               "var^ s : p^t^{ number^, number^[2] }; = q\n"
               "var^ o : p^t^{ number^[1], number^[2] }; = q\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and one position may be written with a count of one");
    check_text(&u,
               "var^ f = p^ t:t^{ number^[1] } { }\n"
               "f({ 1 })\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("13.7's tail may follow a counted run");
    check_text(&u, "var^ f = p^ t:t^{ number^[2], ...:string^ } { }\n");
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

#if LHAT_WITH_RESOLUTIONS
// 07 の 4 章: lhat_check_resolution_at halves its way in, so the array has
// to be ordered by `use` with no position recorded twice. The walk itself
// does not quite give that -- a member is recorded after the target it was
// read from, and 14.7改's two passes over a def^ meet the same member twice
// -- so chk_settle_resolutions makes it hold. This is that invariant, asked
// of a source written to exercise both: nested member chains inside a def^
// whose members read each other.
static void test_resolutions_are_ordered(void)
{
    Unit u;

    LHAT_TEST("07 の 4 章: resolutions come back in source order, once each");
    check_text(&u,
               "let^ pair = { left = 1, right = 2 }\n"
               "let^ Box = def^{\n"
               "    self^{ held = 0 },\n"
               "    get = f^self^ -> number^ { return^ self^.held },\n"
               "    twice = f^self^ -> number^ { return^ self^.get() * 2 },\n"
               "}\n"
               "let^ b = Box.new()\n"
               "let^ n = b.twice() + pair.left + pair.right\n"
               // 15.10: recorded from an early answer in chk_infer_name
               // rather than from a binding, which is the other order the
               // walk can produce records in.
               "let^ fact = f^k:number^ -> number^ {\n"
               "    if^ k < 2 { return^ 1 }\n"
               "    return^ k * this^(k - 1)\n"
               "}\n");
    CHECK_CLEAN(&u);

    LHAT_CHECK(u.checked.resolution_count > 0, "expected names to resolve");
    for (size_t i = 1; i < u.checked.resolution_count; i++) {
        uint32_t before = u.checked.resolutions[i - 1].use;
        uint32_t here = u.checked.resolutions[i].use;
        LHAT_CHECK(before < here,
                   "resolution %zu is at %u, after %u -- the binary search "
                   "in lhat_check_resolution_at reads this order",
                   i, here, before);
    }

    // And every one of them is findable at any position within its name,
    // which is the whole of what the lookup promises.
    for (size_t i = 0; i < u.checked.resolution_count; i++) {
        const LhatResolution *want = &u.checked.resolutions[i];
        const LhatResolution *got =
            lhat_check_resolution_at(&u.checked, want->use);
        LHAT_CHECK(got == want, "resolution %zu was not found at its own use",
                   i);
    }
    unit_dispose(&u);
}

// 07 の 4 章: what a resolution says besides the type. 13.1's "a signature
// declared this" and 8.9's "a let^ bound it" are both things only the
// checker knows -- where a use stands says neither, and the type says
// neither. A tool asking what a name is has to be told.
static void test_resolutions_say_what_bound_the_name(void)
{
    Unit u;

    LHAT_TEST("07 の 4 章: a resolution says a parameter is one, and which "
              "word bound a name");
    check_text(&u,
               "let^ fixed = 1\n"
               "var^ moving = 2\n"
               "let^ take = f^ n:number^ -> number^ {\n"
               "    return^ n + fixed + moving\n"
               "}\n");
    CHECK_CLEAN(&u);

    // The three uses in the body, found where each stands.
    const char *body = strstr(u.source.text, "return^ ");
    LHAT_CHECK(body != NULL, "expected the body to be there");
    if (body != NULL) {
        const LhatResolution *n = lhat_check_resolution_at(
            &u.checked, (uint32_t)(strstr(body, "n ") - u.source.text));
        const LhatResolution *f = lhat_check_resolution_at(
            &u.checked, (uint32_t)(strstr(body, "fixed") - u.source.text));
        const LhatResolution *m = lhat_check_resolution_at(
            &u.checked, (uint32_t)(strstr(body, "moving") - u.source.text));

        LHAT_CHECK(n != NULL && n->is_parameter,
                   "13.1: the parameter reads as one where it is used");
        LHAT_CHECK(f != NULL && !f->is_parameter && f->immutable,
                   "8.9: a let^ name is readonly and is not a parameter");
        LHAT_CHECK(m != NULL && !m->immutable,
                   "8.9: a var^ name is not readonly");
    }
    unit_dispose(&u);
}

// 13.11 with 07 の 4 章: inside the branch that knows most about a name,
// nothing was recorded about it at all -- chk_infer_name is what records one,
// and a narrowed name answers before reaching it. So a tool asking about the
// one place the type is sharpest got the enclosing declaration instead.
static void test_a_narrowed_name_still_resolves(void)
{
    Unit u;

    LHAT_TEST("13.11: a narrowed name resolves, to what the branch knows");
    check_text(&u,
               "let^ held : number^|string^ = 1\n"
               "if^ held isa^ number^ {\n"
               "    let^ doubled = held * 2\n"
               "}\n");
    CHECK_CLEAN(&u);

    const char *narrowed = strstr(u.source.text, "held * 2");
    LHAT_CHECK(narrowed != NULL, "expected the narrowed use to be there");
    if (narrowed != NULL) {
        const LhatResolution *r = lhat_check_resolution_at(
            &u.checked, (uint32_t)(narrowed - u.source.text));
        LHAT_CHECK(r != NULL, "expected the narrowed use to resolve");
        if (r != NULL) {
            // The branch's answer, not the binding's: number^, not the union.
            LHAT_CHECK(r->type != NULL && r->type->kind == LHAT_TYPE_NUMBER,
                       "13.11: the type is what the branch established");
            // 8.9 and the place it was bound are the binding's to answer, and
            // narrowing changes neither.
            LHAT_CHECK(r->immutable, "8.9: a let^ name is readonly here too");
            LHAT_CHECK(r->has_definition, "it still points at its let^");
        }
    }
    unit_dispose(&u);
}

// 14.7改: a written-out definition is a definition. The section is what makes
// it one, and resolve_table_type has always read the section -- it just did
// not say what reading one meant. 14.5's '..' asks this of both operands and
// 14.9's built-in members ask it of a value, so the fact is the language's;
// asked here through a resolution because that is where it became visible,
// a name annotated this way reading as an ordinary table to every tool.
//
// 8.7: an annotation is what the binding holds when one is written, so
// without this the def^'s own answer never reaches the name.
static void test_a_written_definition_is_one(void)
{
    Unit u;

    LHAT_TEST("14.7改: a t^ with a self^ section is a definition");
    check_text(&u,
               "let^ Counter : t^{ self^{ n : number^ }, new : f^ -> Self^; }"
               " = def^{\n"
               "    self^{ n = 0 },\n"
               "}\n"
               "let^ made = Counter.new()\n");
    CHECK_CLEAN(&u);

    const char *use = strstr(u.source.text, "Counter.new");
    LHAT_CHECK(use != NULL, "expected the use to be there");
    if (use != NULL) {
        const LhatResolution *r = lhat_check_resolution_at(
            &u.checked, (uint32_t)(use - u.source.text));
        LHAT_CHECK(r != NULL && r->type != NULL, "expected the name to resolve");
        if (r != NULL && r->type != NULL) {
            LHAT_CHECK(r->type->kind == LHAT_TYPE_TABLE &&
                           r->type->v.table.is_definition,
                       "the annotated name holds a definition");
            // 8.8 is about a value, not about a demand written on a name --
            // a plain table that meets the shape is not closed by having been
            // asked for. Said here so that setting one is not read as
            // forgetting the other.
            LHAT_CHECK(r->type->kind == LHAT_TYPE_TABLE &&
                           !r->type->v.table.from_definition,
                       "and a written demand closes nothing");
        }
    }
    unit_dispose(&u);
}
#endif

int main(void)
{
    test_names();
#if LHAT_WITH_RESOLUTIONS
    test_resolutions_are_ordered();
    test_resolutions_say_what_bound_the_name();
    test_a_narrowed_name_still_resolves();
    test_a_written_definition_is_one();
#endif
    test_expressions();
    test_results();
    test_parameter_inference();
    test_operator_candidates();
    test_undecided_results();
    test_errors();
    test_annotations();
    return lhat_test_report("test_check_core");
}
