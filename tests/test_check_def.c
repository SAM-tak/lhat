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

// 14 章 and 12.5.
static void test_definitions(void)
{
    Unit u;

    // 14.7: an instance reaches the definition's members, so its type holds
    // both those and the template's fields.
    LHAT_TEST("an instance has the fields and the members");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ value := 0, label := \"\" },\n"
               "    show := p^self^ { },\n"
               "}\n"
               "var^ c = C.new()\n"
               "var^ n : number^ = c.value\n"
               "var^ s : string^ = c.label\n"
               "c.show()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.7改 with 14.4: an instance carries what takes a receiver, and 14.11's
    // new takes none -- it is the definition's, and an instance is not another
    // way to make one.
    LHAT_TEST("an instance does not reach new");
    check_text(&u,
               "var^ C = def^{ self^{ value := 0 } }\n"
               "var^ c = C.new().new()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    LHAT_TEST("nor a static member");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ value := 0 },\n"
               "    tag := f^ -> number^ { return^ 1 },\n"
               "}\n"
               "var^ n : number^ = C.new().tag()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    LHAT_TEST("while the definition reaches both");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ value := 0 },\n"
               "    tag := f^ -> number^ { return^ 1 },\n"
               "    show := p^self^ { },\n"
               "}\n"
               "var^ n : number^ = C.tag()\n"
               "var^ f = C.show\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.9: and class^ inside a member is the definition, so a static member
    // is reached from there the way it is from the name.
    LHAT_TEST("and class^ reaches a static member from inside");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ value := 0 },\n"
               "    tag := f^ -> number^ { return^ 1 },\n"
               "    show := p^self^ -> number^ { return^ class^.tag() },\n"
               "}\n"
               "var^ n : number^ = C.new().show()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.16 with 05 の 8.7: what 14.16 writes out reads back as a type. The
    // definition's carries the self^{ … } section, and 13.13's Self^ inside
    // it is the instance -- the two annotations here are the two signatures
    // typeof^ answers with, written by hand.
    LHAT_TEST("a written definition type says what a def^ builds");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ a := 0 },\n"
               "    add := f^self^, rhs:Self^ -> Self^ { return^ self^ },\n"
               "    tag := f^ -> number^ { return^ 1 },\n"
               "}\n"
               "var^ definition : t^{ self^{ a : number^,\n"
               "                            add : f^self^, Self^ -> Self^; },\n"
               "                      new : f^ -> Self^;,\n"
               "                      tag : f^ -> number^; } = C\n"
               "var^ instance : t^{ a : number^,\n"
               "                    add : f^self^, Self^ -> Self^; } = C.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The section is what an instance carries, so a plain structure does not
    // stand where a definition is written.
    LHAT_TEST("and an instance is not one");
    check_text(&u,
               "var^ C = def^{ self^{ a := 0 } }\n"
               "var^ definition : t^{ self^{ a : number^ } } = C.new()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 14.7改: the members reach each other whatever the order they are
    // written in. A ring of them is what makes this more than a convenience:
    // no ordering unties expr -> term -> factor -> expr.
    LHAT_TEST("a member reaches one written after it");
    check_text(&u,
               "var^ A = def^{\n"
               "    self^{ n := 0 },\n"
               "    first := p^self^ -> number^ { return^ self^.second() },\n"
               "    second := p^self^ -> number^ { return^ 1 },\n"
               "}\n"
               "var^ n : number^ = A.new().first()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a ring of them needs no declaration");
    check_text(&u,
               "var^ R = def^{\n"
               "    self^{ n := 0 },\n"
               "    expr := p^self^ -> number^ { return^ self^.term() },\n"
               "    term := p^self^ -> number^ { return^ self^.factor() },\n"
               "    factor := p^self^ -> number^ { return^ self^.expr() },\n"
               "}\n"
               "var^ n : number^ = R.new().expr()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 03 の 3.4改2: nor a result type. The first walk reads the ring before
    // the bodies it runs through were looked at; the second starts from what
    // the first inferred, and that is where the ring closes.
    LHAT_TEST("nor a result type on any of them");
    check_text(&u,
               "var^ R = def^{\n"
               "    self^{ n := 0 },\n"
               "    expr := p^self^ { return^ self^.term() },\n"
               "    term := p^self^ { return^ self^.factor() },\n"
               "    factor := p^self^ {\n"
               "        if^ self^.n > 0 { return^ self^.expr() }\n"
               "        return^ 1\n"
               "    },\n"
               "}\n"
               "var^ n : number^ = R.new().expr()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The second walk says everything the first one said, so what the first
    // one reported has to be dropped rather than added to.
    LHAT_TEST("and what is wrong inside the ring is reported once");
    check_text(&u,
               "var^ R = def^{\n"
               "    self^{ n := 0 },\n"
               "    expr := p^self^ { return^ self^.term() },\n"
               "    term := p^self^ { return^ self^.factor() },\n"
               "    factor := p^self^ { return^ self^.nowhere() },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    LHAT_CHECK_EQ_INT(u.checked.diagnostic_count, 1);
    unit_dispose(&u);

    // 03 の 3.4改2: the walks stop when one of them answers what the last one
    // did. A member whose result is only ever its own answers the same thing
    // from the first walk on, and no number of them settles it -- which is
    // where 3.1 has always left it: the type is written down.
    LHAT_TEST("but a result that is only its own still has to be written");
    check_text(&u,
               "var^ R = def^{\n"
               "    self^{ n := 0 },\n"
               "    loop := p^self^ { return^ self^.loop() },\n"
               "}\n"
               "var^ n : number^ = R.new().loop()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and writing it is what settles it");
    check_text(&u,
               "var^ R = def^{\n"
               "    self^{ n := 0 },\n"
               "    loop := p^self^ -> number^ { return^ self^.loop() },\n"
               "}\n"
               "var^ n : number^ = R.new().loop()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.7改 with 03 の 3.4: what the first pass can put down is what can be
    // read without walking a body. A member whose type only its value knows
    // is not among them, so reaching one before it is written still finds
    // nothing -- the same line 3.4 draws for a subroutine that calls itself.
    LHAT_TEST("but a value member is not reachable before it is written");
    check_text(&u,
               "var^ A = def^{\n"
               "    self^{ n := 0 },\n"
               "    first := p^self^ -> number^ { return^ class^.limit },\n"
               "    limit := 10,\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // 14.12's question is what was already under the name, and a member read
    // ahead by the first pass was not -- writing it once is not writing it
    // twice.
    LHAT_TEST("reading ahead does not make every member a second one");
    check_text(&u,
               "var^ A = def^{\n"
               "    self^{ n := 0 },\n"
               "    m := f^ -> number^ { return^ 1 },\n"
               "    k := f^ -> number^ { return^ 2 },\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and one written twice still wants its marker");
    check_text(&u,
               "var^ A = def^{\n"
               "    self^{ n := 0 },\n"
               "    m := f^ -> number^ { return^ 1 },\n"
               "    m := f^ -> number^ { return^ 2 },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MEMBER_EXISTS);
    unit_dispose(&u);

    // 14.11: a definition without one still offers a new taking nothing.
    LHAT_TEST("new exists without being written");
    check_text(&u, "var^ C = def^{ self^{ v := 1 } }\nvar^ c = C.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.11改: and it is a member like any other, so writing one is 14.12's
    // second member of that name. Unmarked, that is the collision 14.12 is
    // about -- which is what keeps 'adding new(a, b)' from silently taking
    // new() away.
    LHAT_TEST("a written new wants a marker");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 1 },\n"
               "    new := f^ n:number^ { self^{ v := n } },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MEMBER_EXISTS);
    unit_dispose(&u);

    // 14.12改2: an override^ new replaces the default whole, whatever it
    // takes -- the exemption from substitutability. This is how a definition
    // says construction needs arguments.
    LHAT_TEST("an override^ new may ask for arguments the default did not");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 1 },\n"
               "    override^new := f^ n:number^ { self^{ v := n } },\n"
               "}\n"
               "var^ c = C.new(5)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the default is gone once it does");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 1 },\n"
               "    override^new := f^ n:number^ { self^{ v := n } },\n"
               "}\n"
               "var^ c = C.new()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    // 14.12: the other half. An overload^ adds a way to call it and leaves
    // what was there, so both spellings construct.
    LHAT_TEST("an overload^ new keeps the default beside it");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 1 },\n"
               "    overload^new := f^ n:number^ { self^{ v := n } },\n"
               "}\n"
               "var^ a = C.new()\n"
               "var^ b = C.new(5)\n"
               "var^ x : number^ = a.v + b.v\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.7 with 14.12: the instance type is read off what new answers, and an
    // overloaded new answers it from every arm. Without that reading the
    // definition's name would name nothing writable.
    LHAT_TEST("an overloaded new still names an instance type");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 1 },\n"
               "    overload^new := f^ n:number^ { self^{ v := n } },\n"
               "}\n"
               "var^ take = f^ c:C -> number^ { return^ c.v }\n"
               "var^ n : number^ = take(C.new(5))\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.11: self^{ … } inside new is the construction notation, and what it
    // names is the instance of the def^ it stands in. 14.4 hands a receiver
    // only to a member that wrote self^ among its parameters, and new does
    // not -- that is a different question from what this notation means.
    // 14.11: and only a written new runs it. A method has a receiver, but
    // someone else already holds that table -- it adjusts it one field at a
    // time, through self^.name, where 14.4's rules see each write.
    LHAT_TEST("a method body does not take the construction notation");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 1 },\n"
               "    m := p^self^ { self^{ v := 2 } },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_SELF_TABLE_OUTSIDE_NEW);
    unit_dispose(&u);

    LHAT_TEST("nor does a static member");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 1 },\n"
               "    s := p^ { self^{ v := 2 } },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_SELF_TABLE_OUTSIDE_NEW);
    unit_dispose(&u);

    LHAT_TEST("what a written new answers is an instance");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 1 },\n"
               "    override^new := f^ n:number^ { self^{ v := n } },\n"
               "}\n"
               "var^ n : number^ = C.new(5).v\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 03 の 3.4: a field is held to what the template says it holds, so a
    // parameter handed straight into one is decided by that.
    LHAT_TEST("a field decides the type of the parameter written into it");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 1 },\n"
               "    override^new := f^ x { self^{ v := x } },\n"
               "}\n"
               "var^ c = C.new(\"a\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and the same one written with a fitting value checks");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 1 },\n"
               "    override^new := f^ x { self^{ v := x } },\n"
               "}\n"
               "var^ n : number^ = C.new(5).v\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a value that does not fit the field is reported");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 1 },\n"
               "    override^new := f^ { self^{ v := \"a\" } },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 14.7: an instance is closed to a member added afterwards, so a name the
    // template does not declare is not a field to fill in.
    LHAT_TEST("a field the template does not declare is not one");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 1 },\n"
               "    override^new := f^ { self^{ zzz := 1 } },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    LHAT_TEST("a field the definition does not declare is reported");
    check_text(&u,
               "var^ C = def^{ self^{ v := 1 } }\n"
               "var^ c = C.new()\n"
               "var^ x = c.missing\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // 14.4: self^ reaches the instance from inside a method.
    LHAT_TEST("self^ reaches the fields");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 0 },\n"
               "    bump := p^self^, step:number^ {\n"
               "        self^.v := self^.v + step\n"
               "    },\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("class^ reaches the definition's members");
    check_text(&u,
               "var^ print = p^ x:any^ { }\n"
               "var^ C = def^{\n"
               "    self^{ v := 0 },\n"
               "    origin := 7,\n"
               "    show := p^ { print(class^.origin) },\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.4: a member that did not write self^ is static and is handed no
    // receiver, so the name means nothing inside it. Writing p^ where
    // p^self^ was meant is the everyday way to arrive here, and it used to
    // read as a name in scope and fall over at compile time with nowhere to
    // point.
    LHAT_TEST("a static member does not see self^");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 0 },\n"
               "    read := p^ -> number^ { return^ self^.v },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
    unit_dispose(&u);

    // 5.4: what a body may reach, a body written inside it may reach too.
    LHAT_TEST("but a subroutine inside a method still captures it");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 0 },\n"
               "    read := p^self^ -> number^ {\n"
               "        var^ g = f^ -> number^ { return^ self^.v }\n"
               "        return^ g()\n"
               "    },\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.4: the receiver is not written at the call, so it is not counted.
    LHAT_TEST("a method call does not pass the receiver");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 0 },\n"
               "    bump := p^self^, step:number^ { },\n"
               "}\n"
               "var^ c = C.new()\n"
               "c.bump(1)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a method call still checks its arguments");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 0 },\n"
               "    bump := p^self^, step:number^ { },\n"
               "}\n"
               "var^ c = C.new()\n"
               "c.bump(\"text\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("a method call with too few arguments is reported");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 0 },\n"
               "    bump := p^self^, step:number^ { },\n"
               "}\n"
               "var^ c = C.new()\n"
               "c.bump()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    // 14.4: taking the method out from under the dot is what spells the
    // receiver out, and then it is counted like any other argument.
    LHAT_TEST("a method taken as a value is passed the receiver");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 0 },\n"
               "    bump := p^self^, step:number^ { },\n"
               "}\n"
               "var^ c = C.new()\n"
               "var^ bump = C.bump\n"
               "bump(c, 1)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.4: the form is what decides, and a parenthesis does not change it.
    // JavaScript reads '(obj.m)()' the same way.
    LHAT_TEST("parentheses do not take the receiver off");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 0 },\n"
               "    bump := p^self^, step:number^ { },\n"
               "}\n"
               "var^ c = C.new()\n"
               "(C.bump)(c, 1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    // The other side of the dot is unaffected -- there the parenthesis holds
    // the receiver, and what is called is still a member access.
    LHAT_TEST("a parenthesised receiver is still a method call");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 0 },\n"
               "    bump := p^self^, step:number^ { },\n"
               "}\n"
               "var^ x = C.new()\n"
               "var^ y = C.new()\n"
               "(if^ true^: x el^: y;).bump(1)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 12.5 with 12.7: with^ wants a dispose(), and one that returns nothing,
    // because 12.7 made cleanup unable to fail.
    LHAT_TEST("with^ accepts a value that has dispose()");
    check_text(&u,
               "var^ C = def^{ self^{ v := 1 }, dispose := p^self^ { } }\n"
               "with^ c = C.new() { }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("with^ refuses a value without one");
    check_text(&u,
               "var^ C = def^{ self^{ v := 1 } }\n"
               "with^ c = C.new() { }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_DISPOSABLE);
    unit_dispose(&u);

    LHAT_TEST("with^ refuses a dispose() that returns something");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 1 },\n"
               "    dispose := f^self^ -> number^ { return^ 0 },\n"
               "}\n"
               "with^ c = C.new() { }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_DISPOSABLE);
    unit_dispose(&u);

    // 12.5 writes the condition as 'dispose()', parentheses and all: with^
    // hands nothing over, so one asking for an argument could never be called.
    LHAT_TEST("with^ refuses a dispose() that asks for an argument");
    check_text(&u,
               "var^ C = def^{\n"
               "    self^{ v := 1 },\n"
               "    dispose := p^self^, x:number^ { },\n"
               "}\n"
               "with^ c = C.new() { }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_DISPOSABLE);
    unit_dispose(&u);

    // 13.4 keeps self^ out of the parameter list, so 14.4's ordinary shape
    // has an empty one and is accepted -- which the case above must not have
    // taken with it.
    LHAT_TEST("and accepts one written without a self^ as well");
    check_text(&u,
               "var^ t = { dispose := p^ { } }\n"
               "with^ r = t { }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.10: the structural form is what 12.5 is really asking for, so it
    // has to resolve on its own.
    LHAT_TEST("the structural form of a disposable resolves");
    check_text(&u,
               "var^ C = def^{ self^{ v := 1 }, dispose := p^self^ { } }\n"
               "var^ c = C.new()\n"
               "var^ d : t^{ dispose : p^self^; } = c\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 05 の 2.2: one environment for names, which is what 14.9 needs -- it
    // says a definition takes its name from its binding, so that name has to
    // be reachable where a type is written.
    LHAT_TEST("a definition's name works as a type");
    check_text(&u,
               "var^ Foo = def^{ self^{ v := 1 } }\n"
               "var^ x : Foo = Foo.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.7: writing the name asks for the whole structure, and that is what
    // an instance carries -- so as a type the name means an instance.
    LHAT_TEST("as a type the name means an instance");
    check_text(&u,
               "var^ Foo = def^{ self^{ v := 1 } }\n"
               "var^ x : Foo = Foo.new()\n"
               "var^ n : number^ = x.v\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("the definition itself is not an instance");
    check_text(&u,
               "var^ Foo = def^{ self^{ v := 1 } }\n"
               "var^ x : Foo = Foo\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 05 の 2.2: an errordef^ name lives in the same place, so a value of the
    // same name collides rather than shadowing quietly.
    LHAT_TEST("a type and a value of one name collide");
    check_text(&u,
               "errordef^ E { A }\n"
               "var^ E = 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_REDEFINED);
    unit_dispose(&u);

    // 14.9: the name is a label, so two definitions of the same shape are one
    // type and nothing has to be shared for that to hold.
    LHAT_TEST("identity stays structural");
    check_text(&u,
               "var^ A = def^{ self^{ v := 0 } }\n"
               "var^ B = def^{ self^{ v := 0 } }\n"
               "var^ take = p^ x:t^{ v : number^ } { }\n"
               "take(A.new())\n"
               "take(B.new())\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

// 14.5 and 14.12.
static void test_composition(void)
{
    Unit u;

    static const char *const base =
        "var^ Foo = def^{\n"
        "    self^{ a := 0 },\n"
        "    foo := p^self^, x:string^ { },\n"
        "    bar := p^self^ { },\n"
        "}\n";

    // 14.5: the derived definition carries both sides, and 14.7 means an
    // instance of it reaches the base's fields as well as its own.
    LHAT_TEST("composition carries the base's fields and members");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "var^ Bar = Foo .. def^{ self^{ b := 0 }, extra := p^self^ { } }\n"
                 "var^ o = Bar.new()\n"
                 "var^ x : number^ = o.a\n"
                 "var^ y : number^ = o.b\n"
                 "o.bar()\n"
                 "o.extra()\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.12: without a marker, a name the base already uses is a mistake.
    LHAT_TEST("a same-named member needs a marker");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "var^ Bar = Foo .. def^{ self^{}, foo := p^self^ { } }\n");
        check_text(&u, text);
    }
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MEMBER_EXISTS);
    unit_dispose(&u);

    // 14.12: arguments may widen, which is what makes the replacement usable
    // wherever the original was.
    LHAT_TEST("override^ may widen its arguments");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "var^ Bar = Foo .. def^{\n"
                 "    self^{},\n"
                 "    override^ foo := p^self^, x:string^|number^ { },\n"
                 "}\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("override^ may not narrow them");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "var^ Bar = Foo .. def^{\n"
                 "    self^{},\n"
                 "    override^ foo := p^self^, x:number^ { },\n"
                 "}\n");
        check_text(&u, text);
    }
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_SUBSTITUTABLE);
    unit_dispose(&u);

    // 14.12: overloading is only allowed where no call could fit both.
    LHAT_TEST("overload^ needs a signature that stays apart");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "var^ Bar = Foo .. def^{\n"
                 "    self^{},\n"
                 "    overload^ bar := p^self^, n:number^ { },\n"
                 "}\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.12: what a new arm has to stay apart from is each arm already under
    // the name, one at a time. Asking the intersection as though it were one
    // signature answers "cannot be told apart" for every one of them, which
    // would refuse a third arm however plainly it differed.
    LHAT_TEST("a third overload^ is told apart from each arm, not from all");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "var^ Bar = Foo .. def^{\n"
                 "    self^{},\n"
                 "    overload^ bar := p^self^, n:number^ { },\n"
                 "    overload^ bar := p^self^, b:bool^ { },\n"
                 "}\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a third that overlaps an earlier arm still is not");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "var^ Bar = Foo .. def^{\n"
                 "    self^{},\n"
                 "    overload^ bar := p^self^, n:number^ { },\n"
                 "    overload^ bar := p^self^, m:number^ { },\n"
                 "}\n");
        check_text(&u, text);
    }
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OVERLOAD_OVERLAPS);
    unit_dispose(&u);

    LHAT_TEST("an overlapping overload^ is reported");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "var^ Bar = Foo .. def^{\n"
                 "    self^{},\n"
                 "    overload^ foo := p^self^, y:string^ { },\n"
                 "}\n");
        check_text(&u, text);
    }
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OVERLOAD_OVERLAPS);
    unit_dispose(&u);

    // 14.15改: an override^ over nothing is not a mistake -- it says the
    // composition has to bring what it replaces. What it costs is that the
    // definition can no longer be instantiated on its own.
    LHAT_TEST("a marker with nothing under it waits for a composition");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "var^ Bar = Foo .. def^{ self^{}, override^ nope := p^self^ { } }\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and stands in the way of new until one comes");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "var^ Bar = Foo .. def^{ self^{}, override^ nope := p^self^ { } }\n"
                 "var^ o = Bar.new()\n");
        check_text(&u, text);
    }
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_STILL_ABSTRACT);
    unit_dispose(&u);

    // 14.12: overload^ has no such reading. Adding a way to call something
    // that is not there says nothing.
    LHAT_TEST("overload^ over nothing is still reported");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "var^ Bar = Foo .. def^{ self^{}, overload^ nope := p^self^ { } }\n");
        check_text(&u, text);
    }
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE);
    unit_dispose(&u);

    // 14.12: an overloaded member is callable both ways, which is what '&'
    // means, so the intersection is what the member ends up being.
    LHAT_TEST("an overloaded member accepts either signature");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "var^ Bar = Foo .. def^{\n"
                 "    self^{},\n"
                 "    overload^ bar := p^self^, n:number^ { },\n"
                 "}\n"
                 "var^ o = Bar.new()\n"
                 "o.bar()\n"
                 "o.bar(1)\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.12: once a name is overloaded it carries an intersection, and an
    // override^ replaces the one arm it overlaps. Comparing the replacement
    // against the whole intersection would refuse it, since no single
    // signature is usable where all of them were.
    LHAT_TEST("override^ replaces the one overloaded arm it overlaps");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "var^ Bar = Foo .. def^{\n"
                 "    self^{},\n"
                 "    overload^ bar := p^self^, n:number^ { },\n"
                 "}\n"
                 "var^ Baz = Bar .. def^{\n"
                 "    self^{},\n"
                 "    override^ bar := p^self^, n:number^ { },\n"
                 "}\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The arms that were not overridden are untouched, so the name goes on
    // being callable every way it was before.
    LHAT_TEST("the arms an override^ left alone stay callable");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "var^ Bar = Foo .. def^{\n"
                 "    self^{},\n"
                 "    overload^ bar := p^self^, n:number^ { },\n"
                 "}\n"
                 "var^ Baz = Bar .. def^{\n"
                 "    self^{},\n"
                 "    override^ bar := p^self^, n:number^ { },\n"
                 "}\n"
                 "var^ o = Baz.new()\n"
                 "o.bar()\n"
                 "o.bar(1)\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.12: override^ may widen its arguments, and a widening can reach two
    // of the arms at once. Which one was meant is then not decidable.
    LHAT_TEST("an override^ over two overloaded arms is reported");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "var^ Bar = Foo .. def^{\n"
                 "    self^{},\n"
                 "    overload^ bar := p^self^, n:number^ { },\n"
                 "}\n"
                 "var^ Baz = Bar .. def^{\n"
                 "    self^{},\n"
                 "    override^ bar := p^self^, ...:number^ { },\n"
                 "}\n");
        check_text(&u, text);
    }
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OVERLOAD_OVERLAPS);
    unit_dispose(&u);

    // Overlapping is what picks the arm, so a signature that overlaps none of
    // them has nothing to replace -- the same as a marker over a name that
    // was never there.
    LHAT_TEST("an override^ that overlaps no arm is reported");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "var^ Bar = Foo .. def^{\n"
                 "    self^{},\n"
                 "    overload^ bar := p^self^, n:number^ { },\n"
                 "}\n"
                 "var^ Baz = Bar .. def^{\n"
                 "    self^{},\n"
                 "    override^ bar := p^self^, a:number^, b:number^ { },\n"
                 "}\n");
        check_text(&u, text);
    }
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE);
    unit_dispose(&u);

    // 14.5 composes to make something new, so a constructor inherited from
    // the base still has to build the derived instance.
    LHAT_TEST("new builds the derived instance");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "var^ Bar = Foo .. def^{ self^{ b := \"\" } }\n"
                 "var^ s : string^ = Bar.new().b\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.5: the right side of '..' may be a name rather than a def^ literal.
    // The compiler flattens that chain through its own registry (14.2), so
    // the type has to carry both sides: answering with the right one alone
    // would lose everything the left brought.
    static const char *const parts =
        "var^ A = def^{ self^{ x := 0 }, a := f^ -> number^ { return^ 1 } }\n"
        "var^ B = def^{ self^{ y := \"s\" }, b := f^ -> number^ { return^ 2 } }\n";

    LHAT_TEST("composition by name carries both sides");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", parts,
                 "var^ D = A .. B\n"
                 "var^ p : number^ = D.a()\n"
                 "var^ q : number^ = D.b()\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and an instance of it holds both templates");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", parts,
                 "var^ D = A .. B\n"
                 "var^ o = D.new()\n"
                 "var^ p : number^ = o.x\n"
                 "var^ q : string^ = o.y\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A chain mixing both forms is what 14.2 lets the compiler settle, so the
    // checker has to reach the same answer.
    LHAT_TEST("a chain of names ending in a literal carries all of them");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", parts,
                 "var^ D = A .. B .. def^{ self^{ z := true^ },\n"
                 "                         c := f^self^ -> number^ { return^ 3 } }\n"
                 "var^ o = D.new()\n"
                 "var^ p : number^ = o.x\n"
                 "var^ q : string^ = o.y\n"
                 "var^ r : bool^ = o.z\n"
                 "var^ s : number^ = o.c()\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.5改: neither side of a composition by name was written against the
    // other, so neither is the answer under a name they share. The
    // composition stands -- what it costs is that the name is no longer
    // reachable through it.
    LHAT_TEST("a member both sides carry does not stop the composition");
    check_text(&u,
               "var^ A = def^{ self^{}, m := f^ -> number^ { return^ 1 } }\n"
               "var^ B = def^{ self^{}, m := f^ -> number^ { return^ 2 } }\n"
               "var^ D = A .. B\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and reading it through the composition is reported");
    check_text(&u,
               "var^ A = def^{ self^{}, m := f^self^ -> number^ { return^ 1 } }\n"
               "var^ B = def^{ self^{}, m := f^self^ -> number^ { return^ 2 } }\n"
               "var^ D = A .. B\n"
               "var^ r : number^ = D.new().m()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_AMBIGUOUS_MEMBER);
    unit_dispose(&u);

    // 14.4 is the way out, and it wanted nothing added: a method taken from
    // the side that wrote it applies to whatever fits it structurally.
    LHAT_TEST("but naming the side reaches either one");
    check_text(&u,
               "var^ A = def^{ self^{}, m := f^self^ -> number^ { return^ 1 } }\n"
               "var^ B = def^{ self^{}, m := f^self^ -> number^ { return^ 2 } }\n"
               "var^ D = A .. B\n"
               "var^ fromA = A.m\n"
               "var^ fromB = B.m\n"
               "var^ o = D.new()\n"
               "var^ x : number^ = fromA(o)\n"
               "var^ y : number^ = fromB(o)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the names only one side carries are untouched");
    check_text(&u,
               "var^ A = def^{ self^{}, m := f^self^ -> number^ { return^ 1 },\n"
               "  only := f^self^ -> number^ { return^ 9 } }\n"
               "var^ B = def^{ self^{}, m := f^self^ -> number^ { return^ 2 } }\n"
               "var^ D = A .. B\n"
               "var^ r : number^ = D.new().only()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.5改: a field is the one that stays an error at the '..'. A method is
    // shared, so either side's is still reachable through it; a field is
    // per-instance and the flattened table holds one, so there is no
    // qualified form to fall back to.
    LHAT_TEST("a field both templates carry is still reported at the '..'");
    check_text(&u,
               "var^ A = def^{ self^{ v := 0 } }\n"
               "var^ B = def^{ self^{ v := \"x\" } }\n"
               "var^ D = A .. B\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_COMPOSE_COLLIDES);
    unit_dispose(&u);

    // 14.15 is the way to write a mixin that does not own the field it uses,
    // and two of them compose without ever colliding.
    LHAT_TEST("and two mixins that declare rather than own do not collide");
    check_text(&u,
               "var^ A = def^{ self^{ abstract^ v : number^ },\n"
               "  a := f^self^ -> number^ { return^ self^.v } }\n"
               "var^ B = def^{ self^{ abstract^ v : number^ },\n"
               "  b := f^self^ -> number^ { return^ self^.v } }\n"
               "var^ Host = def^{ self^{ v := 3 } }\n"
               "var^ D = Host .. A .. B\n"
               "var^ r : number^ = D.new().a()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.12改: super^ names what an override^ is writing over, so it is a
    // name only there. Nothing else in a def^ hid anything.
    LHAT_TEST("super^ is a name inside an override^");
    check_text(&u,
               "var^ A = def^{ self^{}, m := f^ -> number^ { return^ 1 } }\n"
               "var^ D = A .. def^{ self^{},\n"
               "  override^ m := f^ -> number^ { return^ super^() + 1 } }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and not in a member with no marker");
    check_text(&u,
               "var^ A = def^{ self^{}, m := f^ -> number^ { return^ super^() } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_SUPER_OUTSIDE);
    unit_dispose(&u);

    LHAT_TEST("and not outside a def^ at all");
    check_text(&u, "var^ x = super^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_SUPER_OUTSIDE);
    unit_dispose(&u);

    // 14.4: super^(…) is the bound form, so the receiver is not written; taken
    // as a value it is spelled out, the way a method taken from under the dot
    // is.
    LHAT_TEST("super^ called directly takes the receiver on its own");
    check_text(&u,
               "var^ A = def^{ self^{ n := 0 },\n"
               "  get := f^self^ -> number^ { return^ self^.n } }\n"
               "var^ D = A .. def^{ self^{},\n"
               "  override^ get := f^self^ -> number^ { return^ super^() } }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and wants it written once taken as a value");
    check_text(&u,
               "var^ A = def^{ self^{ n := 0 },\n"
               "  get := f^self^ -> number^ { return^ self^.n } }\n"
               "var^ D = A .. def^{ self^{},\n"
               "  override^ get := f^self^ -> number^ {\n"
               "    var^ old = super^\n"
               "    return^ old(self^)\n"
               "  } }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.15: abstract^ declares a member and leaves it for the composition,
    // which is what lets a mixin call something it does not itself provide.
    LHAT_TEST("abstract^ is filled in by a composition");
    check_text(&u,
               "var^ Counting = def^{\n"
               "    self^{ count := 0 },\n"
               "    abstract^ step : f^ -> number^;,\n"
               "    bump := p^self^ { self^.count := self^.count + class^.step() },\n"
               "}\n"
               "var^ Fast = Counting .. def^{\n"
               "    self^{},\n"
               "    step := f^ -> number^ { return^ 10 },\n"
               "}\n"
               "var^ o = Fast.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.11: an instance carries a value under every name, so one still only
    // declared has nothing to make.
    LHAT_TEST("and a definition still holding one cannot be instantiated");
    check_text(&u,
               "var^ Counting = def^{ self^{}, abstract^ step : f^ -> number^; }\n"
               "var^ o = Counting.new()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_STILL_ABSTRACT);
    unit_dispose(&u);

    // 14.15改2: what an abstract^ asks for is what this def^ does not have.
    // Writing the value here as well leaves it saying nothing -- and 14.7改
    // took away the other reason anyone wrote one, which was to put the name
    // in front of a body that reaches it.
    LHAT_TEST("an abstract^ this def^ also provides is refused");
    check_text(&u,
               "var^ A = def^{ self^{},\n"
               "  abstract^ m : f^ -> number^;,\n"
               "  m := f^ -> number^ { return^ 1 },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ABSTRACT_PROVIDED_HERE);
    unit_dispose(&u);

    LHAT_TEST("and the other order is the same mistake");
    check_text(&u,
               "var^ A = def^{ self^{},\n"
               "  m := f^ -> number^ { return^ 1 },\n"
               "  abstract^ m : f^ -> number^;,\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ABSTRACT_PROVIDED_HERE);
    unit_dispose(&u);

    // A declaration is not a definition of the member, so filling it in is
    // not 14.12's collision and wants no marker.
    LHAT_TEST("filling an abstract^ in needs no marker");
    check_text(&u,
               "var^ A = def^{ self^{}, abstract^ m : f^ -> number^; }\n"
               "var^ B = A .. def^{ self^{},\n"
               "  m := f^ -> number^ { return^ 1 } }\n"
               "var^ o = B.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and refuses one, since nothing was replaced");
    check_text(&u,
               "var^ A = def^{ self^{}, abstract^ m : f^ -> number^; }\n"
               "var^ B = A .. def^{ self^{},\n"
               "  override^ m := f^ -> number^ { return^ 1 } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE);
    unit_dispose(&u);

    // What the declaration does ask is that the value fit the type it wrote.
    LHAT_TEST("what fills an abstract^ has to fit the type it declared");
    check_text(&u,
               "var^ A = def^{ self^{}, abstract^ m : f^ -> number^; }\n"
               "var^ B = A .. def^{ self^{},\n"
               "  m := f^ -> string^ { return^ \"x\" } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("declaring what is already provided is reported");
    check_text(&u,
               "var^ A = def^{ self^{}, m := f^ -> number^ { return^ 1 } }\n"
               "var^ B = A .. def^{ self^{}, abstract^ m : f^ -> number^; }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ALREADY_PROVIDED);
    unit_dispose(&u);

    // 14.6: the template takes one too, which is what lets a mixin reach a
    // field through self^ without owning it.
    LHAT_TEST("a template field may be abstract^ as well");
    check_text(&u,
               "var^ Greet = def^{\n"
               "    self^{ abstract^ n : number^ },\n"
               "    hello := f^self^ -> number^ { return^ self^.n + 1 },\n"
               "}\n"
               "var^ Thing = Greet .. def^{ self^{ n := 10 } }\n"
               "var^ r : number^ = Thing.new().hello()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.15改3 names a field's own way out, so it is told apart from a
    // member's -- a written new could have given it a value, and none was.
    LHAT_TEST("and one left unfilled stops the construction too");
    check_text(&u,
               "var^ Greet = def^{ self^{ abstract^ n : number^ } }\n"
               "var^ o = Greet.new()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FIELD_UNPROVIDED);
    unit_dispose(&u);

    // 14.5: the two sides of a composition by name pair up the same way --
    // one declaring what the other provides is the point, not a collision.
    LHAT_TEST("a declaration and a definition compose by name");
    check_text(&u,
               "var^ Need = def^{ self^{}, abstract^ m : f^self^ -> number^; }\n"
               "var^ Give = def^{ self^{}, m := f^self^ -> number^ { return^ 7 } }\n"
               "var^ D = Need .. Give\n"
               "var^ r : number^ = D.new().m()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.15改: with the two together, a mixin written against no base at all
    // can replace a member and call what it replaced. abstract^ says what the
    // host must hold, the pending override^ says what it must provide.
    LHAT_TEST("a mixin written against no base composes onto one");
    check_text(&u,
               "var^ Base = def^{ self^{ n := 0 },\n"
               "  run := p^self^ { self^.n := self^.n + 1 } }\n"
               "var^ Logged = def^{ self^{ abstract^ n : number^ },\n"
               "  override^ run := p^self^ { self^.n := self^.n + 10\n"
               "                             super^() } }\n"
               "var^ App = Base .. Logged\n"
               "var^ o = App.new()\n"
               "o.run()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and two of them stack");
    check_text(&u,
               "var^ Base = def^{ self^{ n := 0 },\n"
               "  run := p^self^ { self^.n := self^.n + 1 } }\n"
               "var^ A = def^{ self^{ abstract^ n : number^ },\n"
               "  override^ run := p^self^ { super^() } }\n"
               "var^ B = def^{ self^{ abstract^ n : number^ },\n"
               "  override^ run := p^self^ { super^() } }\n"
               "var^ App = Base .. A .. B\n"
               "var^ o = App.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // Stacking two settles neither -- the chain still wants something under
    // them both, so new stays out of reach.
    LHAT_TEST("but stacking two does not settle either");
    check_text(&u,
               "var^ A = def^{ self^{}, override^ run := p^self^ { super^() } }\n"
               "var^ B = def^{ self^{}, override^ run := p^self^ { super^() } }\n"
               "var^ X = A .. B\n"
               "var^ o = X.new()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_STILL_ABSTRACT);
    unit_dispose(&u);

    // 14.12's check is the one the def^ would have had. It runs where the
    // two finally meet instead.
    LHAT_TEST("substitutability is checked where the two meet");
    check_text(&u,
               "var^ Base = def^{ self^{}, run := f^ -> number^ { return^ 1 } }\n"
               "var^ Bad = def^{ self^{},\n"
               "  override^ run := f^ -> string^ { return^ \"x\" } }\n"
               "var^ App = Base .. Bad\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_SUBSTITUTABLE);
    unit_dispose(&u);

    // 14.11 gives every definition a new whether or not one was written, so
    // the two always carry that name. It is rebuilt rather than collided.
    LHAT_TEST("the synthesised new is not a collision");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", parts, "var^ D = A .. B\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

// 02 の 14.12改: typeof^'s own static type is a fixed nominal carrier
// (TypeInfo) regardless of the operand -- the descriptive payload is a
// runtime concern (03 の 4.2), so the checker's job is only to give
// '.signature' somewhere to resolve and to still check the operand.
static void test_typeof(void)
{
    Unit u;

    LHAT_TEST("typeof^(x).signature is a string^");
    check_text(&u, "var^ s : string^ = typeof^(5).signature\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("an unknown member is refused");
    check_text(&u, "var^ x = typeof^(5).bogus\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    LHAT_TEST("the operand is still checked");
    check_text(&u, "var^ x = typeof^(nowhere)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
    unit_dispose(&u);

    // 02 の 2811: typeof^(x) = typeof^(y) has to type-check at all, which
    // needs both sides to resolve to the very same TypeInfo type.
    LHAT_TEST("two typeof^ results compare with '='");
    check_text(&u, "var^ b : bool^ = typeof^(5) = typeof^(\"x\")\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 05 の 8.8: a nominal type carries what made it, and nothing written in
    // L^ replaces that. The same mark is what a registered host type carries
    // (program.c), so both are refused by this one rule -- adding a member is
    // already refused by 02 の 8.8's own mark, and this is the other half.
    LHAT_TEST("what a nominal type carries cannot be written over");
    check_text(&u,
               "var^ t = typeof^(5)\n"
               "t.signature := \"x\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PATH_IS_OPAQUE);
    unit_dispose(&u);

    // 14 章's fields are what a def^ instance is for, and it is not nominal.
    LHAT_TEST("but a def^ instance's fields still are");
    check_text(&u,
               "var^ P = def^{ self^{ x := 0 } }\n"
               "var^ o = P.new()\n"
               "o.x := 1\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // Session-scoped the way L^'s environment is (05 の 8.6), so a typeof^
    // bound in one input is still the same nominal type in the next.
    LHAT_TEST("the TypeInfo type is the same across a session's inputs");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_next_text(&u, s, "var^ t = typeof^(5)\n");
        CHECK_CLEAN(&u);
        unit_dispose(&u);
        check_next_text(&u, s, "var^ b : bool^ = t = typeof^(\"x\")\n");
        CHECK_CLEAN(&u);
        unit_dispose(&u);
        lhat_check_session_dispose(s);
    }
}


// 14.6 with 8.6: a template field may write its type as well as its value.
// What it holds is then what was written -- the same reading a let^ target
// has, and the reason is the same: a reader relies on what was written, not
// on what the first value happened to be.
static void test_field_types(void)
{
    Unit u;

    LHAT_TEST("a field may be written with a type and a value");
    check_text(&u,
               "let^ D = def^{\n"
               "  self^{ hp : number^ = 50, name : string^ = \"\" },\n"
               "}\n"
               "let^ d = D.new()\n"
               "let^ n : number^ = d.hp\n"
               "let^ s : string^ = d.name\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("the written type is what the field holds, not the value's");
    check_text(&u,
               "let^ D = def^{\n"
               "  self^{ slot : any^ = 1 },\n"
               "  widen = p^self^ { self^.slot := \"text\" },\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // Without one the field is as narrow as its initialiser, which is what
    // makes the annotation worth writing.
    LHAT_TEST("where an unwritten one is as narrow as its value");
    check_text(&u,
               "let^ D = def^{\n"
               "  self^{ slot = 1 },\n"
               "  widen = p^self^ { self^.slot := \"text\" },\n"
               "}\n");
    LHAT_CHECK(u.checked.diagnostic_count > 0, "reported");
    unit_dispose(&u);

    LHAT_TEST("and the value has to fit it");
    check_text(&u,
               "let^ D = def^{\n"
               "  self^{ hp : string^ = 50 },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 14.15 is the other half: a type and no value at all, which says the
    // composition owes one rather than saying what this one holds.
    LHAT_TEST("abstract^ is still the form with no value");
    check_text(&u,
               "let^ D = def^{\n"
               "  self^{ abstract^ hp : number^ },\n"
               "}\n"
               "let^ d = D.new()\n");
    LHAT_CHECK(u.checked.diagnostic_count > 0, "reported");
    unit_dispose(&u);
}

// 14.15改3: a template field is one an instance holds, and 14.11 builds an
// instance in a written new -- so a new that writes the field gives it a
// value, exactly as a composition's initialiser would. 14.12改2 leaves an
// override^ new the only way to build one, which is what makes reading it
// enough.
static void test_new_fills_fields(void)
{
    Unit u;

    LHAT_TEST("an override^ new writing the field is what provides it");
    check_text(&u,
               "let^ D = def^{\n"
               "  self^{ abstract^ slot : number^ },\n"
               "  override^new = f^v:number^ { self^{ slot = v } },\n"
               "  read = f^self^ -> number^ { return^ self^.slot },\n"
               "}\n"
               "let^ n : number^ = D.new(1).read()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and what is composed onto it is built the same way");
    check_text(&u,
               "let^ D = def^{\n"
               "  self^{ abstract^ slot : number^ },\n"
               "  override^new = f^v:number^ { self^{ slot = v } },\n"
               "}\n"
               "let^ E = D..def^{\n"
               "  self^{ extra = 0 },\n"
               "  twice = f^self^ -> number^ { return^ self^.slot * 2 },\n"
               "}\n"
               "let^ n : number^ = E.new(21).twice()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The hole this closes was open in both directions before: the field went
    // unwritten and nothing said so.
    LHAT_TEST("but a new that does not write it leaves the hole");
    check_text(&u,
               "let^ D = def^{\n"
               "  self^{ abstract^ slot : number^ },\n"
               "  override^new = f^ { self^{} },\n"
               "}\n"
               "let^ d = D.new()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FIELD_UNPROVIDED);
    unit_dispose(&u);

    // 14.12改2 exempts only override^. An overload^ keeps the default new()
    // beside the written one, and that arm writes nothing.
    LHAT_TEST("and an overload^ leaves the default arm to leave it too");
    check_text(&u,
               "let^ D = def^{\n"
               "  self^{ abstract^ slot : number^ },\n"
               "  overload^new = f^v:number^ { self^{ slot = v } },\n"
               "}\n"
               "let^ d = D.new(1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FIELD_UNPROVIDED);
    unit_dispose(&u);

    // 14.15's own case is untouched: new cannot define a member, so only a
    // composition settles one.
    LHAT_TEST("a member's declaration is still a composition's to fill");
    check_text(&u,
               "let^ D = def^{\n"
               "  self^{},\n"
               "  abstract^ step : f^ -> number^;,\n"
               "  override^new = f^ { self^{} },\n"
               "}\n"
               "let^ d = D.new()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_STILL_ABSTRACT);
    unit_dispose(&u);

    // 14.15改's wait is for super^'s other half, not for a value.
    LHAT_TEST("nor does it settle an override^ with nothing to replace");
    check_text(&u,
               "let^ D = def^{\n"
               "  self^{ n = 0 },\n"
               "  override^run = p^self^ { self^.n += 1 },\n"
               "  override^new = f^ { self^{ n = 1 } },\n"
               "}\n"
               "let^ d = D.new()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_STILL_ABSTRACT);
    unit_dispose(&u);

    // Any of the self^{ … } written may be the one that runs, so a field is
    // provided only where every one of them writes it.
    LHAT_TEST("a field written on one path only is not provided");
    check_text(&u,
               "let^ D = def^{\n"
               "  self^{ abstract^ x : number^, abstract^ y : number^ },\n"
               "  override^new = f^ v:number^ {\n"
               "    self^{ x = 0 }\n"
               "    if^ v > 0 { self^{ x = v, y = v } }\n"
               "  },\n"
               "}\n"
               "let^ d = D.new(1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FIELD_UNPROVIDED);
    unit_dispose(&u);

    // A self^{ … } inside a def^ written in the body builds that one's
    // instance, and says nothing about this one's fields.
    LHAT_TEST("and a self^{ } belonging to some other def^ does not count");
    check_text(&u,
               "let^ D = def^{\n"
               "  self^{ abstract^ slot : number^ },\n"
               "  override^new = f^ {\n"
               "    let^ Inner = def^{\n"
               "      self^{ slot = 0 },\n"
               "      override^new = f^ { self^{ slot = 1 } },\n"
               "    }\n"
               "    self^{}\n"
               "  },\n"
               "}\n"
               "let^ d = D.new()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FIELD_UNPROVIDED);
    unit_dispose(&u);
}

// 14.11: the prototype is a value -- the table a definition hangs under
// self^, holding every field's default, evaluated once as the definition is
// built. Construction copies it shallowly, which is what the rules here
// protect: only immutable defaults may sit on it, nothing writes through it,
// and a new body answers nothing of its own.
static void test_prototype(void)
{
    Unit u;

    // A table default is a literal tree: each level born fresh at this very
    // expression, so the copies construction hands out share nothing.
    LHAT_TEST("a table literal default is each instance's own");
    check_text(&u,
               "let^ D = def^{ self^{ items = { } } }\n"
               "let^ d = D.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a nested literal tree checks clean");
    check_text(&u,
               "let^ D = def^{ self^{ grid = { { 0, 0 }, { 0, 0 } } } }\n"
               "let^ d = D.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A table that arrived any other way is an identity from somewhere
    // else, which is what new is for.
    LHAT_TEST("a name standing where a default goes is refused");
    check_text(&u,
               "let^ shared = { }\n"
               "let^ D = def^{ self^{ items = shared } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MUTABLE_DEFAULT);
    unit_dispose(&u);

    LHAT_TEST("and so is a call's answer inside a literal");
    check_text(&u,
               "let^ make = f^ -> t^{} { return^ { } }\n"
               "let^ D = def^{ self^{ box = { inner = make() } } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MUTABLE_DEFAULT);
    unit_dispose(&u);

    LHAT_TEST("a definition is a shared leaf");
    check_text(&u,
               "let^ S = def^{ tag = f^ -> number^ { return^ 1 } }\n"
               "let^ D = def^{ self^{ strategy = S } }\n"
               "let^ d = D.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and so is a call answering one");
    check_text(&u,
               "let^ make = f^ -> t^{} { return^ { } }\n"
               "let^ D = def^{ self^{ items = make() } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MUTABLE_DEFAULT);
    unit_dispose(&u);

    // The value's own type is what is judged, not the field's width.
    LHAT_TEST("an immutable value under a wide type is fine");
    check_text(&u,
               "let^ D = def^{ self^{ slot : any^ = 1 } }\n"
               "let^ d = D.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a call answering a number is a value like any other");
    check_text(&u,
               "let^ pick = f^ -> number^ { return^ 3 }\n"
               "let^ D = def^{ self^{ n = pick() } }\n"
               "let^ x : number^ = D.new().n\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a return^ in a new body is refused");
    check_text(&u,
               "let^ D = def^{\n"
               "  self^{ n = 0 },\n"
               "  override^new = f^ {\n"
               "    self^{ n = 1 }\n"
               "    return^ self^\n"
               "  },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NEW_RETURNS);
    unit_dispose(&u);

    LHAT_TEST("the prototype takes no writes");
    check_text(&u,
               "let^ D = def^{ self^{ n = 0 } }\n"
               "D.self^.n := 5\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PROTOTYPE_SEALED);
    unit_dispose(&u);

    LHAT_TEST("and is not replaced whole either");
    check_text(&u,
               "let^ D = def^{ self^{ n = 0 } }\n"
               "D.self^ := { n = 1 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PROTOTYPE_SEALED);
    unit_dispose(&u);

    // However deep the path goes, and through an index as well as a member.
    LHAT_TEST("nothing inside it is written through it either");
    check_text(&u,
               "let^ D = def^{ self^{ used = { 0, 0 } } }\n"
               "D.self^.used[1] := 9\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PROTOTYPE_SEALED);
    unit_dispose(&u);

    // 14.7改: the member is the definition's own; what it answers is the
    // instance type, since the prototype is one canonical instance.
    LHAT_TEST("reading it is reading an instance");
    check_text(&u,
               "let^ D = def^{ self^{ n = 0 } }\n"
               "let^ x : number^ = D.self^.n\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.1's exception: the copy is the body's own -- nothing outside holds
    // it yet -- so an f^ new writes it directly as well as through the
    // batch form.
    LHAT_TEST("a new body writes self^ from inside an f^");
    check_text(&u,
               "let^ D = def^{\n"
               "  self^{ n = 0 },\n"
               "  override^new = f^ {\n"
               "    self^.n := 5\n"
               "  },\n"
               "}\n"
               "let^ d = D.new()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

int main(void)
{
    test_definitions();
    test_composition();
    test_typeof();
    test_field_types();
    test_new_fills_fields();
    test_prototype();
    return lhat_test_report("test_check_def");
}
