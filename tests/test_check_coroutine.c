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

// 02 の 15.5 and 15.8: what a call of a yieldable procedure answers, and the
// mistake the answer makes catchable.
static void test_coroutines(void)
{
    Unit u;

    LHAT_TEST("a call of a yieldable procedure answers a coroutine");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "var^ c = gen()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.8: 15.5 makes such a call run no part of the body, so the statement
    // has no effect at all. C#'s IEnumerator allows this silently.
    LHAT_TEST("dropping the coroutine is reported");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "gen()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_COROUTINE_DROPPED);
    unit_dispose(&u);

    LHAT_TEST("delegating to it is not");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "var^ outer = p^ { await^ gen() }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("binding it is not either");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "var^ c = gen()\n"
               "var^ d = c\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A procedure that does not yield is unaffected: its effect has already
    // happened when the call returns.
    LHAT_TEST("an ordinary call still stands alone");
    check_text(&u,
               "var^ go = p^ { var^ x = 1 }\n"
               "go()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 05 の 8.5: a coroutine carries these without anything being imported.
    LHAT_TEST("a coroutine carries start, resume, dispose and iterate");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "var^ c = gen()\n"
               "var^ v = c.start()\n"
               "var^ w = c.resume()\n"
               "c.dispose()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.6改: and the two questions, which answer bool^ whatever the three
    // types of 13.9 turn out to be.
    LHAT_TEST("a coroutine carries done and started");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "var^ c = gen()\n"
               "var^ d : bool^ = c.done()\n"
               "var^ s : bool^ = c.started()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The case that made them necessary: Y is nil^ and the body ends without
    // a value, so the union a resume answers is nil^ alone and carries no
    // sign of which of the two happened.
    LHAT_TEST("they answer even where the resume union says nothing");
    check_text(&u,
               "var^ gen = p^ { yield^ }\n"
               "var^ c = gen()\n"
               "var^ v : nil^ = c.start()\n"
               "var^ d : bool^ = c.done()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("neither takes an argument");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "var^ c = gen()\n"
               "c.done(1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    LHAT_TEST("and nothing else");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "var^ c = gen()\n"
               "var^ v = c.nowhere\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // 05 の 8.6: L^ is there without being imported, and is not a name -- so
    // 8.1's "nothing is visible" is untouched.
    LHAT_TEST("L^ carries the collector and the registry");
    check_text(&u,
               "L^.collectgarbage()\n"
               "var^ m = L^.modules\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and nothing else");
    check_text(&u, "var^ x = L^.nowhere\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    LHAT_TEST("and collectgarbage takes no argument");
    check_text(&u, "L^.collectgarbage(1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    // 05 の 8.6: the registry is the machine's, and 5.3's registration
    // is what the compiler emits rather than a line anyone writes. Adding a
    // member is refused the same way writing over one is -- 8.8's walk is
    // exactly what would have made the segments on the way.
    LHAT_TEST("L^ is not a root a path may be grown from");
    check_text(&u, "var^ L^.modules.ns1.mod1 = { greet := 1 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TABLE_IS_SEALED);
    unit_dispose(&u);

    LHAT_TEST("and writing over what it holds is the same refusal");
    check_text(&u, "L^.collectgarbage := p^ { }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TABLE_IS_SEALED);
    unit_dispose(&u);

    // 15.1改 answers the f^ side of the same question; this is the other one.
    LHAT_TEST("a p^ reaches it no more than the top level does");
    check_text(&u, "var^ f = p^ { L^.collectgarbage := p^ { } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TABLE_IS_SEALED);
    unit_dispose(&u);

    LHAT_TEST("but reading it is untouched");
    check_text(&u,
               "L^.collectgarbage()\n"
               "var^ m = L^.modules\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // Only that one spelling is a place; the rest name nothing.
    LHAT_TEST("but another hat identifier is not a root");
    check_text(&u, "var^ true^.x = 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
    unit_dispose(&u);

    // 8.8: var^ introduces a member, the way ':=' reassigns one. The tables on
    // the way are made where the path does not reach one yet.
    LHAT_TEST("var^ introduces a member along a path");
    check_text(&u,
               "var^ a.b.c = 1\n"
               "var^ n : number^ = a.b.c\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and two paths through one table meet");
    check_text(&u,
               "var^ a.b.c = 1\n"
               "var^ a.b.d = 2\n"
               "var^ a.z = 3\n"
               "var^ n : number^ = a.b.c + a.b.d + a.z\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and an existing table gains the member");
    check_text(&u,
               "var^ t = { p := 1 }\n"
               "var^ t.q = 2\n"
               "var^ n : number^ = t.p + t.q\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The last segment is the name being introduced, so 8.7 holds for it.
    LHAT_TEST("but writing the last segment twice is a redefinition");
    check_text(&u,
               "var^ a.b = 1\n"
               "var^ a.b = 2\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_REDEFINED);
    unit_dispose(&u);

    LHAT_TEST("and a segment on the way has to be a table");
    check_text(&u,
               "var^ a = 1\n"
               "var^ a.b = 2\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PATH_NOT_TABLE);
    unit_dispose(&u);

    // 14 章 fixes what an instance of a def^ carries, which is the one kind
    // of table this cannot add to.
    LHAT_TEST("and a def^ instance takes no new member");
    check_text(&u,
               "var^ P = def^{ self^{ x := 0 } }\n"
               "var^ p = P.new()\n"
               "var^ p.y = 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PATH_IS_DEFINITION);
    unit_dispose(&u);

    LHAT_TEST("nor does the definition itself");
    check_text(&u,
               "var^ P = def^{ self^{ x := 0 } }\n"
               "var^ P.y = 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PATH_IS_DEFINITION);
    unit_dispose(&u);

    // The root says where the member goes and nothing about itself, so an
    // enclosing binding is reached rather than shadowed.
    LHAT_TEST("and the root is not shadowed");
    check_text(&u,
               "var^ outer = { }\n"
               "var^ add = p^ { var^ outer.k = 7 }\n"
               "add()\n"
               "var^ n : number^ = outer.k\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and an annotation on the last segment is checked");
    check_text(&u,
               "var^ a.b : string^ = 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 15.11: a body that must be a coroutine but has nothing to suspend for.
    // _yield^ says the same thing about the three types as a yield^ does, so
    // the two are the same type -- which is the whole point of it, since
    // 13.9's三つ has to match what the consumer was written against.
    LHAT_TEST("_yield^ gives the same coroutine type a yield^ does");
    check_text(&u,
               "var^ real = p^ -> number^ {\n"
               "  var^ got : string^ = yield^ 1\n"
               "  return^ 9\n"
               "}\n"
               "var^ fake = p^ -> number^ {\n"
               "  var^ _^ : string^ = _yield^ 1\n"
               "  return^ 9\n"
               "}\n"
               "var^ a : c^{ p^string^ -> number^ -> number^ } = real()\n"
               "var^ b : c^{ p^string^ -> number^ -> number^ } = fake()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.11: a _yield^ never runs, so a name cannot take what it would
    // have answered -- only _^ stands on the left, the annotation being
    // all the statement says.
    LHAT_TEST("a _yield^ bound to a name is refused");
    check_text(&u,
               "var^ fake = p^ -> number^ {\n"
               "  var^ got : string^ = _yield^ 1\n"
               "  return^ 9\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PHANTOM_YIELD_BINDS);
    unit_dispose(&u);

    LHAT_TEST("and so is one reassigned into a name");
    check_text(&u,
               "var^ fake = p^ -> number^ {\n"
               "  var^ got : string^ = \"\"\n"
               "  got := _yield^ 1\n"
               "  return^ 9\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PHANTOM_YIELD_BINDS);
    unit_dispose(&u);

    LHAT_TEST("several _^ fix a tuple R off a _yield^");
    check_text(&u,
               "var^ fake = p^ {\n"
               "  var^ _^:string^, _^:number^ = _yield^ (\"a\", 1)\n"
               "}\n"
               "var^ c = fake()\n"
               "c.start()\n"
               "c.resume(\"x\", 2)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a body with only _yield^ is still a coroutine");
    check_text(&u,
               "var^ fake = p^ { _yield^ 1 }\n"
               "var^ c : c^{ p^-> number^} = fake()\n"
               "var^ d : bool^ = c.done()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // Same node, same rule: 15.8 does not care which of the two made it.
    LHAT_TEST("and dropping what it answers is reported the same way");
    check_text(&u,
               "var^ fake = p^ { _yield^ 1 }\n"
               "fake()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_COROUTINE_DROPPED);
    unit_dispose(&u);

    LHAT_TEST("and the wrong type is caught the same way");
    check_text(&u,
               "var^ fake = p^ { var^ got : string^ = _yield^ 1 }\n"
               "var^ c : c^{ p^number^ -> number^} = fake()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 16.3: a table answers iterate() with a walk over its keys, without
    // anything being written. The machine has always read it that way; the
    // type of it was missing here.
    LHAT_TEST("a table carries the built-in iterate");
    check_text(&u,
               "var^ t = { 1, 2 }\n"
               "var^ w = t.iterate^()\n"
               "var^ d : bool^ = w.done()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.8改: the walk yields the tuple (K, V), so what start() answers is
    // '(K, V)|nil^' -- a maybe-run no name can hold. The loop is what
    // discriminates and takes one apart; hand-driving a walk in checked
    // code has nothing to bind the answer to.
    LHAT_TEST("and the walk's answer is a maybe-tuple no name can hold");
    check_text(&u,
               "var^ t = { 1, 2 }\n"
               "var^ pair = t.iterate^().start()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TUPLE_MISPLACED);
    unit_dispose(&u);

    // 16.3 lets a written iterate win, which is why the built-in is only
    // reached once the search for a written member has failed.
    LHAT_TEST("a written iterate wins over the built-in one");
    check_text(&u,
               "var^ t = { iterate^ := f^ { return^ p^ { yield^ 9 }() } }\n"
               "var^ v : number^|nil^ = t.iterate^().start()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.5: and it may yield on its own rather than hand back a coroutine it
    // made -- 16.3 asks that a coroutine arrive, not who wrote it. This is
    // the shorter of the two spellings and the one anyone writes first.
    LHAT_TEST("and one that yields for itself is the same thing");
    check_text(&u,
               "var^ t = { iterate^ := f^self^ { yield^ 9 } }\n"
               "for^ v in^ t { var^ n : number^ = v }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.5 with 05 の 8.7: what a call answers is part of the signature, so
    // the type of a coroutine-making subroutine can be written down -- and
    // 14.16 writes exactly this form out.
    LHAT_TEST("the type of a coroutine-making subroutine can be written");
    check_text(&u,
               "var^ gen = p^ n:number^ { yield^ n }\n"
               "var^ g : p^number^ -> c^{ p^-> number^}; = gen\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.2 with 15.5: a signature answering nothing is not one that answers a
    // coroutine. Before the answer was part of the type this went through,
    // and the call under it was typed as producing no value at all.
    LHAT_TEST("and a signature answering nothing does not take one");
    check_text(&u,
               "var^ gen = p^ n:number^ { yield^ n }\n"
               "var^ g : p^number^; = gen\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 03 の 3.4改: a signature written on the binding says what the result is.
    // 13.9's third type is what the body returns, so that is what the body is
    // handed down -- the coroutine around it belongs to the caller.
    LHAT_TEST("a written signature hands the body the third type, not the coroutine");
    check_text(&u,
               "var^ g : p^number^ -> c^{ p^-> number^ -> string^ }; =\n"
               "    p^ n:number^ { yield^ n return^ \"z\" }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.5: the same statement about the same value, so the same reading --
    // 13.9's "書く場所が違う" is about where it stands, not about a body that
    // makes a coroutine meaning something else by it. 05 の 8.7 wants typeof^'s
    // answer to read back, and it reads back in both positions.
    LHAT_TEST("and the literal's own arrow takes that coroutine too");
    check_text(&u,
               "var^ g = p^ n:number^ -> c^{ p^-> number^ -> string^ }\n"
               "    { yield^ n return^ \"z\" }\n"
               "var^ s : number^|string^ = g(1).start()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.9: a body reaching its end leaves the third slot empty -- 15.6改's
    // nil^ is what the last resume is handed, so it joins Y|T there rather
    // than standing in the type.
    LHAT_TEST("a body with no return^ leaves the third slot empty");
    check_text(&u,
               "var^ g = p^ n:number^ -> c^{ p^-> number^}\n"
               "    { yield^ n }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.9: the three slots each have a way of saying "none", and each says
    // something a reader can act on. Written back as an annotation they say
    // the same thing again, which is what 05 の 8.7 asks of typeof^'s answer.
    LHAT_TEST("each of the ways a slot can be empty is its own type");
    check_text(&u,
               "var^ ends    : p^ -> c^{ p^-> number^};         ="
               " p^ { yield^ 1 }\n"
               "var^ answers : p^ -> c^{ p^-> number^ -> number^ }; ="
               " p^ { yield^ 1 return^ 2 }\n"
               "var^ never   : p^ -> c^{ p^-> number^ -> - };        ="
               " p^ { repeat^ { yield^ 1 } }\n"
               "var^ silent  : p^ -> c^{ p^-> nil^};            ="
               " p^ { yield^ }\n"
               "var^ takes   : p^ -> c^{ p^string^ -> number^}; ="
               " p^ { var^ x : string^ = yield^ 1 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // An empty R is not a nil^ one: 14.12 already holds arities {0} and {1}
    // disjoint, and a resume that takes nothing is not one that takes a value.
    LHAT_TEST("an empty receive is not a nil^ one");
    check_text(&u,
               "var^ g : p^ -> c^{ p^nil^ -> number^}; ="
               " p^ { yield^ 1 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_COROUTINE_MISMATCH);
    unit_dispose(&u);

    // And '-' is not an empty third slot: one answers Y where the other
    // answers Y|nil^, so a consumer of either would be told the wrong thing.
    LHAT_TEST("and '-' is not an empty third slot");
    check_text(&u,
               "var^ g : p^ -> c^{ p^-> number^ -> - }; = p^ { yield^ 1 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_COROUTINE_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("nor the other way round");
    check_text(&u,
               "var^ g : p^ -> c^{ p^-> number^}; ="
               " p^ { repeat^ { yield^ 1 } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_COROUTINE_MISMATCH);
    unit_dispose(&u);

    // 13.9: a body that cannot end never reaches a last resume, so nothing
    // joins Y -- putting nil^ there would have every consumer narrow away a
    // value that never arrives.
    LHAT_TEST("a coroutine that cannot end answers only what it yields");
    check_text(&u,
               "var^ g = p^ { repeat^ { yield^ 1 } }\n"
               "var^ v : number^ = g().start()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("while one that ends is handed nil^ at the last resume");
    check_text(&u,
               "var^ g = p^ { yield^ 1 }\n"
               "var^ v : number^ = g().start()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // '-' reads as "none" only where 13.9 puts it. Nothing else in the type
    // grammar begins with it.
    LHAT_TEST("'-' is not a type anywhere else");
    check_text(&u, "var^ x : - = 1\n");
    LHAT_CHECK(syntax_errors(&u) > 0, "expected a syntax error");
    unit_dispose(&u);

    // 15.2 settles Y and R from the yield^ sites, so a written c^{ … } is a
    // second statement about them and the two have to agree.
    LHAT_TEST("what it yields has to be what the signature wrote");
    check_text(&u,
               "var^ g = p^ n:number^ -> c^{ p^-> string^}\n"
               "    { yield^ n }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_COROUTINE_MISMATCH);
    unit_dispose(&u);

    // 15.3改: and so does the kind, which decides who may advance it.
    LHAT_TEST("and so does the kind of body it came from");
    check_text(&u,
               "var^ g = p^ -> c^{ f^-> number^} { yield^ 1 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_COROUTINE_MISMATCH);
    unit_dispose(&u);

    // The unwrap is for a body that makes one. A body that hands back a
    // coroutine it got from somewhere else really does result in that type.
    LHAT_TEST("a body answering a coroutine it did not make keeps that result");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "var^ outer = f^ -> c^{ p^-> number^}"
               " { return^ gen() }\n"
               "var^ v : number^|nil^ = outer().start()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.5: the arguments belong to the call, which binds the parameters.
    // 15.4's resume carries the one value a yield^ answers with, which 13.9
    // types as the coroutine's receive type -- a different thing.
    LHAT_TEST("the arguments belong to the call");
    check_text(&u,
               "var^ p = p^ x:number^, y:string^ { var^ a:number^ = yield^ 10 return^ \"a\" }\n"
               "var^ c = p(1, \"b\")\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the call still checks them");
    check_text(&u,
               "var^ p = p^ x:number^, y:string^ { yield^ 10 }\n"
               "var^ c = p(1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    // 13.9: what a resume answers is the union of the yield type and the
    // return type, which the consumer tells apart. 15.2: both are now
    // inferred from the body's yield^/return^ sites.
    LHAT_TEST("what a resume answers includes both the yield and the return type");
    check_text(&u,
               "var^ p = p^ -> string^ { var^ a:number^ = yield^ 10 return^ \"a\" }\n"
               "var^ c = p()\n"
               "var^ s : number^|string^ = c.resume(1)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.6改: the return type is what the last resume receives, so a body
    // that reaches its end without one puts nil^ there -- that is the value
    // the machine really hands back, not 03's "returns nothing" leaking in.
    LHAT_TEST("a body with no return^ answers nil^ at the end");
    check_text(&u,
               "var^ p = p^ { yield^ 1 }\n"
               "var^ v : number^|nil^ = p().start()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("so the yield type alone does not cover it");
    check_text(&u,
               "var^ p = p^ { yield^ 1 }\n"
               "var^ v : number^ = p().start()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // And where every exit produces one, nothing spurious joins the union.
    LHAT_TEST("a body that always returns a value brings no nil^");
    check_text(&u,
               "var^ p = p^ { yield^ 1 return^ 9 }\n"
               "var^ v : number^ = p().start()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.9: nor does one that cannot end at all. 16.5's endless repeat^ has
    // no last resume, so there is no value for the third type to be the type
    // of -- and a nil^ there would make every consumer narrow away something
    // that never arrives.
    LHAT_TEST("a coroutine that cannot end answers only what it yields");
    check_text(&u,
               "var^ p = p^ { repeat^ { yield^ 1 } }\n"
               "var^ v : number^ = p().start()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and its walk binds a focus with no nil^ in it either");
    check_text(&u,
               "var^ p = p^ { repeat^ { yield^ 1 } }\n"
               "for^ x in^ p() { var^ n : number^ = x }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A bare return^ ends it, so nil^ is back -- the resume that takes that
    // exit really receives one.
    LHAT_TEST("but a bare return^ is an end, and brings nil^ with it");
    check_text(&u,
               "var^ p = p^ { yield^ 1 return^ }\n"
               "var^ v : number^ = p().start()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and so does a break^ that lets the loop finish");
    check_text(&u,
               "var^ p = p^ { repeat^ { yield^ 1 break^ } }\n"
               "var^ v : number^ = p().start()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // done() is what tells the two apart, and it is there either way.
    LHAT_TEST("done() is carried whether or not the body can end");
    check_text(&u,
               "var^ p = p^ { repeat^ { yield^ 1 } }\n"
               "var^ d : bool^ = p().done()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.14: the one spelling has the one message, and it says what await^
    // is for. NOT_COROUTINE stays the walk's (for^ in^ over something with
    // no iterate^).
    LHAT_TEST("await^ needs a coroutine");
    check_text(&u,
               "var^ plain = f^ -> number^ { return^ 1 }\n"
               "var^ outer = p^ { await^ plain() }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_AWAIT_NOT_COROUTINE);
    unit_dispose(&u);

    // 15.8: the value of a delegation is the inner one's return value.
    LHAT_TEST("the value of await^ is the inner return type");
    check_text(&u,
               "var^ gen = p^ -> number^ { yield^ 1 return^ 2 }\n"
               "var^ outer = p^ { var^ n : number^ = await^ gen() }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and it is not the type it yields");
    check_text(&u,
               "var^ gen = p^ -> number^ { yield^ 1 return^ 2 }\n"
               "var^ outer = p^ { var^ s : string^ = await^ gen() }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 15.14: await^ is the same delegation, so the same type comes out of it
    // -- what is awaited finishes, and its T is what the wait answers with.
    LHAT_TEST("await^ answers what the awaited one returns");
    check_text(&u,
               "var^ gen = p^ -> number^ { yield^ 1 return^ 2 }\n"
               "var^ outer = p^ { var^ n : number^ = await^ gen() }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and it says so in its own words when there is nothing to wait for");
    check_text(&u,
               "var^ plain = f^ -> number^ { return^ 1 }\n"
               "var^ outer = p^ { var^ n = await^ plain() }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_AWAIT_NOT_COROUTINE);
    unit_dispose(&u);

    // 15.2 with 15.5: writing await^ is what makes the body yieldable, and
    // calling that body answers a coroutine rather than running it. No async
    // marker is written anywhere -- 15.5's colouring never starts.
    LHAT_TEST("a body that awaits is yieldable, and its caller is not");
    check_text(&u,
               "var^ gen = p^ -> number^ { yield^ 1 return^ 2 }\n"
               "var^ task = p^ { var^ n : number^ = await^ gen() }\n"
               "var^ plain = p^ { var^ c = task() c.dispose() }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.2: start() takes nothing and answers the same union resume does --
    // it is what runs a fresh coroutine from the top.
    LHAT_TEST("start() answers the same union as resume()");
    check_text(&u,
               "var^ p = p^ -> string^ { var^ a:number^ = yield^ 10 return^ \"a\" }\n"
               "var^ c = p()\n"
               "var^ s : number^|string^ = c.start()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("start() takes no arguments");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "var^ c = gen()\n"
               "c.start(1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    // 13.9: nothing in this body receives a yield^, so R is empty and there is
    // nothing to send -- a resume of it takes no argument at all.
    LHAT_TEST("a resume of a coroutine nothing is sent to takes no argument");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "var^ c = gen()\n"
               "c.resume()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and writing nil^ there is an argument it has no parameter for");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "var^ c = gen()\n"
               "c.resume(nil^)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    // Where a var^ does receive one, R is that type and the resume takes it.
    LHAT_TEST("but one that receives takes exactly one argument");
    check_text(&u,
               "var^ gen = p^ { var^ a : number^ = yield^ 1 }\n"
               "var^ c = gen()\n"
               "c.resume()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    LHAT_TEST("and that one argument is of R");
    check_text(&u,
               "var^ gen = p^ { var^ a : number^ = yield^ 1 }\n"
               "var^ c = gen()\n"
               "c.resume(2)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("resume takes no more than the one");
    check_text(&u,
               "var^ gen = p^ { var^ a : number^ = yield^ 1 }\n"
               "var^ c = gen()\n"
               "c.resume(1, 2)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    // 13.9: every yield^ in a body has to agree on what it sends (Y) and
    // what it answers (R) -- no more folding differing yields into a union.
    LHAT_TEST("two bare yield^ that agree on Y stay clean");
    check_text(&u,
               "var^ p = p^ { yield^ 1 yield^ 2 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("two bare yield^ that disagree on Y are reported");
    check_text(&u,
               "var^ p = p^ { yield^ 1 yield^ \"a\" }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_YIELD_TYPE_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("two bound yield^ that disagree on R are reported");
    check_text(&u,
               "var^ p = p^ {\n"
               "    var^ a:number^ = yield^ 1\n"
               "    var^ b:string^ = yield^ 2\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_YIELD_TYPE_MISMATCH);
    unit_dispose(&u);

    // 15.2: a yield^ that a var^ binds directly is the only place R can be
    // written, so leaving the annotation off is reported rather than left
    // to infer to UNKNOWN.
    LHAT_TEST("a bound yield^ needs a written type");
    check_text(&u,
               "var^ p = p^ { var^ a = yield^ 1 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_YIELD_NEEDS_ANNOTATION);
    unit_dispose(&u);

    // 15.4: a reassigned yield^ needs no annotation -- the name already
    // holds its type, and that is what fixes R.
    LHAT_TEST("a reassigned yield^ reads R off what the name holds");
    check_text(&u,
               "var^ p = p^ {\n"
               "    var^ a:number^ = 0\n"
               "    a := yield^ 1\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a disagreeing reassignment is the usual mismatch");
    check_text(&u,
               "var^ p = p^ {\n"
               "    var^ a:number^ = 0\n"
               "    var^ s:string^ = yield^ 1\n"
               "    a := yield^ 2\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_YIELD_TYPE_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("a yield^ buried in an expression has nowhere to write it either");
    check_text(&u,
               "var^ p = p^ -> number^ { return^ 1 + yield^ 1 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_YIELD_NEEDS_ANNOTATION);
    unit_dispose(&u);

    // 15.8: await^ passes the inner coroutine's Y/R through as this
    // body's own, exactly as if a yield^ had been written here directly.
    LHAT_TEST("await^ can disagree with this body's own yield^ on R");
    check_text(&u,
               "var^ inner = p^ { var^ a:number^ = yield^ 1 }\n"
               "var^ outer = p^ {\n"
               "    var^ b:string^ = yield^ \"x\"\n"
               "    await^ inner()\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_YIELD_TYPE_MISMATCH);
    unit_dispose(&u);
}

// 15.4 with 13.8改: a tuple R -- several names binding one yield^ fix it,
// and resume() takes that many arguments.
static void test_multi_value_receive(void)
{
    Unit u;

    LHAT_TEST("several annotated names fix a tuple R");
    check_text(&u,
               "var^ gen = p^ {\n"
               "    let^ a:number^, b:string^ = yield^ 0\n"
               "    return^ a\n"
               "}\n"
               "var^ c = gen()\n"
               "c.start()\n"
               "var^ got = c.resume(1, \"x\")\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the resume's own arity is the tuple's width");
    check_text(&u,
               "var^ gen = p^ {\n"
               "    let^ a:number^, b:string^ = yield^ 0\n"
               "    return^ a\n"
               "}\n"
               "var^ c = gen()\n"
               "c.start()\n"
               "c.resume(1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    LHAT_TEST("a name with no annotation still needs one");
    check_text(&u,
               "var^ gen = p^ {\n"
               "    let^ a:number^, b = yield^ 0\n"
               "    return^ a\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_YIELD_NEEDS_ANNOTATION);
    unit_dispose(&u);

    LHAT_TEST("a written c^ has to agree with the taking-apart");
    check_text(&u,
               "var^ gen = p^ -> c^{p^number^ -> number^ -> number^} {\n"
               "    let^ a:number^, b:number^ = yield^ 0\n"
               "    return^ a + b\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_COROUTINE_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and matches when it spells the same tuple");
    check_text(&u,
               "var^ gen = p^ -> c^{p^number^, number^ -> number^ ->"
               " number^} {\n"
               "    let^ a:number^, b:number^ = yield^ 0\n"
               "    return^ a + b\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.8: the inner R is the outer R, tuple or not.
    LHAT_TEST("a await^ carries a tuple R outwards");
    check_text(&u,
               "var^ inner = p^ {\n"
               "    let^ a:number^, b:number^ = yield^ 1\n"
               "    yield^ a + b\n"
               "}\n"
               "var^ outer = p^ {\n"
               "    await^ inner()\n"
               "}\n"
               "var^ c = outer()\n"
               "c.start()\n"
               "c.resume(1, 2)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

// 13.8改: union(Y, T) with differing widths folds, the short side padded
// with nil^ -- '(string^, number^)|string^' is '(string^, number^|nil^)',
// a type a binding can take apart.
static void test_folded_answer(void)
{
    Unit u;

    LHAT_TEST("a tuple Y and a single T fold into one tuple");
    check_text(&u,
               "var^ gen = p^ {\n"
               "    yield^ \"a\", 1\n"
               "    return^ \"done\"\n"
               "}\n"
               "var^ c = gen()\n"
               "let^ s, n = c.start()\n"
               "var^ picked : number^ = n ?? 0\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the other way round");
    check_text(&u,
               "var^ gen = p^ {\n"
               "    yield^ 1\n"
               "    return^ \"done\", 2\n"
               "}\n"
               "var^ c = gen()\n"
               "let^ v, w = c.start()\n"
               "var^ picked : number^ = w ?? 0\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 16.3: the walk's '(K, V)|nil^' does not fold -- nil^ is the arm the
    // '??' discriminates, and folding it away would break the drive.
    LHAT_TEST("a walk's nil^ arm stays beside the tuple");
    check_text(&u,
               "let^ t = { 10, 20 }\n"
               "var^ w = t.iterate^()\n"
               "var^ k, v = w.start() ?? (nil^, nil^)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

// A conditional loop's condition is a condition -- bool^, as an if^'s is.
// 'until^ c.done' reads the member without calling it, and a signature is
// not a bool^; the machine used to be the one to say so.
static void test_loop_condition(void)
{
    Unit u;

    LHAT_TEST("an uncalled done is not a condition");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "var^ c = gen()\n"
               "c.start()\n"
               "repeat^until^c.done {\n"
               "    c.resume()\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_BOOL);
    unit_dispose(&u);

    LHAT_TEST("and the called one is");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "var^ c = gen()\n"
               "c.start()\n"
               "repeat^until^c.done() {\n"
               "    c.resume()\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a for^ while^ condition is held to bool^ too");
    check_text(&u,
               "for^ var^ i = 0 while^ i next^ i := i + 1 { }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_BOOL);
    unit_dispose(&u);
}

int main(void)
{
    test_coroutines();
    test_multi_value_receive();
    test_folded_answer();
    test_loop_condition();
    return lhat_test_report("test_check_coroutine");
}
