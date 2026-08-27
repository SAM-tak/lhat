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

// 02 の 13.2 gives a signature a form for "no result", and 03 の 3.4 keeps
// that apart from nil^ on the grounds that otherwise 11.7's `??` would apply
// to a meaningless expression. That reasoning only holds if calling such a
// subroutine produces something no value fits -- which it did not: the result
// was a NULL, and a NULL is how "not inferred" is spelled, so every use of it
// was waved through.
static void test_no_value(void)
{
    Unit u;

    // The case 03 names outright.
    LHAT_TEST("?? cannot apply to a call that produces nothing");
    check_text(&u,
               "var^ log = p^ m:string^ { var^ y = m }\n"
               "var^ v = log(\"a\") ?? 0\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_CANNOT_BE_NIL);
    unit_dispose(&u);

    // 04 の 4.1 puts catch^ on the same footing.
    LHAT_TEST("nor can catch^");
    check_text(&u,
               "var^ log = p^ m:string^ { var^ y = m }\n"
               "var^ v = log(\"a\") catch^ 0\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_CANNOT_FAIL);
    unit_dispose(&u);

    LHAT_TEST("a name cannot be bound to it");
    check_text(&u,
               "var^ log = p^ m:string^ { var^ y = m }\n"
               "var^ v = log(\"a\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("nor reassigned to one");
    check_text(&u,
               "var^ log = p^ m:string^ { var^ y = m }\n"
               "var^ v = 1\n"
               "v := log(\"a\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("an annotation does not admit it either");
    check_text(&u,
               "var^ log = p^ m:string^ { var^ y = m }\n"
               "var^ v : number^ = log(\"a\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and neither does an argument");
    check_text(&u,
               "var^ log = p^ m:string^ { var^ y = m }\n"
               "var^ v = log(log(\"a\"))\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("arithmetic needs a number, which this is not");
    check_text(&u,
               "var^ log = p^ m:string^ { var^ y = m }\n"
               "var^ v = log(\"a\") + 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    LHAT_TEST("and a condition a bool^");
    check_text(&u,
               "var^ log = p^ m:string^ { var^ y = m }\n"
               "if^ log(\"a\") { }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_BOOL);
    unit_dispose(&u);

    // Reported where it is written rather than let into the result, where it
    // would reach every caller as a type nothing inhabits.
    LHAT_TEST("a return^ cannot carry it");
    check_text(&u,
               "var^ log = p^ m:string^ { var^ y = m }\n"
               "var^ f = p^ { return^ log(\"a\") }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("with a written result saying so too");
    check_text(&u,
               "var^ log = p^ m:string^ { var^ y = m }\n"
               "var^ f = p^ -> number^ { return^ log(\"a\") }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 8.2: a call is the one expression that stands alone as a statement, and
    // that is where a subroutine with no result belongs.
    LHAT_TEST("but calling it as a statement is the point of it");
    check_text(&u,
               "var^ log = p^ m:string^ { var^ y = m }\n"
               "log(\"a\")\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 03 の 3.4 reads a NULL result as "still being worked out", so a body
    // calling itself has to keep getting one.
    LHAT_TEST("and a self-call is still a result being inferred");
    check_text(&u,
               "var^ fact = f^ n:number^ {\n"
               "    if^ n <= 1 { return^ 1 }\n"
               "    return^ n * this^(n - 1)\n"
               "}\n"
               "var^ n : number^ = fact(5)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.9: a coroutine that cannot end never produces a return value, so
    // delegating to one produces nothing either.
    LHAT_TEST("delegating to a coroutine that cannot end produces nothing");
    check_text(&u,
               "var^ g = p^ { repeat^ { yield^ 1 } }\n"
               "var^ o = p^ { var^ v = await^ g() }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // Every position that wants a value says so. A statement is the one that
    // does not (8.2), and it is checked above.
    LHAT_TEST("a table entry wants a value");
    check_text(&u,
               "var^ log = p^ m:string^ { var^ y = m }\n"
               "var^ t = { a := log(\"x\") }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and so does a positional one");
    check_text(&u,
               "var^ log = p^ m:string^ { var^ y = m }\n"
               "var^ t = { log(\"x\") }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("an index key wants one");
    check_text(&u,
               "var^ log = p^ m:string^ { var^ y = m }\n"
               "var^ t = { 1 }\n"
               "var^ v = t[log(\"x\")]\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("as^ wants one to ascribe");
    check_text(&u,
               "var^ log = p^ m:string^ { var^ y = m }\n"
               "var^ v = log(\"x\") as^ number^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);


    // 11.3 leaves what '..' means to op^, but it cannot mean anything at all
    // when handed nothing.
    LHAT_TEST("'..' wants one on either side");
    check_text(&u,
               "var^ log = p^ m:string^ { var^ y = m }\n"
               "var^ v = log(\"x\") .. \"a\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 11.2: '..' joins what answers it, and 11.3 settles that here. The
    // machine already refused these; the checker had been silent.
    LHAT_TEST("a number does not answer '..'");
    check_text(&u, "var^ v = 1 .. \"b\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    LHAT_TEST("on either side of it");
    check_text(&u, "var^ v = \"a\" .. 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    LHAT_TEST("nor does a bool^ or a nil^");
    check_text(&u, "var^ v = true^ .. \"a\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    // Every arm has to answer, since the value may be any of them. Said with
    // the code 03 の 1 章 keeps for the nil^ arm: the rest of this union does
    // answer '..', so what is asked for is a narrowing rather than an
    // operator nobody wrote.
    LHAT_TEST("and a union answers only when all of it does");
    check_text(&u,
               "var^ x : string^|nil^ = \"a\"\n"
               "var^ v = x .. \"b\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL);
    unit_dispose(&u);

    LHAT_TEST("and when no arm of it does, the plain refusal stands");
    check_text(&u,
               "var^ x : number^|nil^ = 1\n"
               "var^ v = x .. \"b\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    LHAT_TEST("a string answers, which is 11.2's first example");
    check_text(&u, "var^ v : string^ = \"a\" .. \"b\" .. \"c\"\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.5: between two definitions '..' composes, and never calls an op^..
    // either of them carries. 14.7 gives a definition and an instance the
    // same members, so what tells them apart is what made them.
    LHAT_TEST("and between two definitions it composes");
    check_text(&u,
               "var^ Foo = def^{ self^{} }\n"
               "var^ Baz = def^{ self^{} }\n"
               "var^ A = Foo .. Baz\n"
               "var^ B = Foo .. def^{ self^{} }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 11.1: an operator is a function, carried by the type as a member whose
    // name is the operator. 01 の 6 章 keeps that name unwritable by hand.
    LHAT_TEST("an instance answers with the op^ its definition wrote");
    check_text(&u,
               "var^ Vec = def^{\n"
               "  self^{ x := 0 },\n"
               "  op^.. := f^self^, other:string^ -> string^ { return^ other },\n"
               "}\n"
               "var^ v = Vec.new()\n"
               "var^ s : string^ = v .. \"a\"\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the operator's result is what the join answers");
    check_text(&u,
               "var^ Vec = def^{\n"
               "  self^{ x := 0 },\n"
               "  op^.. := f^self^, other:string^ -> string^ { return^ other },\n"
               "}\n"
               "var^ v = Vec.new()\n"
               "var^ n : number^ = v .. \"a\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 11.1 again: the right operand is the operator's argument, so what may
    // stand there is what its parameter admits.
    LHAT_TEST("and its parameter is what may stand on the right");
    check_text(&u,
               "var^ Vec = def^{\n"
               "  self^{ x := 0 },\n"
               "  op^.. := f^self^, other:string^ -> string^ { return^ other },\n"
               "}\n"
               "var^ v = Vec.new()\n"
               "var^ s = v .. 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    // 11.2改: two plain tables concatenate, built in -- but a name both
    // sides carry reaches no one value, and a table put beside itself
    // carries every name twice.
    LHAT_TEST("a name both sides of a '..' carry is refused");
    check_text(&u,
               "var^ t = { a := 1 }\n"
               "var^ v = t .. t\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_CONCAT_COLLIDES);
    unit_dispose(&u);

    // 11.4: the arithmetic operators ask the same question, so a written
    // op^ answers them too.
    LHAT_TEST("a definition answers arithmetic with its own op^");
    check_text(&u,
               "var^ Vec = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^+ := f^self^, o:number^ -> string^ { return^ \"x\" },\n"
               "}\n"
               "var^ v = Vec.new()\n"
               "var^ s : string^ = v + 1\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("with its parameter deciding the right side");
    check_text(&u,
               "var^ Vec = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^+ := f^self^, o:number^ -> number^ { return^ o },\n"
               "}\n"
               "var^ v = Vec.new()\n"
               "var^ n = v + \"a\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    // 14.8 keeps number^ answering all seven, so ordinary arithmetic is
    // exactly what it was.
    LHAT_TEST("and a number still answers all of them itself");
    check_text(&u,
               "var^ n : number^ = 1 + 2 - 3 * 4 / 5\n"
               "var^ m : number^ = 7 // 2 + 7 % 2 + 2 ** 3\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 11.8: bool^ carries none, and and^/or^/'!' are the language's own.
    LHAT_TEST("a bool^ answers no operator");
    check_text(&u, "var^ v = true^ + 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    // 11.8: an op^ is an f^ taking self^ and one argument. Nothing later
    // checks it -- the call site reads the signature and believes it, and the
    // machine hands over a receiver and one argument whatever was declared --
    // so a shape that is not an operator has to be refused here.
    LHAT_TEST("an op^ without a self^ is not an operator");
    check_text(&u,
               "var^ V = def^{ self^{},\n"
               "  op^.. := f^ o:string^ -> string^ { return^ o } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_BAD_OPERATOR);
    unit_dispose(&u);

    // 11.1: an operator is a function, so a p^ could carry side effects into
    // one.
    LHAT_TEST("nor is a p^");
    check_text(&u,
               "var^ V = def^{ self^{}, op^.. := p^self^, o:string^ { } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_BAD_OPERATOR);
    unit_dispose(&u);

    LHAT_TEST("nor anything that is not a subroutine");
    check_text(&u, "var^ V = def^{ self^{}, op^.. := 5 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_BAD_OPERATOR);
    unit_dispose(&u);

    // 14.4 leaves exactly one parameter beside the self^: the right operand.
    LHAT_TEST("and it takes the right operand, no more and no less");
    check_text(&u,
               "var^ V = def^{ self^{},\n"
               "  op^.. := f^self^ -> string^ { return^ \"z\" } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_BAD_OPERATOR);
    unit_dispose(&u);

    LHAT_TEST("two arguments is not an operator either");
    check_text(&u,
               "var^ V = def^{ self^{},\n"
               "  op^.. := f^self^, a:string^, b:string^ -> string^ { return^ a } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_BAD_OPERATOR);
    unit_dispose(&u);

    // 14.12: an overloaded operator is every arm at once, so each has to be
    // an operator on its own.
    LHAT_TEST("every arm of an overloaded operator is checked");
    check_text(&u,
               "var^ V = def^{ self^{},\n"
               "  op^.. := f^self^, o:string^ -> string^ { return^ o },\n"
               "  overload^ op^.. := f^self^ -> string^ { return^ \"z\" },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_BAD_OPERATOR);
    unit_dispose(&u);

    // 11.8改: '-' is the one operator with a unary spelling, so self^ alone is
    // its second shape. What tells the two apart is the count and nothing
    // else, which is why the arms above are still refused.
    LHAT_TEST("op^- written with self^ alone is the unary one");
    check_text(&u,
               "var^ V = def^{ self^{ n := 0 },\n"
               "  op^- := f^self^ -> number^ { return^ 0 - self^.n } }\n"
               "var^ r : number^ = -V.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a type may carry both under the one name");
    check_text(&u,
               "var^ V = def^{ self^{ n := 0 },\n"
               "  op^- := f^self^, o:number^ -> number^ { return^ o },\n"
               "  overload^ op^- := f^self^ -> number^ { return^ 0 },\n"
               "}\n"
               "var^ a : number^ = V.new() - 1\n"
               "var^ b : number^ = -V.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The counts do not stand in for each other: what a type wrote for one
    // spelling says nothing about the other.
    LHAT_TEST("a unary op^- does not answer a binary use");
    check_text(&u,
               "var^ V = def^{ self^{ n := 0 },\n"
               "  op^- := f^self^ -> number^ { return^ 0 } }\n"
               "var^ r = V.new() - 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    // 14.8 is what a unary '-' falls back on, so this reads as arithmetic
    // asked of something that is not a number^ -- the same report a type
    // carrying no '-' at all gets.
    LHAT_TEST("nor does a binary op^- answer a unary use");
    check_text(&u,
               "var^ V = def^{ self^{ n := 0 },\n"
               "  op^- := f^self^, o:number^ -> number^ { return^ o } }\n"
               "var^ r = -V.new()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_NUMBER);
    unit_dispose(&u);

    // Only '-' has one. The shape is refused for every other spelling exactly
    // as it was, since there is no unary '..' for it to mean.
    LHAT_TEST("no other operator has a unary spelling");
    check_text(&u,
               "var^ V = def^{ self^{},\n"
               "  op^+ := f^self^ -> number^ { return^ 0 } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_BAD_OPERATOR);
    unit_dispose(&u);

    // 11.1 and 15.7改 are unchanged by the new shape: what they refuse is
    // refused whichever count is written.
    LHAT_TEST("a unary op^- is still a function that cannot yield^");
    check_text(&u,
               "var^ V = def^{ self^{}, op^- := p^self^ { } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_BAD_OPERATOR);
    unit_dispose(&u);

    // 14.5: what a composition brings in keeps working, and 14.12's markers
    // reach an operator like any other member.
    LHAT_TEST("a composition carries an operator in");
    check_text(&u,
               "var^ B = def^{ self^{},\n"
               "  op^.. := f^self^, o:string^ -> string^ { return^ o } }\n"
               "var^ D = B .. def^{ self^{} }\n"
               "var^ s : string^ = D.new() .. \"x\"\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("override^ may widen what an operator takes");
    check_text(&u,
               "var^ B = def^{ self^{},\n"
               "  op^.. := f^self^, o:string^ -> string^ { return^ o } }\n"
               "var^ D = B .. def^{ self^{},\n"
               "  override^ op^.. := f^self^, o:string^|number^ -> string^ {\n"
               "    return^ \"d\" } }\n"
               "var^ s : string^ = D.new() .. 1\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and not narrow it");
    check_text(&u,
               "var^ B = def^{ self^{},\n"
               "  op^.. := f^self^, o:string^|number^ -> string^ {\n"
               "    return^ \"b\" } }\n"
               "var^ D = B .. def^{ self^{},\n"
               "  override^ op^.. := f^self^, o:string^ -> string^ {\n"
               "    return^ \"d\" } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_SUBSTITUTABLE);
    unit_dispose(&u);

    // 11.3 判定 is structural, so a '..' member put there by 14.14's computed
    // key answers exactly as a written op^ does.
    LHAT_TEST("a '..' reached by a computed key answers too");
    check_text(&u,
               "var^ t = { [\"..\"] := f^self^, o:string^ -> string^ {\n"
               "  return^ o } }\n"
               "var^ s : string^ = t .. \"x\"\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 11.8: an operator is a member, so 14.12's overload^ is what lets one
    // type answer several right-hand types.
    LHAT_TEST("overload^ gives one operator several right-hand types");
    check_text(&u,
               "var^ Vec = def^{\n"
               "  self^{},\n"
               "  op^.. := f^self^, o:string^ -> string^ { return^ o },\n"
               "  overload^ op^.. := f^self^, o:number^ -> number^ { return^ o },\n"
               "}\n"
               "var^ v = Vec.new()\n"
               "var^ s : string^ = v .. \"a\"\n"
               "var^ n : number^ = v .. 1\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // Which arm it is has to reach the answer's type. Until the arms were
    // walked here, an overloaded operator settled nothing and the caller fell
    // back -- on unknown^ for '..', which fits anywhere, and on 14.8's
    // number^ for arithmetic, which fits where the arm's own answer does not.
    LHAT_TEST("the arm that fits decides what an operator answers");
    check_text(&u,
               "var^ Vec = def^{\n"
               "  self^{},\n"
               "  op^+ := f^self^, o:number^ -> number^ { return^ o },\n"
               "  overload^ op^+ := f^self^, o:string^ -> string^ { return^ o },\n"
               "}\n"
               "var^ n : number^ = Vec.new() + \"a\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 14.12 leaves at most one arm fitting, so none fitting is the same
    // mistake a single arm makes when the right operand is not what it takes.
    LHAT_TEST("a right operand no arm takes is reported");
    check_text(&u,
               "var^ Vec = def^{\n"
               "  self^{},\n"
               "  op^+ := f^self^, o:number^ -> number^ { return^ o },\n"
               "  overload^ op^+ := f^self^, o:string^ -> string^ { return^ o },\n"
               "}\n"
               "var^ x = Vec.new() + true^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    // 11.5 の (5): every link of a chain is asked what a comparison written
    // on its own is asked. Until the links were walked here, a chain was
    // checked by inferring its operands and nothing else.
    LHAT_TEST("every link of a chain is judged");
    check_text(&u, "var^ a : bool^ = 1 < 2 < 3\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a pair that can never meet is caught in one");
    check_text(&u, "var^ a = 1 < 2 < \"x\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_INCOMPARABLE);
    unit_dispose(&u);

    LHAT_TEST("and so is a pair with no ordering between them");
    check_text(&u,
               "var^ V = def^{ self^{ n := 0 } }\n"
               "var^ a = V.new() < V.new() < V.new()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_ORDERED);
    unit_dispose(&u);

    // 13.11: an fits^ link takes a type, which is not an operand the next link
    // could compare against -- so it tests the value to its left and leaves
    // that value in place.
    LHAT_TEST("an fits^ link tests the operand to its left");
    check_text(&u, "var^ a : bool^ = 1 < 2 fits^ number^ < 3\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 11.9: the one comparison a type writes, and the four orderings
    // are read off it. Without one there was nothing to reach for at all --
    // a written 'a < b' passed here and faulted at run time.
    LHAT_TEST("an ordering is read off op^<=>");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^<=> := f^self^, o:V -> number^ { return^ self^.n },\n"
               "}\n"
               "var^ a : bool^ = V.new() < V.new()\n"
               "var^ b : bool^ = V.new() >= V.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and without one there is no ordering to read");
    check_text(&u,
               "var^ V = def^{ self^{ n := 0 } }\n"
               "var^ a = V.new() < V.new()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_ORDERED);
    unit_dispose(&u);

    // Equality is a different matter: 14.2 says what a table is the same as
    // whether or not it says how it orders.
    LHAT_TEST("but equality needs none");
    check_text(&u,
               "var^ V = def^{ self^{ n := 0 } }\n"
               "var^ a : bool^ = V.new() = V.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // '(a <=> b) < 0' is what an ordering reads, so the answer has to be a
    // thing zero can be on one side of.
    LHAT_TEST("op^<=> answers a number^");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^<=> := f^self^, o:V -> bool^ { return^ true^ },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_COMPARE_NOT_NUMBER);
    unit_dispose(&u);

    // 11.9改: a type may know what equals what and put its values in no
    // order at all. Then '=' and '≠' read the op^= it wrote, and the four
    // orderings have nothing to read -- which is exactly what it means.
    LHAT_TEST("an op^= answers equality on its own");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^= := f^self^, o:V -> bool^ { return^ true^ },\n"
               "}\n"
               "var^ a : bool^ = V.new() = V.new()\n"
               "var^ b : bool^ = V.new() \xE2\x89\xA0 V.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and leaves the orderings with nothing to read");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^= := f^self^, o:V -> bool^ { return^ true^ },\n"
               "}\n"
               "var^ a = V.new() < V.new()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_ORDERED);
    unit_dispose(&u);

    // '=' takes what an op^= answers as it stands, so it has to be a bool^ --
    // the same rule the '<=>' above is held to, one type over.
    LHAT_TEST("op^= answers a bool^");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^= := f^self^, o:V -> number^ { return^ 1 },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_EQUAL_NOT_BOOL);
    unit_dispose(&u);

    // Both of them is not a contradiction: one says what equals what, the
    // other how they order, and 11.9改 has '=' read the first.
    LHAT_TEST("and a type may write both");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^= := f^self^, o:V -> bool^ { return^ true^ },\n"
               "  op^<=> := f^self^, o:V -> number^ { return^ self^.n },\n"
               "}\n"
               "var^ a : bool^ = V.new() = V.new()\n"
               "var^ b : bool^ = V.new() < V.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 11.3改: written with the self^ last it relates a pair 14.12 would
    // otherwise call separate, so the disjointness rule gives way to it.
    LHAT_TEST("a self^-last one orders against a built-in");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^<=> := f^lhs:number^, self^ -> number^ { return^ lhs },\n"
               "}\n"
               "var^ a : bool^ = 3 < V.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 11.9: number^ and string^ each order their own, so a written '"a" < "b"'
    // resolves rather than faulting at run time.
    LHAT_TEST("the built-in types carry their own");
    check_text(&u,
               "var^ a : bool^ = \"a\" < \"b\"\n"
               "var^ b : number^ = 1 <=> 2\n"
               "var^ c : number^ = \"a\" <=> \"b\"\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and two that never meet are still fixed already");
    check_text(&u, "var^ a = 1 < \"b\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_INCOMPARABLE);
    unit_dispose(&u);

    // 11.3改: a self^ written last says the receiver is the RIGHT
    // operand, which is the only way to join an operation whose left operand
    // is a built-in -- number^ carries the arithmetic and takes nothing but
    // its own kind, and no program can add to what it carries.
    LHAT_TEST("a self^ written last answers from the right");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^+ := f^lhs:number^, self^ -> number^ { return^ lhs },\n"
               "}\n"
               "var^ a : number^ = 1 + V.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // and only from the right: written that way it describes one order, and
    // standing on the left is the other one.
    LHAT_TEST("and does not answer with its owner on the left");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^+ := f^lhs:number^, self^ -> number^ { return^ lhs },\n"
               "}\n"
               "var^ a = V.new() + 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    // 11.3's order is kept: the right side is asked only once the left has
    // no arm taking this operand. Both sides claiming the same pair is not a
    // question -- the left one answers and the other is never reached.
    LHAT_TEST("the left still answers first when it can");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^+ := f^self^, r:number^ -> string^ { return^ \"left\" },\n"
               "  overload^ op^+ := f^lhs:string^, self^ -> number^ "
               "{ return^ 0 },\n"
               "}\n"
               "var^ a : string^ = V.new() + 1\n"
               "var^ b : number^ = \"x\" + V.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.12 with 11.3改: and the two orders are apart even where the operand
    // is the same type on both sides, which is the ordinary case -- 'v * 2'
    // and '2 * v'. Which side the receiver stands on is a position too, and
    // the one 11.3's order settles before any other is looked at.
    LHAT_TEST("both orders of one operand type are not an overlap");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^* := f^self^, r:number^ -> number^ { return^ self^.n * r },\n"
               "  overload^ op^* := f^lhs:number^, self^ -> number^ "
               "{ return^ lhs * self^.n },\n"
               "}\n"
               "var^ a : number^ = V.new() * 2\n"
               "var^ b : number^ = 2 * V.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The receiver's side is what separates them, so two written for the same
    // side still collide however the operands are spelled.
    LHAT_TEST("but two written for one order still overlap");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^* := f^self^, r:number^ -> number^ { return^ r },\n"
               "  overload^ op^* := f^self^, o:number^ -> number^ "
               "{ return^ self^.n },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OVERLOAD_OVERLAPS);
    unit_dispose(&u);

    // 14.4: everywhere else the receiver is what stands before the dot, so a
    // member saying it is the last argument says nothing that can be acted on.
    LHAT_TEST("only an op^ writes its self^ last");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{ n := 0 },\n"
               "  m := f^x:number^, self^ -> number^ { return^ x },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_SELF_LAST_NOT_OPERATOR);
    unit_dispose(&u);

    LHAT_TEST("a declared one is refused the same way");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{},\n"
               "  abstract^ m : f^number^, self^ -> number^;,\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_SELF_LAST_NOT_OPERATOR);
    unit_dispose(&u);

    // Written at both ends it is the receiver twice over, leaving no ordinary
    // parameter at all -- which 11.8's shape rule is what reports.
    LHAT_TEST("and a self^ at both ends leaves no argument");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^+ := f^self^, self^ -> number^ { return^ 0 },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_BAD_OPERATOR);
    unit_dispose(&u);

    // 14.12 asks for a marker on two members of one name whether they came
    // from a base or from the same def^. Only the base had been looked at.
    LHAT_TEST("two members of one name in one def^ need the marker too");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{},\n"
               "  show := p^self^, x:string^ { },\n"
               "  overload^ show := p^self^, x:number^ { },\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and without one it is still the ordinary mistake");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{},\n"
               "  show := p^self^, x:string^ { },\n"
               "  show := p^self^, x:number^ { },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MEMBER_EXISTS);
    unit_dispose(&u);

    LHAT_TEST("with the overlap rule applying inside one def^ as well");
    check_text(&u,
               "var^ V = def^{\n"
               "  self^{},\n"
               "  show := p^self^, x:string^ { },\n"
               "  overload^ show := p^self^, x:string^ { },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OVERLOAD_OVERLAPS);
    unit_dispose(&u);

    // The operator is what this is about: a gap says nothing either way, so
    // '..' does not refuse it. 3.1③ reports the parameter itself, which is a
    // different sentence about a different place.
    LHAT_TEST("a gap in inference says nothing either way");
    check_text(&u, "var^ f = f^ x -> string^ { return^ x .. \"b\" }\n");
    CHECK_NOT_REPORTED(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    LHAT_TEST("and 13.7's any^ is every value at once");
    check_text(&u,
               "var^ x : any^ = \"a\"\n"
               "var^ v = x .. \"b\"\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a yield^ wants one to send out");
    check_text(&u,
               "var^ log = p^ m:string^ { var^ y = m }\n"
               "var^ g = p^ { yield^ log(\"x\") }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 01 の 5.4: a hole is an ordinary expression, and was not being checked
    // at all -- not for this and not for anything else.
    LHAT_TEST("an interpolation hole wants one");
    check_text(&u,
               "var^ log = p^ m:string^ { var^ y = m }\n"
               "var^ s = $\"v={log(\"x\")}\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and is checked like any other expression");
    check_text(&u, "var^ s = $\"v={nowhere}\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
    unit_dispose(&u);

    LHAT_TEST("while a sound one stays sound");
    check_text(&u,
               "var^ n = 1\n"
               "var^ s = $\"v={n} and {n + 1}\"\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

int main(void)
{
    test_no_value();
    return lhat_test_report("test_check_novalue");
}
