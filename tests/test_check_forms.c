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

    // 14.20: the comparison '=' makes, with the error term written down.
    // Only a number^ carries it -- it is the one value arithmetic error
    // accumulates in -- and the bare word alone (14.18改).
    LHAT_TEST("a number^ answers eq with both of its arguments");
    check_text(&u, "var^ a : bool^ = (1).eq(1.0, 0.1)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and wants exactly the two");
    check_text(&u, "var^ a : bool^ = (1).eq(1.0)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    LHAT_TEST("both of them number^");
    check_text(&u, "var^ a : bool^ = (1).eq(1.0, \"x\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and nothing else carries it");
    check_text(&u, "var^ a : bool^ = \"x\".eq(1.0, 0.1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // 14.18改: the hat is there to keep a built-in off a name a writer may
    // mean for something else, and nothing can be written on a number^.
    LHAT_TEST("nor is there a hat spelling of it");
    check_text(&u, "var^ a : bool^ = (1).eq^(1.0, 0.1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // 14.21: the three roundings, carried by a number^ for the same reasons.
    LHAT_TEST("a number^ answers the three roundings");
    check_text(&u,
               "var^ a : number^ = (2.7).floor()\n"
               "var^ b : number^ = (2.7).ceil()\n"
               "var^ c : number^ = (2.7).round()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and none of them takes anything");
    check_text(&u, "var^ a : number^ = (2.7).floor(1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    LHAT_TEST("nothing else carries them");
    check_text(&u, "var^ a : number^ = \"x\".round()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    LHAT_TEST("and they have no hat spelling either");
    check_text(&u, "var^ a : number^ = (2.7).floor^()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
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

// 02 の 14.18: how much of a value there is, answered as a number^ with no
// call written. What the checker owes here is the type and which value
// carries which spelling -- the counts themselves are the machine's, and
// test_vm_data pins those.
static void test_counting(void)
{
    Unit u;

    LHAT_TEST("a table answers a number^ with no call written");
    check_text(&u,
               "var^ t = { 1, 2 }\n"
               "var^ a : number^ = t.length^\n"
               "var^ b : number^ = t.len^\n"
               "var^ c : number^ = t.count^\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.18 with 14.17改: nothing can be written on a string^, so the bare
    // word takes nothing from anyone.
    LHAT_TEST("and a string^ answers its length and its bytes");
    check_text(&u,
               "var^ d : number^ = \"x\".length\n"
               "var^ e : number^ = \"x\".len\n"
               "var^ f : number^ = \"x\".size\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.18改: and the hat spelling is not a second way of writing it. What
    // the hat does is keep a built-in off a name the writer may mean for
    // something else, which on a string^ is nothing.
    LHAT_TEST("a string^ has no hat spelling of them");
    check_text(&u, "var^ a : number^ = \"x\".length^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // 14.18: each spelling is a reading of one kind of value. Bytes are a
    // reading of a string^, and elements of a table.
    LHAT_TEST("a table has no size^");
    check_text(&u, "var^ t = { 1 }\nvar^ n : number^ = t.size^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    LHAT_TEST("and a string^ no count^");
    check_text(&u, "var^ n : number^ = \"x\".count^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // 14.18: on a table the hat is not optional, even where 14.17改 would let
    // the bare word through. 14 章 reserves new, tostring and dispose on a
    // def^ and these are not among them.
    LHAT_TEST("the bare word is the writer's on a table");
    check_text(&u, "var^ t = { 1 }\nvar^ n : number^ = t.length\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    LHAT_TEST("on an instance too, where a written length would collide");
    check_text(&u,
               "var^ P = def^{ self^{ x := 1 } }\n"
               "var^ n : number^ = P.new().count\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // 14.17改's order again: what is written under the name is what answers.
    LHAT_TEST("a written length^ is what the type says");
    check_text(&u,
               "var^ t = { 1, length^ := \"nine\" }\n"
               "var^ s : string^ = t.length^\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

// 02 の 14.19: a run of a string^'s characters, under three names. What the
// checker owes is the two shapes and the answer -- a plain string^, since a
// range that does not stand is empty rather than absent.
static void test_substring(void)
{
    Unit u;

    LHAT_TEST("either form answers a string^, under any of the three names");
    check_text(&u,
               "var^ a : string^ = \"xyz\".substring(2)\n"
               "var^ b : string^ = \"xyz\".substr(2, 3)\n"
               "var^ c : string^ = \"xyz\".sub(-1)\n"
               "var^ d : string^ = \"xyz\".sub(1, -1)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.18改: three names and no hat spelling of any of them.
    LHAT_TEST("and no fourth spelling with a hat");
    check_text(&u, "var^ a : string^ = \"xyz\".sub^(1, -1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // 14.12 tells the two apart by how many arguments arrive, so neither a
    // third nor none at all is one of them.
    LHAT_TEST("no ordinal at all is neither form");
    check_text(&u, "var^ a : string^ = \"xyz\".substr()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and three is neither either");
    check_text(&u, "var^ a : string^ = \"xyz\".substr(1, 2, 3)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // An ordinal is a number^. Handing over something else is the writer's
    // mistake, not a range that came out empty.
    LHAT_TEST("an ordinal has to be a number^");
    check_text(&u, "var^ a : string^ = \"xyz\".substr(\"x\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 14.19 answers a string^ and nothing else -- there is no nil^ arm, so
    // what comes back is usable without narrowing.
    LHAT_TEST("the answer needs no narrowing");
    check_text(&u, "var^ n : number^ = \"xyz\".substr(2).length\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and nothing but a string^ carries it");
    check_text(&u, "var^ a = { 1 }.substr(1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // 14.19改: one character, which is that run with both ends at the same
    // ordinal -- so it answers a string^ too, there being no character type
    // for it to answer instead.
    LHAT_TEST("at takes one ordinal and answers a string^");
    check_text(&u, "var^ a : string^ = \"xyz\".at(2)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // One signature rather than 14.19's intersection, so a call that does not
    // fit is short or long rather than "no arm of it".
    LHAT_TEST("and one is the whole of its shape");
    check_text(&u,
               "var^ a : string^ = \"xyz\".at()\n"
               "var^ b : string^ = \"xyz\".at(1, 2)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    LHAT_CHECK_EQ_INT(u.checked.diagnostic_count, 2);
    unit_dispose(&u);

    LHAT_TEST("an ordinal of at has to be a number^ as well");
    check_text(&u, "var^ a : string^ = \"xyz\".at(\"x\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 14.19改 takes the name only where 14.19's member is. `at` is a word a
    // writer reaches for -- sample/24.lh's reading position is one -- and on
    // a table it stays theirs.
    LHAT_TEST("at on a table is the writer's own");
    check_text(&u,
               "var^ P = def^{ self^{ at := 1 } }\n"
               "var^ n : number^ = P.new().at\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and absent on a table where nothing was written");
    check_text(&u, "var^ a = { 1 }.at(1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);
}

// 02 の 16.3改2: the two projections of a table's walk. What the checker owes
// is the type each yields -- the halves of the pair 16.3 hands over -- and
// that a loop written with two names over one of them is refused.
static void test_projections(void)
{
    Unit u;

    LHAT_TEST("keys^ yields the key half and values^ the value half");
    check_text(&u,
               "var^ t = { 1, 2 }\n"
               "for^ k in^ t.keys^() { var^ n : number^ = k }\n"
               "for^ v in^ t.values^() { var^ m : number^ = v }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A keyed member makes the key half a union, which is what the walk
    // hands over and so what the focus holds.
    LHAT_TEST("a keyed member widens the key half");
    check_text(&u,
               "var^ t = { 1, a := 2 }\n"
               "for^ k in^ t.keys^() { var^ n : number^ = k }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 13.8改: a projection yields one value, so two names have nothing to
    // take apart. Caught here rather than left to the machine.
    LHAT_TEST("two names over a projection are refused");
    check_text(&u,
               "var^ t = { 1, 2 }\n"
               "for^ k, v in^ t.keys^() { }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TUPLE_ARITY);
    unit_dispose(&u);

    // The same refusal reaches any walk that hands over one value, which is
    // the hole the projections made visible.
    LHAT_TEST("and over any coroutine yielding a single value");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "for^ a, b in^ gen() { }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TUPLE_ARITY);
    unit_dispose(&u);

    // 13.10: a walk yielding a table is still taken apart by position, so
    // the refusal above must not reach it.
    LHAT_TEST("but a walk yielding a table still comes apart");
    check_text(&u,
               "var^ gen = p^ { yield^ { 1, \"x\" } }\n"
               "for^ a, b in^ gen() { var^ n : number^ = a }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 16.3改2 with 14.18: the hat is not optional on a table.
    LHAT_TEST("the bare word is the writer's");
    check_text(&u, "var^ t = { 1 }\nfor^ k in^ t.keys { }\n");
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
               "var^ b : number^|nil^ = \"ff\".tonumber(\"%x\")\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.18改: what only a string^ carries has the bare spelling and no
    // other. Nothing can be written on one for a hat to be keeping this off.
    LHAT_TEST("and carries no hat spelling of it");
    check_text(&u, "var^ c : number^|nil^ = \"1\".tonumber^()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
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

    // 16.3: two names over an in^ walk take the index and the element, the
    // same as walking any other table -- 13.7 does not special-case this.
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

    // What leads a spread owes the fixed arguments and no more. Beyond them
    // the values written join the tail the spread continues, which is the
    // only way to write `print("a", ...)` -- print declares no fixed ones.
    LHAT_TEST("values written into the tail may lead a spread");
    check_text(&u,
               "var^ inner = f^ ...:number^ -> number^ { return^ 0 }\n"
               "var^ outer = f^ ...:number^ -> number^ {\n"
               "  return^ inner(10, 20, ...)\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and they are matched against the variadic type");
    check_text(&u,
               "var^ inner = f^ ...:number^ -> number^ { return^ 0 }\n"
               "var^ outer = f^ ...:number^ -> number^ {\n"
               "  return^ inner(\"a\", ...)\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("a spread does not stand in for a missing fixed argument");
    check_text(&u,
               "var^ inner = f^ base:number^, tag:string^, ...:number^ "
               "-> number^ { return^ base }\n"
               "var^ outer = f^ ...:number^ -> number^ {\n"
               "  return^ inner(1, ...)\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    // 13.8改: the same spelling over a tuple. Unlike a table, whose tail is
    // one element type, each position is matched against the variadic type on
    // its own -- so a mixed tuple is refused at the position that does not fit
    // rather than wholesale.
    LHAT_TEST("a tuple spreads into a variadic tail");
    check_text(&u,
               "var^ inner = f^ ...:number^ -> number^ { return^ 0 }\n"
               "var^ pair = f^ -> (number^, number^) { return^ 1, 2 }\n"
               "var^ outer = f^ -> number^ {\n"
               "  return^ inner(pair()...)\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a position that does not fit the tail is refused");
    check_text(&u,
               "var^ inner = f^ ...:number^ -> number^ { return^ 0 }\n"
               "var^ pair = f^ -> (number^, string^) { return^ 1, \"a\" }\n"
               "var^ outer = f^ -> number^ {\n"
               "  return^ inner(pair()...)\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 13.8改 keeps the two readings apart: a tuple reaches a variadic tail
    // only where '...' is written, never because the callee happens to take
    // one. Without the spelling it is a value in an argument position, which
    // is where a tuple cannot stand.
    LHAT_TEST("a tuple does not expand on its own");
    check_text(&u,
               "var^ inner = f^ ...:number^ -> number^ { return^ 0 }\n"
               "var^ pair = f^ -> (number^, number^) { return^ 1, 2 }\n"
               "var^ outer = f^ -> number^ {\n"
               "  return^ inner(pair())\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TUPLE_MISPLACED);
    unit_dispose(&u);
}

// 16.3: the forms of for^ that answer with a value rather than iterate.
static void test_for_expressions(void)
{
    Unit u;

    // 13.8改 keeps a tuple out of an argument list, so the focus is where the
    // names for its positions go, and do^: answers with what they build.
    LHAT_TEST("do^: takes the type of its body");
    check_text(&u,
               "var^ f = f^ -> (number^, number^) { return^ 0, 1 }\n"
               "var^ n : number^ = for^ let^ x, y = f() do^: x + y;\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a body of the wrong type is reported");
    check_text(&u,
               "var^ f = f^ -> (number^, number^) { return^ 0, 1 }\n"
               "var^ s : string^ = for^ let^ x, y = f() do^: x + y;\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // Each focus is scoped to the one after it, so the outer names are there
    // for the inner clause as well as for the body.
    LHAT_TEST("a run of for^ sees the focuses before it");
    check_text(&u,
               "var^ f = f^ -> (number^, number^) { return^ 0, 1 }\n"
               "var^ g = f^ a:number^ -> number^ { return^ a }\n"
               "var^ n : number^ = for^ let^ x, y = f()\n"
               "                   for^ let^ z = g(x)\n"
               "                   do^: x + y + z;\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 16.7: the focus does not leave, whichever form was written.
    LHAT_TEST("but the focus does not leave the construct");
    check_text(&u,
               "var^ f = f^ -> (number^, number^) { return^ 0, 1 }\n"
               "var^ n : number^ = for^ let^ x, y = f() do^: x + y;\n"
               "var^ m : number^ = x\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
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
    test_counting();
    test_substring();
    test_projections();
    test_tonumber();
    test_variadic();
    test_for_expressions();
    test_patterns();
    return lhat_test_report("test_check_forms");
}
