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

// 01 の 8 章: a scope specifier says which scope to start looking in. What
// counts as one is what a '{' opens when names become visible in it, which
// is exactly where this pushes a Scope -- so the compiler counting the same
// braces reaches the same binding.
static void test_scope_specifiers(void)
{
    Unit u;

    LHAT_TEST("'$^' looks past the scope it is written in");
    check_text(&u,
               "var^ x = 1\n"
               "do^{ var^ x = \"s\"\n"
               "  var^ n : number^ = $^x\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and does not see the one it was written beside");
    check_text(&u,
               "var^ x = 1\n"
               "do^{ var^ x = \"s\"\n"
               "  var^ n : string^ = $^x\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("'$^^' steps out of one more");
    check_text(&u,
               "var^ x = 1\n"
               "do^{ var^ x = \"a\"\n"
               "  do^{ var^ x = \"b\"\n"
               "    var^ n : number^ = $^^x\n"
               "  }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A specifier says where to begin, not where to stop, so a scope that
    // holds nothing of that name hands the search on outwards.
    LHAT_TEST("the search goes on outwards from there");
    check_text(&u,
               "var^ x = 1\n"
               "do^{ do^{ var^ y = 2\n"
               "  var^ n : number^ = $^x\n"
               "}}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("'$' names the unit's own top level");
    check_text(&u,
               "var^ x = 1\n"
               "do^{ var^ x = \"a\"\n"
               "  do^{ var^ x = \"b\"\n"
               "    var^ n : number^ = $x\n"
               "  }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // Repeating the sigil counts inwards from there, which is the other of
    // the two ways -- '$^' counts outwards from here.
    LHAT_TEST("'$$' is the scope one inside the unit");
    check_text(&u,
               "var^ x = 1\n"
               "do^{ var^ x = \"a\"\n"
               "  do^{ var^ x = 2\n"
               "    var^ n : string^ = $$x\n"
               "  }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The same place named both ways, so the two numberings meet.
    LHAT_TEST("and reaches what '$^' reaches from the other side");
    check_text(&u,
               "var^ x = 1\n"
               "do^{ var^ x = \"a\"\n"
               "  do^{ var^ x = 2\n"
               "    var^ n : string^ = $^x\n"
               "  }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("naming a scope further in than this one is refused");
    check_text(&u, "var^ x = 1\nvar^ n = $$x\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_SCOPE_TOO_FAR);
    unit_dispose(&u);

    // A subroutine's body is one scope, the way a block is -- its parameters
    // are in it rather than around it.
    LHAT_TEST("a body is one scope, parameters and all");
    check_text(&u,
               "var^ x = 1\n"
               "var^ f = f^ x:string^ -> number^ { return^ $^x }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.4 binds class^ into the scope a def^'s '{' opens, so that scope is
    // one for a specifier to count. 01 の 2.3: class^ is its own name, so
    // the outer `class` never collides and the specifier spells the hat.
    LHAT_TEST("a def^ body is one scope too");
    check_text(&u,
               "var^ class = 1\n"
               "var^ D = def^{ self^{},\n"
               "  m := f^self^ -> t^{} { return^ $^class^ }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("counting more scopes than are open is refused");
    check_text(&u, "do^{ var^ n = $^^^^x }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_SCOPE_TOO_FAR);
    unit_dispose(&u);

    // 8.7: a var^ makes a name where it is written, so there is no other
    // scope for a specifier to name. ':=' is what reaches an existing one.
    LHAT_TEST("a var^ takes no specifier");
    check_text(&u, "var^ x = 1\ndo^{ var^ $^x = 2 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_SCOPE_ON_DEFINE);
    unit_dispose(&u);

    LHAT_TEST("but ':=' writes through one");
    check_text(&u,
               "var^ x = 1\n"
               "do^{ var^ x = 2\n"
               "  $^x := 9\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // Reading and writing resolve the same way, so a write cannot land
    // somewhere a read of the same words would not.
    LHAT_TEST("and writes where a read of the same words looks");
    check_text(&u,
               "var^ x = 1\n"
               "do^{ var^ x = \"s\"\n"
               "  $^x := \"no\"\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);
}

// 02 の 14.17: every value carries tostring, and a number^ carries a second
// signature taking a format. 14.12 makes the two an intersection.
static void test_tostring(void)
{
    Unit u;

    LHAT_TEST("every value answers with a string^");
    check_text(&u,
               "var^ a : string^ = nil^.tostring()\n"
               "var^ b : string^ = true^.tostring()\n"
               "var^ c : string^ = (1).tostring()\n"
               "var^ d : string^ = \"x\".tostring()\n"
               "var^ e : string^ = { n := 1 }.tostring^()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a number^ also takes a format");
    check_text(&u, "var^ a : string^ = (255).tostring(\"%x\")\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The second signature is the number^'s alone, so this is neither of the
    // two ways of writing a string^ down.
    LHAT_TEST("but nothing else does");
    check_text(&u, "var^ a : string^ = \"x\".tostring(\"%x\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    LHAT_TEST("the format has to be a string^");
    check_text(&u, "var^ a : string^ = (1).tostring(2)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 11.1's reason for keeping an operator pure holds here too: writing a
    // value down changes nothing, so 15.1 lets an f^ reach it.
    LHAT_TEST("an f^ may call it");
    check_text(&u,
               "var^ show = f^ n:number^ -> string^ { return^ n.tostring() }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 16.3's rule for iterate, which 14.17 follows.
    LHAT_TEST("a written tostring is what the type says");
    check_text(&u,
               "var^ t = { tostring := f^self^ -> number^ { return^ 1 } }\n"
               "var^ n : number^ = t.tostring()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.17改: on a table the writer wrote, the bare name is the writer's
    // and holds nothing until something is written under it. The checker has
    // to say the same as the machine, or it would pass what the machine
    // answers nil^ for.
    LHAT_TEST("a plain table holds no bare tostring");
    check_text(&u, "var^ t = { n := 1 }\nvar^ s : string^ = t.tostring()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    LHAT_TEST("and answers the hat spelling instead");
    check_text(&u, "var^ t = { n := 1 }\nvar^ s : string^ = t.tostring^()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("an instance answers either spelling");
    check_text(&u,
               "var^ P = def^{ self^{ x := 1 } }\n"
               "var^ a : string^ = P.new().tostring()\n"
               "var^ b : string^ = P.new().tostring^()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and so does the definition itself");
    check_text(&u,
               "var^ P = def^{ self^{ x := 1 } }\n"
               "var^ s : string^ = P.tostring()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 16.3改: the same split, for the name in^ asks for.
    LHAT_TEST("a plain table holds no bare iterate either");
    check_text(&u, "var^ t = { 1, 2 }\nvar^ w = t.iterate()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);
}

// 02 の 14.17改2: the same two signatures the other way round, on the one
// value a number^ can be read out of.
static void test_tonumber(void)
{
    Unit u;

    // The nil^ arm is the type saying what 14.17改2 says: a string^ holding
    // no number^ is data, so the answer is a number^ or nothing.
    LHAT_TEST("a string^ answers a number^ or nil^");
    check_text(&u,
               "var^ a : number^|nil^ = \"1\".tonumber()\n"
               "var^ b : number^|nil^ = \"ff\".tonumber(\"%x\")\n"
               "var^ c : number^|nil^ = \"1\".tonumber^()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the arm has to be dealt with");
    check_text(&u, "var^ a : number^ = \"1\".tonumber()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 11.7's '??' is one of the two ways of dealing with it; 04 の 11.4's
    // narrowing is the other.
    LHAT_TEST("'??' is one way of dealing with it");
    check_text(&u, "var^ a : number^ = \"1\".tonumber() ?? 0\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("the format has to be a string^");
    check_text(&u, "var^ a : number^|nil^ = \"1\".tonumber(2)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 14.12: a call fitting no arm of the intersection is a mismatch, which
    // is where both of these land -- there is no arm taking two, and none
    // taking a number^.
    LHAT_TEST("and a third argument is neither signature");
    check_text(&u,
               "var^ a : number^|nil^ = \"1\".tonumber(\"%d\", \"%d\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // Reading a value changes nothing, the same as 14.17's tostring.
    LHAT_TEST("an f^ may call it");
    check_text(&u,
               "var^ read = f^ s:string^ -> number^ {\n"
               "  return^ s.tonumber() ?? 0 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // Every value carries tostring; this one only a string^ carries, and the
    // checker has to say so where the machine does.
    LHAT_TEST("nothing else carries it");
    check_text(&u, "var^ a : number^|nil^ = (1).tonumber()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    check_text(&u, "var^ a : number^|nil^ = nil^.tonumber()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    LHAT_TEST("and on a table it is a name like any other");
    check_text(&u,
               "var^ t = { n := 1 }\nvar^ a : number^|nil^ = t.tonumber^()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);
}

// 02 の 13.7: the variadic collector. '...' inside the body names it, typed
// as 14.10's unbounded tail -- a table whose sequence half is one element
// type repeated, nothing fixed.
static void test_variadic(void)
{
    Unit u;

    LHAT_TEST("'...' inside the body is a table of the element type");
    check_text(&u,
               "var^ f = f^ ...:number^ -> number^ {\n"
               "  var^ t : t^{ ...:number^ } = ...\n"
               "  return^ 0\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.8 has no tuples, so a single name over an in^ walk takes the whole
    // pair (14 章), the same as walking any other table -- 13.7 does not
    // special-case this.
    LHAT_TEST("for^ i, x in^ ... gives x the element type");
    check_text(&u,
               "var^ f = f^ ...:number^ -> number^ {\n"
               "  var^ total = 0\n"
               "  for^ i, x in^ ... {\n"
               "    var^ n : number^ = x\n"
               "    var^ p : number^ = i\n"
               "    total := total + x\n"
               "  }\n"
               "  return^ total\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 04 の 11.3: a dynamic key may be out of range, and this one really can
    // be -- the count is unbounded, unlike a table with listed members.
    LHAT_TEST("a dynamic index into ... may be nil^");
    check_text(&u,
               "var^ f = f^ ...:number^ -> number^ {\n"
               "  var^ i = 1\n"
               "  var^ v : number^|nil^ = ...[i]\n"
               "  return^ 0\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.7: at least the fixed count, any number beyond it.
    LHAT_TEST("fewer than the fixed count is refused");
    check_text(&u,
               "var^ f = f^ a:number^, b:number^, ...:number^ -> number^ {\n"
               "  return^ a + b\n"
               "}\n"
               "f(1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    LHAT_TEST("exactly the fixed count, with none variadic, is fine");
    check_text(&u,
               "var^ f = f^ a:number^, b:number^, ...:number^ -> number^ {\n"
               "  return^ a + b\n"
               "}\n"
               "f(1, 2)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a mismatched variadic argument is still caught");
    check_text(&u,
               "var^ f = f^ ...:number^ -> number^ { return^ 0 }\n"
               "f(1, \"x\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 13.7: 'expr...' spreads a collected table back into a variadic tail.
    LHAT_TEST("'...' forwards into another variadic call");
    check_text(&u,
               "var^ inner = f^ ...:number^ -> number^ { return^ 0 }\n"
               "var^ outer = f^ ...:number^ -> number^ {\n"
               "  return^ inner(...)\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("forwarding into a non-variadic callee is refused");
    check_text(&u,
               "var^ inner = f^ -> number^ { return^ 0 }\n"
               "var^ outer = f^ ...:number^ -> number^ {\n"
               "  return^ inner(...)\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_VARIADIC);
    unit_dispose(&u);

    LHAT_TEST("fixed arguments may lead a forwarded spread");
    check_text(&u,
               "var^ inner = f^ base:number^, ...:number^ -> number^ {\n"
               "  return^ base\n"
               "}\n"
               "var^ outer = f^ ...:number^ -> number^ {\n"
               "  return^ inner(100, ...)\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

// 17 章. Nothing here is checked by machinery of its own -- 17.9 lowers a
// pattern to a condition, so what runs is 13.11's narrowing.
static void test_patterns(void)
{
    Unit u;

    // 16.2: the focus with no name written is called it^, and it is bound.
    LHAT_TEST("the subject is in scope as it^");
    check_text(&u,
               "var^ f = f^ -> number^ { return^ 0 }\n"
               "for^ f() { when^ 0: var^ n : number^ = it^ other^: }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a named subject is in scope under its name");
    check_text(&u,
               "var^ f = f^ -> number^ { return^ 0 }\n"
               "for^ var^ r := f() { when^ 0: var^ n : number^ = r other^: }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 16.2 applies to every form of for^, not only to a match.
    LHAT_TEST("it^ is bound in a loop too");
    check_text(&u, "for^ 1 to^ 3 { var^ n : number^ = it^ }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 17.9: narrowing is what makes a declared field visible, exactly as in
    // an if-chain.
    LHAT_TEST("a type pattern narrows the subject");
    check_text(&u,
               "errordef^ ParseError { Syntax { line : number^ }, Eof }\n"
               "var^ parse = f^ -> number^|ParseError { return^ 0 }\n"
               "for^ var^ r := parse() {\n"
               "    when^ isa^ ParseError.Syntax:\n"
               "        var^ n : number^ = r.line\n"
               "    other^:\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("the field is not visible in another clause");
    check_text(&u,
               "errordef^ ParseError { Syntax { line : number^ }, Eof }\n"
               "var^ parse = f^ -> number^|ParseError { return^ 0 }\n"
               "for^ var^ r := parse() {\n"
               "    when^ isa^ ParseError.Eof:\n"
               "        var^ n = r.line\n"
               "    other^:\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // 04 の 7 章: exhaustiveness needed no mechanism of its own, and it keeps
    // working through the sugar.
    LHAT_TEST("an exhausted union leaves the success type");
    check_text(&u,
               "errordef^ IOError { NotFound, Denied }\n"
               "var^ open = f^ -> number^|IOError { return^ 0 }\n"
               "var^ use = f^ -> number^ {\n"
               "    for^ var^ r := open() {\n"
               "        when^ isa^ IOError.NotFound: return^ 0\n"
               "        when^ isa^ IOError.Denied: return^ 0\n"
               "        other^: return^ r\n"
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
               "    for^ var^ r := open() {\n"
               "        when^ isa^ IOError.NotFound: return^ 0\n"
               "        other^: return^ r\n"
               "    }\n"
               "    return^ 0\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 17.3: a value pattern is a comparison, so the subject and the value
    // have to be comparable at all.
    LHAT_TEST("a pattern the subject can never match is reported");
    check_text(&u, "for^ \"text\" { when^ 1 to^ 3: other^: }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_INCOMPARABLE);
    unit_dispose(&u);

    LHAT_TEST("a value pattern of the wrong type is reported");
    check_text(&u, "for^ 1 { when^ \"text\": other^: }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_INCOMPARABLE);
    unit_dispose(&u);

    // 17.2: the expression form has a type, taken from its clauses.
    LHAT_TEST("the expression form yields the union of its clauses");
    check_text(&u,
               "var^ n = 1\n"
               "var^ s : string^ = for^ n: when^ 0: \"zero\" other^: \"more\" ;\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a clause of the wrong type is reported");
    check_text(&u,
               "var^ n = 1\n"
               "var^ s : string^ = for^ n: when^ 0: \"zero\" other^: 1 ;\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);
}

int main(void)
{
    test_scope_specifiers();
    test_tostring();
    test_tonumber();
    test_variadic();
    test_patterns();
    return lhat_test_report("test_check_forms");
}
