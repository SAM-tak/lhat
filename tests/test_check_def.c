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

    // 14.11: a definition without one still offers a new taking nothing.
    LHAT_TEST("new exists without being written");
    check_text(&u, "var^ C = def^{ self^{ v := 1 } }\nvar^ c = C.new()\n");
    CHECK_CLEAN(&u);
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
                 "                         c := f^ -> number^ { return^ 3 } }\n"
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
               "var^ A = def^{ self^{}, m := f^ -> number^ { return^ 1 } }\n"
               "var^ B = def^{ self^{}, m := f^ -> number^ { return^ 2 } }\n"
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
               "var^ A = def^{ self^{}, m := f^ -> number^ { return^ 1 },\n"
               "  only := f^ -> number^ { return^ 9 } }\n"
               "var^ B = def^{ self^{}, m := f^ -> number^ { return^ 2 } }\n"
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

    LHAT_TEST("and one left unfilled stops the construction too");
    check_text(&u,
               "var^ Greet = def^{ self^{ abstract^ n : number^ } }\n"
               "var^ o = Greet.new()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_STILL_ABSTRACT);
    unit_dispose(&u);

    // 14.5: the two sides of a composition by name pair up the same way --
    // one declaring what the other provides is the point, not a collision.
    LHAT_TEST("a declaration and a definition compose by name");
    check_text(&u,
               "var^ Need = def^{ self^{}, abstract^ m : f^ -> number^; }\n"
               "var^ Give = def^{ self^{}, m := f^ -> number^ { return^ 7 } }\n"
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

int main(void)
{
    test_definitions();
    test_composition();
    test_typeof();
    return lhat_test_report("test_check_def");
}
