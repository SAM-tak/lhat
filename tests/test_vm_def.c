// L^ (lhat) -- tests for the bytecode and the machine.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed. Programs are run end to end, since what is worth pinning is the
// answer rather than the instructions chosen to reach it -- 5.1 expects those
// to be replaced by specialised ones later.

#include <math.h>
#include <string.h>

#include "code.h"
#include "fixture.h"

// 02 の 14 章: def^ is the one mechanism for a user-defined type.
static void test_definitions(void)
{
    Run r;

    // 14.11: without a new of its own, a definition gets one taking no
    // arguments that answers what the template says.
    LHAT_TEST("the default new builds an instance from the template");
    run_text(&r,
             "var^ Foo = def^{ self^{ a := 1, b := 2 } }\n"
             "var^ f = Foo.new()\n"
             "return^ f.a * 10 + f.b\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    // 14.3: a method belongs to the definition and is shared; a field belongs
    // to the instance and is copied.
    LHAT_TEST("two instances have their own fields");
    run_text(&r,
             "var^ Foo = def^{ self^{ n := 0 } }\n"
             "var^ a = Foo.new()\n"
             "var^ b = Foo.new()\n"
             "a.n := 5\n"
             "return^ b.n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 14.11: a table written out as a literal is a tree of the instance's
    // own -- construction copies it, so two instances never share one.
    // Python's mutable default argument has no counterpart here.
    LHAT_TEST("a literal table default is each instance's own");
    run_text(&r,
             "var^ Foo = def^{ self^{ items := { } } }\n"
             "var^ a = Foo.new()\n"
             "var^ b = Foo.new()\n"
             "a.items[1] := 9\n"
             "return^ b.items[1] ?? 0\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    LHAT_TEST("and a nested one is copied all the way down");
    run_text(&r,
             "var^ Foo = def^{ self^{ grid := { { 0 }, { 0 } } } }\n"
             "var^ a = Foo.new()\n"
             "var^ b = Foo.new()\n"
             "a.grid[1][1] := 9\n"
             "return^ b.grid[1][1] ?? 0\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 14.11: a definition among the defaults is a public identity, not
    // per-instance data -- the copy leaves it shared.
    LHAT_TEST("a definition among the defaults is shared, not copied");
    run_text(&r,
             "var^ S = def^{ tag := f^ { return^ 1 } }\n"
             "var^ Foo = def^{ self^{ strategy := S } }\n"
             "var^ a = Foo.new()\n"
             "var^ b = Foo.new()\n"
             "return^ a.strategy is^ b.strategy\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // 14.11: a value nothing may share -- a coroutine here -- is refused
    // where the values land (SETPROTO).
    LHAT_TEST("a default nothing may share is refused");
    run_text(&r,
             "var^ C = p^ { yield^ }\n"
             "var^ Foo = def^{ self^{ w := C() } }\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_MUTABLE_DEFAULT);
    run_dispose(&r);

    // 14.11: the seal reaches the whole tree, not only the top table.
    LHAT_TEST("the prototype's inner tables take no writes either");
    run_text(&r,
             "var^ Foo = def^{ self^{ used := { 0, 0 } } }\n"
             "Foo.self^.used[1] := 9\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_SEALED);
    run_dispose(&r);

    // 14.11: baking copies -- a table an initialiser handed over becomes the
    // prototype's own tree, and the original stays the writer's, unsealed.
    LHAT_TEST("what the prototype holds is its own copy");
    run_text(&r,
             "var^ shared = { 5 }\n"
             "var^ Foo = def^{ self^{ items := shared } }\n"
             "shared[1] := 7\n"
             "return^ Foo.new().items[1] * 10 + shared[1]\n");
    CHECK_INTEGER(&r, 57);
    run_dispose(&r);

    // 14.11: the spelling for one -- declare the field and make the value
    // inside new, where it is made per construction.
    LHAT_TEST("a table made inside new is each instance's own");
    run_text(&r,
             "var^ Foo = def^{\n"
             "  self^{ abstract^items : t^{} },\n"
             "  override^new := f^ { self^{ items := { } } },\n"
             "}\n"
             "var^ a = Foo.new()\n"
             "var^ b = Foo.new()\n"
             "a.items[1] := 9\n"
             "return^ b.items[1] ?? 0\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 14.4: the shape of the signature says it is a method. No modifier does.
    LHAT_TEST("a method gets the receiver as its self^");
    run_text(&r,
             "var^ Foo = def^{\n"
             "  self^{ n := 7 },\n"
             "  get := f^self^ { return^ self^.n },\n"
             "}\n"
             "return^ Foo.new().get()\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("a method takes arguments after the receiver");
    run_text(&r,
             "var^ Foo = def^{\n"
             "  self^{ n := 1 },\n"
             "  add := f^self^, x { return^ self^.n + x },\n"
             "}\n"
             "return^ Foo.new().add(4)\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    LHAT_TEST("a method may change the instance");
    run_text(&r,
             "var^ Foo = def^{\n"
             "  self^{ n := 0 },\n"
             "  bump := p^self^ { self^.n := self^.n + 1 },\n"
             "}\n"
             "var^ f = Foo.new()\n"
             "f.bump()\n"
             "f.bump()\n"
             "return^ f.n\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    // 14.4: taking the method out and passing the receiver by hand is the
    // same call written differently.
    LHAT_TEST("a method taken out is called with the receiver by hand");
    run_text(&r,
             "var^ Foo = def^{\n"
             "  self^{ n := 3 },\n"
             "  get := f^self^ { return^ self^.n },\n"
             "}\n"
             "var^ f = Foo.new()\n"
             "var^ g = f.get\n"
             "return^ g(f)\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 14.4: a member with no self^ is a static one, so the receiver is not
    // passed to it.
    LHAT_TEST("a member without self^ is static");
    run_text(&r,
             "var^ Foo = def^{ self^{ }, make := f^x { return^ x * 2 } }\n"
             "return^ Foo.make(21)\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    // 14.7改: what an instance reaches through its definition is what takes a
    // receiver. A static member is the definition's own.
    LHAT_TEST("an instance reaches a member that takes a receiver");
    run_text(&r,
             "var^ Foo = def^{ self^{ }, tag := f^self^ { return^ 9 } }\n"
             "return^ Foo.new().tag()\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    LHAT_TEST("and not a static one, which is nothing there");
    run_text(&r,
             "var^ Foo = def^{ self^{ }, tag := f^ { return^ 9 } }\n"
             "return^ Foo.new().tag()\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_NOT_CALLABLE);
    run_dispose(&r);

    LHAT_TEST("while the definition reaches it as it always did");
    run_text(&r,
             "var^ Foo = def^{ self^{ }, tag := f^ { return^ 9 } }\n"
             "return^ Foo.tag()\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    // 14.11: new is the definition's, so an instance is not another way to
    // make one -- which is what asking a value for its own maker would be.
    LHAT_TEST("and new is out of an instance's reach");
    run_text(&r,
             "var^ Foo = def^{ self^{ a := 1 } }\n"
             "return^ Foo.new().new()\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_NOT_CALLABLE);
    run_dispose(&r);

    // 14.11: new overwrites what it names; the rest keep the copy's defaults.
    LHAT_TEST("new fills what it names and the copy keeps the rest");
    run_text(&r,
             "var^ Foo = def^{\n"
             "  self^{ a := 1, b := 2 },\n"
             "  override^new := f^v { self^{ a := v } },\n"
             "}\n"
             "var^ f = Foo.new(8)\n"
             "return^ f.a * 10 + f.b\n");
    CHECK_INTEGER(&r, 82);
    run_dispose(&r);

    // 14.11: an initialiser runs once, as the definition is built -- its
    // value is baked onto the prototype, and construction copies rather
    // than evaluates.
    LHAT_TEST("an initialiser runs once, at the definition");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ side = f^ { log.n := log.n + 1 return^ 0 }\n"
             "var^ Foo = def^{\n"
             "  self^{ a := side() },\n"
             "  override^new := f^ { self^{ a := 5 } },\n"
             "}\n"
             "var^ f = Foo.new()\n"
             "var^ g = Foo.new()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 14.11改 with 14.12: the default new is a real member from the start, so
    // an overload^ adds an arm beside it and both spellings construct. Before
    // this, writing one took the default away.
    LHAT_TEST("an overload^ new stands beside the default");
    run_checked_text(&r,
                     "var^ Foo = def^{\n"
                     "  self^{ a := 1, b := 2 },\n"
                     "  overload^new := f^v:number^ {\n"
                     "    self^{ a := v }\n"
                     "  },\n"
                     "}\n"
                     "return^ Foo.new().a * 10 + Foo.new(7).a\n");
    CHECK_INTEGER(&r, 17);
    run_dispose(&r);

    // 14.12改 with 14.11: super^ inside an override^ new is the hook of the
    // new written before it, run against the same instance. With none
    // written it does nothing, and the copy is what the body starts from.
    LHAT_TEST("super^ inside an override^ new reaches the default hook");
    run_checked_text(&r,
                     "var^ Foo = def^{\n"
                     "  self^{ a := 4 },\n"
                     "  override^new := f^ { super^() },\n"
                     "}\n"
                     "return^ Foo.new().a\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);

    // 14.12改: and down a composition it is the previous part's new, so a
    // derived new runs the base's adjustments before its own -- against the
    // one instance the machine made.
    LHAT_TEST("super^ chains the hooks against one instance");
    run_checked_text(&r,
                     "var^ A = def^{\n"
                     "  self^{ trail := \"\" },\n"
                     "  override^new := f^ { self^{ trail := \"a\" } },\n"
                     "}\n"
                     "var^ B = A..def^{\n"
                     "  override^new := f^ {\n"
                     "    super^()\n"
                     "    self^{ trail := self^.trail .. \"b\" }\n"
                     "  },\n"
                     "}\n"
                     "var^ C = B..def^{\n"
                     "  override^new := f^ {\n"
                     "    super^()\n"
                     "    self^{ trail := self^.trail .. \"c\" }\n"
                     "  },\n"
                     "}\n"
                     "return^ C.new().trail\n");
    CHECK_STRING(&r, "abc");
    run_dispose(&r);

    // 14.5 with 14.12: composing two written definitions rebuilds the
    // constructor so it answers with the composed instance. Every arm is
    // rebuilt -- an overloaded new used to lose all of them here, leaving a
    // composition that could only be constructed with no arguments.
    LHAT_TEST("composition keeps every arm of an overloaded new");
    run_checked_text(&r,
                     "var^ Left = def^{ self^{ b := 2 } }\n"
                     "var^ Right = def^{\n"
                     "  self^{ a := 1 },\n"
                     "  overload^new := f^v:number^ {\n"
                     "    self^{ a := v }\n"
                     "  },\n"
                     "}\n"
                     "var^ Both = Left .. Right\n"
                     "var^ x = Both.new(5)\n"
                     "return^ x.a * 10 + x.b\n");
    CHECK_INTEGER(&r, 52);
    run_dispose(&r);

    // 14.11: the prototype is a member like any other -- App.self^ reads it,
    // an instance does not (14.7改: it takes no receiver), and writing it
    // meets the seal.
    LHAT_TEST("a definition's self^ is the prototype, readable");
    run_text(&r,
             "var^ Foo = def^{ self^{ a := 1, b := 2 } }\n"
             "return^ Foo.self^.a * 10 + Foo.self^.b\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    LHAT_TEST("an instance does not see self^ through the link");
    run_text(&r,
             "var^ Foo = def^{ self^{ a := 1 } }\n"
             "return^ Foo.new().self^ is^ nil^\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("the prototype takes no writes");
    run_text(&r,
             "var^ Foo = def^{ self^{ a := 1 } }\n"
             "Foo.self^.a := 5\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_SEALED);
    run_dispose(&r);

    // 14.11: a written new adjusts the copy; the prototype it was copied
    // from keeps its defaults.
    LHAT_TEST("what new writes lands on the copy, not the prototype");
    run_text(&r,
             "var^ Foo = def^{\n"
             "  self^{ a := 1 },\n"
             "  override^new := f^v { self^{ a := v } },\n"
             "}\n"
             "var^ f = Foo.new(9)\n"
             "return^ Foo.self^.a * 10 + f.a\n");
    CHECK_INTEGER(&r, 19);
    run_dispose(&r);

    // 14.11: an initialiser cannot see self^, which does not exist yet, but
    // can see def^, which does.
    LHAT_TEST("an initialiser sees def^");
    run_text(&r,
             "var^ Foo = def^{\n"
             "  self^{ a := def^.base() },\n"
             "  base := f^ { return^ 6 },\n"
             "}\n"
             "return^ Foo.new().a\n");
    CHECK_INTEGER(&r, 6);
    run_dispose(&r);

    LHAT_TEST("a method sees def^ too");
    run_text(&r,
             "var^ Foo = def^{\n"
             "  self^{ },\n"
             "  base := f^ { return^ 4 },\n"
             "  get := f^self^ { return^ def^.base() },\n"
             "}\n"
             "return^ Foo.new().get()\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);

    // 14.5: composition is '..' and the order matters.
    LHAT_TEST("composition brings the base's members along");
    run_text(&r,
             "var^ Foo = def^{ self^{ a := 1 }, one := f^self^ { return^ 1 } }\n"
             "var^ Bar = Foo .. def^{ self^{ b := 2 }, two := f^self^ { return^ 2 } }\n"
             "var^ x = Bar.new()\n"
             "return^ x.a * 1000 + x.b * 100 + x.one() * 10 + x.two()\n");
    CHECK_INTEGER(&r, 1212);
    run_dispose(&r);

    // 14.12: override^ replaces, and the later part is what wins.
    LHAT_TEST("override^ replaces the member it names");
    run_text(&r,
             "var^ Foo = def^{ self^{ }, tag := f^self^ { return^ 1 } }\n"
             "var^ Bar = Foo .. def^{\n"
             "  self^{ },\n"
             "  override^\n"
             "  tag := f^self^ { return^ 2 },\n"
             "}\n"
             "return^ Bar.new().tag()\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    LHAT_TEST("and the base keeps its own");
    run_text(&r,
             "var^ Foo = def^{ self^{ }, tag := f^self^ { return^ 1 } }\n"
             "var^ Bar = Foo .. def^{\n"
             "  self^{ },\n"
             "  override^\n"
             "  tag := f^self^ { return^ 2 },\n"
             "}\n"
             "return^ Foo.new().tag()\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 14.12改: super^ is what the override^ wrote over. The parts are walked
    // in order, so it is read out of the table before the write.
    LHAT_TEST("super^ reaches the definition an override^ replaced");
    run_text(&r,
             "var^ Foo = def^{ self^{ }, tag := f^self^ { return^ 1 } }\n"
             "var^ Bar = Foo .. def^{\n"
             "  self^{ },\n"
             "  override^ tag := f^self^ { return^ super^() + 100 },\n"
             "}\n"
             "return^ Bar.new().tag()\n");
    CHECK_INTEGER(&r, 101);
    run_dispose(&r);

    // Each link sees the one before it, since each is written over what the
    // parts up to that point had built.
    LHAT_TEST("and each link of a chain reaches its own");
    run_text(&r,
             "var^ A = def^{ self^{ }, m := f^self^ { return^ 1 } }\n"
             "var^ B = A .. def^{ self^{ },\n"
             "  override^ m := f^self^ { return^ super^() + 10 } }\n"
             "var^ C = B .. def^{ self^{ },\n"
             "  override^ m := f^self^ { return^ super^() + 100 } }\n"
             "return^ C.new().m()\n");
    CHECK_INTEGER(&r, 111);
    run_dispose(&r);

    // 14.4: the receiver of super^(…) is the one the body already holds, so
    // it is not written. This one reads a field through it.
    LHAT_TEST("super^ takes the receiver without it being written");
    run_text(&r,
             "var^ Foo = def^{ self^{ n := 3 },\n"
             "  get := f^self^ -> number^ { return^ self^.n } }\n"
             "var^ Bar = Foo .. def^{ self^{ },\n"
             "  override^ get := f^self^ -> number^ { return^ super^() * 10 } }\n"
             "return^ Bar.new().get()\n");
    CHECK_INTEGER(&r, 30);
    run_dispose(&r);

    // 14.4 again: named on its own it is a value like any other, and then
    // the receiver is spelled out.
    LHAT_TEST("and spelled out once super^ is taken as a value");
    run_text(&r,
             "var^ Foo = def^{ self^{ n := 3 },\n"
             "  get := f^self^ -> number^ { return^ self^.n } }\n"
             "var^ Bar = Foo .. def^{ self^{ },\n"
             "  override^ get := f^self^ -> number^ {\n"
             "    var^ old = super^\n"
             "    return^ old(self^) + 1\n"
             "  } }\n"
             "return^ Bar.new().get()\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);

    // 14.12: an override^ over an overloaded name replaces the one arm it
    // overlaps, so a plain write would take the whole group with it.
    LHAT_TEST("override^ over an overload^ keeps the other arms");
    run_text(&r,
             "var^ A = def^{ self^{ }, m := f^self^ { return^ 1 } }\n"
             "var^ B = A .. def^{ self^{ },\n"
             "  overload^ m := f^self^, x:number^ { return^ x } }\n"
             "var^ C = B .. def^{ self^{ },\n"
             "  override^ m := f^self^ { return^ super^() + 100 } }\n"
             "return^ C.new().m(7)\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("and super^ there is the arm that was replaced");
    run_text(&r,
             "var^ A = def^{ self^{ }, m := f^self^ { return^ 1 } }\n"
             "var^ B = A .. def^{ self^{ },\n"
             "  overload^ m := f^self^, x:number^ { return^ x } }\n"
             "var^ C = B .. def^{ self^{ },\n"
             "  override^ m := f^self^ { return^ super^() + 100 } }\n"
             "return^ C.new().m()\n");
    CHECK_INTEGER(&r, 101);
    run_dispose(&r);

    // 14.15: a declaration puts nothing in the table, and what a composition
    // provides goes under the same name. The mixin's own body reaches it.
    LHAT_TEST("what fills an abstract^ is what the mixin calls");
    run_text(&r,
             "var^ Counting = def^{\n"
             "  self^{ count := 0 },\n"
             "  abstract^ step : f^ -> number^;,\n"
             "  bump := p^self^ { self^.count := self^.count + def^.step() },\n"
             "}\n"
             "var^ Fast = Counting .. def^{\n"
             "  self^{},\n"
             "  step := f^ -> number^ { return^ 10 },\n"
             "}\n"
             "var^ o = Fast.new()\n"
             "o.bump()\n"
             "return^ o.count\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    // 14.6: a declared field has no initialiser to run, so the one that fills
    // it is the only one the construction sees.
    LHAT_TEST("an abstract^ field is initialised by what fills it");
    run_text(&r,
             "var^ Greet = def^{\n"
             "  self^{ abstract^ n : number^ },\n"
             "  hello := f^self^ -> number^ { return^ self^.n + 1 },\n"
             "}\n"
             "var^ Thing = Greet .. def^{ self^{ n := 10 } }\n"
             "return^ Thing.new().hello()\n");
    CHECK_INTEGER(&r, 11);
    run_dispose(&r);

    // 14.15改: a mixin written against no base, composed onto one. What its
    // super^ reaches is settled by 14.2's chain the same as any other.
    LHAT_TEST("a mixin written against no base reaches the base it gets");
    run_text(&r,
             "var^ Base = def^{ self^{ n := 0 },\n"
             "  run := p^self^ { self^.n := self^.n + 1 } }\n"
             "var^ Logged = def^{ self^{ abstract^ n : number^ },\n"
             "  override^ run := p^self^ { self^.n := self^.n + 10\n"
             "                             super^() } }\n"
             "var^ App = Base .. Logged\n"
             "var^ o = App.new()\n"
             "o.run()\n"
             "return^ o.n\n");
    CHECK_INTEGER(&r, 11);
    run_dispose(&r);

    LHAT_TEST("and two of them stack in the order they are written");
    run_text(&r,
             "var^ Base = def^{ self^{ n := 0 },\n"
             "  run := p^self^ { self^.n := self^.n + 1 } }\n"
             "var^ A = def^{ self^{ abstract^ n : number^ },\n"
             "  override^ run := p^self^ { self^.n := self^.n + 10\n"
             "                             super^() } }\n"
             "var^ B = def^{ self^{ abstract^ n : number^ },\n"
             "  override^ run := p^self^ { self^.n := self^.n + 100\n"
             "                             super^() } }\n"
             "var^ App = Base .. A .. B\n"
             "var^ o = App.new()\n"
             "o.run()\n"
             "return^ o.n\n");
    CHECK_INTEGER(&r, 111);
    run_dispose(&r);

    // 14.5改: nothing is written under a name both sides carry, since the
    // checker will not read it through the composition -- but the method each
    // side wrote is still a value, and 14.4 applies it to whatever fits.
    LHAT_TEST("either side of an ambiguous name is still reachable");
    run_text(&r,
             "var^ A = def^{ self^{},\n"
             "  m := f^self^ -> number^ { return^ 1 },\n"
             "  only := f^ -> number^ { return^ 9 } }\n"
             "var^ B = def^{ self^{}, m := f^self^ -> number^ { return^ 2 } }\n"
             "var^ D = A .. B\n"
             "var^ fromA = A.m\n"
             "var^ fromB = B.m\n"
             "var^ o = D.new()\n"
             "return^ fromA(o) * 100 + fromB(o) * 10 + D.only()\n");
    CHECK_INTEGER(&r, 129);
    run_dispose(&r);

    // 14.2: the chain is settled at the definition, so an instance made
    // before a later definition is unaffected by it.
    LHAT_TEST("two definitions of the same shape stay separate");
    run_text(&r,
             "var^ Foo = def^{ self^{ n := 1 } }\n"
             "var^ Bar = def^{ self^{ n := 2 } }\n"
             "return^ Foo.new().n * 10 + Bar.new().n\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    LHAT_TEST("a composed definition's new fills the base's fields too");
    run_text(&r,
             "var^ Foo = def^{ self^{ a := 1, b := 2 } }\n"
             "var^ Bar = Foo .. def^{\n"
             "  self^{ c := 3 },\n"
             "  override^new := f^v { self^{ b := v } },\n"
             "}\n"
             "var^ x = Bar.new(9)\n"
             "return^ x.a * 100 + x.b * 10 + x.c\n");
    CHECK_INTEGER(&r, 193);
    run_dispose(&r);

    // 14.12: overload^ adds a way to call without losing the one that was
    // there, and 14.12's ban on overlapping signatures means the call finds
    // at most one candidate -- a search rather than a choice.
    LHAT_TEST("overload^ keeps the way that was already there");
    run_text(&r,
             "var^ Foo = def^{ self^{ }, m := f^self^ { return^ 1 } }\n"
             "var^ Bar = Foo .. def^{\n"
             "  self^{ },\n"
             "  overload^\n"
             "  m := f^self^, x:string^ { return^ 2 },\n"
             "}\n"
             "return^ Bar.new().m()\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("and answers the added one when that is what fits");
    run_text(&r,
             "var^ Foo = def^{ self^{ }, m := f^self^ { return^ 1 } }\n"
             "var^ Bar = Foo .. def^{\n"
             "  self^{ },\n"
             "  overload^\n"
             "  m := f^self^, x:string^ { return^ 2 },\n"
             "}\n"
             "return^ Bar.new().m(\"s\")\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    // 14.4: a candidate that takes self^ is reached as a method, so what was
    // given starts past the receiver. The search had been comparing the
    // receiver against the first parameter instead, and no candidate taking
    // self^ ever fitted.
    LHAT_TEST("a candidate taking self^ is found");
    run_text(&r,
             "var^ V = def^{ self^{},\n"
             "  m := f^self^, o:number^ -> number^ { return^ o },\n"
             "  overload^ m := f^self^, o:string^ -> number^ { return^ 9 },\n"
             "}\n"
             "var^ v = V.new()\n"
             "return^ v.m(7) * 10 + v.m(\"x\")\n");
    CHECK_INTEGER(&r, 79);
    run_dispose(&r);

    LHAT_TEST("across a composition as well");
    run_text(&r,
             "var^ Foo = def^{ self^{},\n"
             "  m := f^self^, o:number^ -> number^ { return^ o } }\n"
             "var^ Bar = Foo .. def^{ self^{},\n"
             "  overload^ m := f^self^, o:string^ -> number^ { return^ 9 } }\n"
             "var^ v = Bar.new()\n"
             "return^ v.m(7) * 10 + v.m(\"x\")\n");
    CHECK_INTEGER(&r, 79);
    run_dispose(&r);

    // 14.5 and 14.12 reach an operator the way they reach any other member.
    LHAT_TEST("a composition carries an operator in");
    run_text(&r,
             "var^ B = def^{ self^{ t := \"b\" },\n"
             "  op^.. := f^self^, o:string^ -> string^ { return^ self^.t .. o } }\n"
             "var^ D = B .. def^{ self^{} }\n"
             "return^ D.new() .. \"x\"\n");
    CHECK_STRING(&r, "bx");
    run_dispose(&r);

    LHAT_TEST("and an override^ of one replaces it");
    run_text(&r,
             "var^ B = def^{ self^{},\n"
             "  op^.. := f^self^, o:string^ -> string^ { return^ \"base\" } }\n"
             "var^ D = B .. def^{ self^{},\n"
             "  override^ op^.. := f^self^, o:string^ -> string^ {\n"
             "    return^ \"derived\" } }\n"
             "return^ D.new() .. \"x\"\n");
    CHECK_STRING(&r, "derived");
    run_dispose(&r);

    LHAT_TEST("while an overload^ of one adds to it");
    run_text(&r,
             "var^ B = def^{ self^{},\n"
             "  op^.. := f^self^, o:string^ -> string^ { return^ \"s\" } }\n"
             "var^ D = B .. def^{ self^{},\n"
             "  overload^ op^.. := f^self^, o:number^ -> string^ {\n"
             "    return^ \"n\" } }\n"
             "var^ d = D.new()\n"
             "return^ (d .. \"x\") .. (d .. 7)\n");
    CHECK_STRING(&r, "sn");
    run_dispose(&r);

    // 11.3 judges structurally, so a '..' put on a plain table by 14.14's
    // computed key answers exactly as a written op^ does.
    LHAT_TEST("a '..' reached by a computed key answers as well");
    run_text(&r,
             "var^ t = { [\"..\"] := f^self^, o:string^ -> string^ {\n"
             "  return^ o } }\n"
             "return^ t .. \"x\"\n");
    CHECK_STRING(&r, "x");
    run_dispose(&r);

    LHAT_TEST("and an operator may answer with a structure");
    run_text(&r,
             "var^ V = def^{ self^{ n := 3 },\n"
             "  op^+ := f^self^, o:number^ -> t^{ n : number^ } {\n"
             "    return^ self^ } }\n"
             "return^ (V.new() + 1).n\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 11.8 makes an operator a member, so 14.12's overload^ reaches it too --
    // and 14.4 makes the left operand the receiver, which is the shape the
    // search has to ask in.
    LHAT_TEST("an operator may be overloaded like any member");
    run_text(&r,
             "var^ V = def^{ self^{},\n"
             "  op^.. := f^self^, o:string^ -> number^ { return^ 1 },\n"
             "  overload^ op^.. := f^self^, o:number^ -> number^ { return^ 2 },\n"
             "}\n"
             "var^ v = V.new()\n"
             "return^ (v .. \"s\") * 10 + (v .. 7)\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    // 11.8改: the unary '-'. NEG handles number^ itself and comes here for
    // everything else, which is the same posture the binary instructions take.
    LHAT_TEST("a unary op^- answers '-x'");
    run_text(&r,
             "var^ V = def^{ self^{ n := 3 },\n"
             "  op^- := f^self^ -> number^ { return^ 0 - self^.n } }\n"
             "return^ -V.new()\n");
    CHECK_INTEGER(&r, -3);
    run_dispose(&r);

    // Both spellings under the one member name, told apart by the count --
    // which is what makes the unary one need no name of its own.
    LHAT_TEST("and stands beside the binary one");
    run_text(&r,
             "var^ V = def^{ self^{ n := 7 },\n"
             "  op^- := f^self^, o:number^ -> number^ { return^ self^.n - o },\n"
             "  overload^ op^- := f^self^ -> number^ { return^ 0 - self^.n } }\n"
             "var^ v = V.new()\n"
             "return^ (v - 2) * 10 + -v\n");
    CHECK_INTEGER(&r, 43);  // 5 * 10 + (-7)
    run_dispose(&r);

    // run_text compiles straight past the checker, so these are the machine
    // answering on its own: the counts do not stand in for each other.
    LHAT_TEST("a unary op^- is no candidate for a binary use");
    run_text(&r,
             "var^ V = def^{ self^{ n := 3 },\n"
             "  op^- := f^self^ -> number^ { return^ 0 } }\n"
             "return^ V.new() - 1\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_NO_CANDIDATE);
    run_dispose(&r);

    LHAT_TEST("nor a binary one for a unary use");
    run_text(&r,
             "var^ V = def^{ self^{ n := 3 },\n"
             "  op^- := f^self^, o:number^ -> number^ { return^ 0 } }\n"
             "return^ -V.new()\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_NO_CANDIDATE);
    run_dispose(&r);

    // 11.3 judges structurally here too, so a '-' put on a plain table
    // answers exactly as a written op^ does.
    LHAT_TEST("a unary '-' reached by a computed key answers as well");
    run_text(&r,
             "var^ t = { [\"-\"] := f^self^ -> number^ { return^ 9 } }\n"
             "return^ -t\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    // A type carrying nothing for it faults the way it always did.
    LHAT_TEST("and a table with no '-' still faults");
    run_text(&r, "var^ t = { }\nreturn^ -t\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    // The candidates may differ in the type rather than the count, which is
    // the case a count alone could not tell apart.
    LHAT_TEST("candidates of one arity are told apart by type");
    run_text(&r,
             "var^ Show = def^{\n"
             "  self^{ },\n"
             "  show := f^self^, x:string^ { return^ 1 },\n"
             "  overload^\n"
             "  show := f^self^, x:number^ { return^ 2 },\n"
             "}\n"
             "var^ s = Show.new()\n"
             "return^ s.show(\"t\") * 10 + s.show(7)\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    // 14.10: the judgement is structural, so a candidate taking a shape is
    // asked whether the value has those members.
    LHAT_TEST("a structural parameter is judged by its members");
    run_text(&r,
             "var^ Draw = def^{\n"
             "  self^{ },\n"
             "  draw := f^self^, s:t^{ radius : number^ } { return^ 1 },\n"
             "  overload^\n"
             "  draw := f^self^, s:t^{ width : number^, height : number^ } { return^ 2 },\n"
             "}\n"
             "var^ d = Draw.new()\n"
             "return^ d.draw({ radius := 1 }) * 10 + d.draw({ width := 1, height := 2 })\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    LHAT_TEST("no candidate taking these arguments is a fault");
    run_text(&r,
             "var^ Show = def^{\n"
             "  self^{ },\n"
             "  show := f^self^, x:string^ { return^ 1 },\n"
             "  overload^\n"
             "  show := f^self^, x:number^ { return^ 2 },\n"
             "}\n"
             "return^ Show.new().show(true^)\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_NO_CANDIDATE);
    run_dispose(&r);

    // 03 の 5.11c. The two paths answer differently here, which is the whole
    // reason the static one exists: a parameter left to inference (03 の 3.4)
    // is a type the checker knows and the descriptor fits_call reads does not
    // -- lower_type had no annotation to read, and a descriptor that is not
    // there asks nothing. So the search takes the first arm for any argument
    // at all, while the checker took the second. Strict bakes in the arm it
    // resolved, and the answer is the one checking promised.
    static const char *inferred_parameter_arms =
        "var^ Show = def^{\n"
        "  self^{ },\n"
        "  show := f^self^, x { return^ x + 1 },\n"
        "  overload^\n"
        "  show := f^self^, y:string^ -> number^ { return^ 2 },\n"
        "}\n"
        "return^ Show.new().show(\"t\")\n";

    LHAT_TEST("strict takes the arm the checker resolved");
    run_checked_text(&r, inferred_parameter_arms);
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    LHAT_TEST("without checking the search takes the arm asking nothing");
    run_text(&r, inferred_parameter_arms);
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    // 03 の 5.11c: an arm index means the same thing on both sides only if
    // the group holds what the checker's type says it holds. An override^
    // over an overloaded name is where that is decided -- the replacement
    // takes the place of the arm it replaces rather than going in front of a
    // group that still carries it, so the arms after it keep their positions.
    LHAT_TEST("an override^ leaves the other arms where they were");
    run_checked_text(&r,
             "var^ Base = def^{\n"
             "  self^{ },\n"
             "  m := f^self^, x:number^ -> number^ { return^ 1 },\n"
             "  overload^\n"
             "  m := f^self^, y:string^ -> number^ { return^ 2 },\n"
             "}\n"
             "var^ Derived = Base .. def^{\n"
             "  override^\n"
             "  m := f^self^, x:number^ -> number^ { return^ 3 },\n"
             "}\n"
             "var^ d = Derived.new()\n"
             "return^ d.m(0) * 10 + d.m(\"t\")\n");
    CHECK_INTEGER(&r, 32);
    run_dispose(&r);

    // 02 の 14.12: the ban is pairwise, so a name carries as many ways of
    // being called as are told apart. Three of them is where an arm index
    // starts being worth checking: the middle one is not reachable by taking
    // either end.
    static const char *three_arms =
        "var^ S = def^{\n"
        "  self^{ },\n"
        "  m := f^self^, x:number^ -> number^ { return^ 1 },\n"
        "  overload^\n"
        "  m := f^self^, y:string^ -> number^ { return^ 2 },\n"
        "  overload^\n"
        "  m := f^self^, z:bool^ -> number^ { return^ 4 },\n"
        "}\n"
        "var^ s = S.new()\n"
        "return^ s.m(0) * 100 + s.m(\"t\") * 10 + s.m(true^)\n";

    LHAT_TEST("three arms are told apart, resolved statically");
    run_checked_text(&r, three_arms);
    CHECK_INTEGER(&r, 124);
    run_dispose(&r);

    // The search has to reach the same three, which is what says the index
    // strict bakes in and the order the group is built in agree.
    LHAT_TEST("three arms are told apart by the search too");
    run_text(&r, three_arms);
    CHECK_INTEGER(&r, 124);
    run_dispose(&r);

    // 03 の 5.11c, the case the two orders differ in most: the arm replaced
    // is neither the first nor the last, so putting the replacement in front
    // instead of in its place would move both of the others.
    LHAT_TEST("an override^ of a middle arm moves none of the others");
    run_checked_text(&r,
             "var^ Base = def^{\n"
             "  self^{ },\n"
             "  m := f^self^, x:number^ -> number^ { return^ 1 },\n"
             "  overload^\n"
             "  m := f^self^, y:string^ -> number^ { return^ 2 },\n"
             "  overload^\n"
             "  m := f^self^, z:bool^ -> number^ { return^ 4 },\n"
             "}\n"
             "var^ Derived = Base .. def^{\n"
             "  override^\n"
             "  m := f^self^, y:string^ -> number^ { return^ super^(y) + 20 },\n"
             "}\n"
             "var^ d = Derived.new()\n"
             "return^ d.m(0) * 1000 + d.m(\"t\") * 10 + d.m(true^)\n");
    CHECK_INTEGER(&r, 1224);
    run_dispose(&r);

    // 02 の 14.12改: super^ is bound from the group as it was before the
    // write, so dropping the replaced arm from the new group leaves it
    // reachable -- and the call through it resolves against the old arms.
    LHAT_TEST("super^ still reaches the arm an override^ replaced");
    run_checked_text(&r,
             "var^ Base = def^{\n"
             "  self^{ },\n"
             "  m := f^self^, x:number^ -> number^ { return^ 1 },\n"
             "  overload^\n"
             "  m := f^self^, y:string^ -> number^ { return^ 2 },\n"
             "}\n"
             "var^ Derived = Base .. def^{\n"
             "  override^\n"
             "  m := f^self^, x:number^ -> number^ { return^ super^(x) + 30 },\n"
             "}\n"
             "return^ Derived.new().m(0)\n");
    CHECK_INTEGER(&r, 31);
    run_dispose(&r);

    // 14.8: number^ is one type, so either representation answers to it.
    LHAT_TEST("either representation of a number answers to number^");
    run_text(&r,
             "var^ Show = def^{\n"
             "  self^{ },\n"
             "  show := f^self^, x:string^ { return^ 1 },\n"
             "  overload^\n"
             "  show := f^self^, x:number^ { return^ 2 },\n"
             "}\n"
             "return^ Show.new().show(0.5)\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    LHAT_TEST("an ordinary member is untouched by any of this");
    run_text(&r,
             "var^ Foo = def^{ self^{ }, m := f^self^, x { return^ x + 1 } }\n"
             "return^ Foo.new().m(1)\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    // 14.13: self^{ … } outside a definition has no fields to name.
    LHAT_TEST("self^{ } outside a definition does not compile");
    run_text(&r, "var^ x = self^{ a := 1 }\nreturn^ x\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNSUPPORTED);
    run_dispose(&r);
}

// 02 の 13.11: fits^ against anything that can be written as a type. 04 の 6.1's
// error kinds are in test_errors above and 05 の 8.8's host types in test_io --
// the same instruction answers all of them, so what is pinned here is the rest:
// the builtin names, a structure, a union, and a def^.
static void test_isa(void)
{
    Run r;

    LHAT_TEST("a builtin name asks about the value's own tag");
    run_text(&r, "return^ 1 fits^ number^\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("and answers false for a value of another kind");
    run_text(&r, "return^ \"x\" fits^ number^\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    LHAT_TEST("string^");
    run_text(&r, "return^ \"x\" fits^ string^\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("bool^");
    run_text(&r, "return^ true^ fits^ bool^\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // 14.8: one type, two representations -- so both answer number^.
    LHAT_TEST("a real is a number^ the same way an integer is");
    run_text(&r, "return^ 1.5 fits^ number^\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("t^");
    run_text(&r, "return^ { a := 1 } fits^ t^{}\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("int^ and float^ are both number^");
    run_text(&r, "return^ 1 fits^ int^ and^ 1.5 fits^ float^\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("p^; asks only that it is a subroutine");
    run_text(&r, "return^ (p^ { }) fits^ p^;\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // 11.7 and 04 の 11.4: nil^ is a name like the others now, so the question
    // reaches the same instruction rather than one of its own.
    LHAT_TEST("nil^");
    run_text(&r, "return^ nil^ fits^ nil^\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("and something that is not nil^ is not");
    run_text(&r, "return^ 1 fits^ nil^\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    // 14.10: at least these members.
    LHAT_TEST("a structure asks for the members it names");
    run_text(&r, "return^ { a := 1, b := 2 } fits^ t^{ a : number^ }\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("and refuses a value missing one of them");
    run_text(&r, "return^ { b := 2 } fits^ t^{ a : number^ }\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    LHAT_TEST("a member of the wrong type does not answer for it either");
    run_text(&r, "return^ { a := \"x\" } fits^ t^{ a : number^ }\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    // 13.5: an arm is enough. The parser leaves a union as a tree of two
    // sides, so a three-armed one is what pins that every arm is reached.
    LHAT_TEST("a union holds when any arm does");
    run_text(&r, "return^ \"x\" fits^ number^|string^\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("a three-armed union reaches its last arm");
    run_text(&r, "return^ true^ fits^ number^|string^|bool^\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("and fails when no arm does");
    run_text(&r, "return^ true^ fits^ number^|string^\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    // 13.7: any^ is the top of every value, so the answer is fixed. The
    // checker reports the writing (test_check); a compile that never checked
    // still has to answer, and this is the answer.
    LHAT_TEST("fits^ any^ is true whatever is on the left");
    run_text(&r, "return^ \"x\" fits^ any^\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("and the left side still runs");
    run_text(&r,
             "var^ n = 0\n"
             "var^ bump = f^ { n := n + 1 return^ n }\n"
             "var^ b = bump() fits^ any^\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 14.9 keeps a definition structural, and 14.2 builds it while the program
    // runs -- so the name is loaded as a value and the shape read off it.
    LHAT_TEST("a def^ name asks for the shape the definition holds");
    run_text(&r,
             "var^ Point = def^{ self^{ x := 0, y := 0 } }\n"
             "return^ Point.new() fits^ Point\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("a value that is not a table is not an instance of one");
    run_text(&r,
             "var^ Point = def^{ self^{ x := 0, y := 0 } }\n"
             "return^ 1 fits^ Point\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    LHAT_TEST("a table missing a member of the definition does not fit it");
    run_text(&r,
             "var^ Point = def^{ self^{ x := 0, y := 0 }, m := f^ { return^ 1 } }\n"
             "return^ { m := 1 } fits^ Point\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    // 14.7: composition carries everything the left part carried, so an
    // instance of the composed definition still fits the plain one. This is
    // what 13.11 wanted conformance rather than exact identity for.
    LHAT_TEST("an instance of a composition fits the definition composed into");
    run_text(&r,
             "var^ Point = def^{ self^{ x := 0 }, m := f^ { return^ 1 } }\n"
             "var^ Point3 = Point .. def^{ self^{ z := 0 } }\n"
             "return^ Point3.new() fits^ Point\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("and the other direction does not hold");
    run_text(&r,
             "var^ Point = def^{ self^{ x := 0 } }\n"
             "var^ Point3 = Point .. def^{ self^{ z := 0 } }\n"
             "return^ Point.new() fits^ Point3\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    // 14.15: a field written as a type rather than a value, which is where a
    // definition's shape carries a type at all.
    LHAT_TEST("a declared field asks for its type as well as its name");
    run_text(&r,
             "var^ Named = def^{ self^{ abstract^ name : string^ } }\n"
             "return^ { name := 1 } fits^ Named\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    LHAT_TEST("and holds when the member is of that type");
    run_text(&r,
             "var^ Named = def^{ self^{ abstract^ name : string^ } }\n"
             "return^ { name := \"x\" } fits^ Named\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // 14.15 again: a definition may be annotated with itself, and the walk of
    // its shape has to stop descending rather than follow that for ever. The
    // member is left asking for its name alone, which is what makes this
    // answer at all.
    LHAT_TEST("a definition naming itself lowers without running away");
    run_text(&r,
             "var^ Node = def^{ self^{ value := 0 }, abstract^ next : Node }\n"
             "return^ { value := 1, next := 2 } fits^ Node\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // 5.13: the right side is a type the compiler settles, always. A value
    // that only arrives while the program runs -- a definition passed as a
    // parameter -- carries no type to ask about: it is a plain table there,
    // and a program that receives one probes it member by member instead.
    // No fallback reads a shape off the table at run time.
    LHAT_TEST("a definition reached as a value is not a type");
    run_text(&r,
             "var^ Point = def^{ self^{ x := 0 }, m := f^ { return^ 1 } }\n"
             "var^ fits = f^ D { return^ Point.new() fits^ D }\n"
             "return^ fits(Point)\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    LHAT_TEST("and not inside a union either");
    run_text(&r,
             "var^ f = f^ D { return^ 1 fits^ D|nil^ }\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNSUPPORTED);
    run_dispose(&r);

    LHAT_TEST("a name that reaches nothing does not compile");
    run_text(&r, "return^ 1 fits^ Nowhere\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    // 11.6 with 5.13: the same rule holds for as^ -- it promised to
    // panic on a mismatch, so a cast the compiler cannot lower is refused
    // rather than silently checking nothing.
    LHAT_TEST("as^ against a value-only definition is refused too");
    run_text(&r,
             "var^ f = f^ D { return^ 1 as^ D }\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    // 11.6: as^ lowers the written type the same way fits^ does, so a
    // union reaching every one of its arms is one question for both. What
    // this pins is the lowering: a union that kept only its first arm would
    // make the second one panic where it should hold.
    LHAT_TEST("as^ against a union holds for an arm past the first");
    run_text(&r,
             "var^ x = nil^\n"
             "var^ y = x as^ number^|nil^\n"
             "return^ 1\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 11.6改3: and answers the failure for a value no arm admits. The
    // lowering is what this still pins -- a union that kept only its first
    // arm would fail here where it should hold, and hold above where it
    // should fail.
    LHAT_TEST("and answers a failure for a value no arm admits");
    run_text(&r,
             "var^ x = \"s\"\n"
             "var^ y = x as^ number^|nil^\n"
             "return^ y fits^ localerror^.CastFailure\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_OK);
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // 13.11's narrowing is the checker's half; this is the machine's -- the
    // branch taken has to be the one the value actually answers for.
    LHAT_TEST("fits^ decides a branch at run time");
    run_checked_text(&r,
                     "var^ describe = f^ x:any^ {\n"
                     "    if^ x fits^ number^ { return^ 1 }\n"
                     "    if^ x fits^ string^ { return^ 2 }\n"
                     "    return^ 0\n"
                     "}\n"
                     "return^ describe(\"x\")\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);
}

// 02 の 14.12改: typeof^ reflects the value itself rather than anything the
// checker inferred (03 の 4.2), so this is testing reflect_type's walk of the
// actual runtime object graph.
static void test_typeof(void)
{
    Run r;

    LHAT_TEST("a number's signature");
    run_text(&r, "return^ typeof^(5).signature\n");
    CHECK_STRING(&r, "number^");
    run_dispose(&r);

    LHAT_TEST("and a real's is the same -- 14.8 is one type");
    run_text(&r, "return^ typeof^(5.5).signature\n");
    CHECK_STRING(&r, "number^");
    run_dispose(&r);

    LHAT_TEST("a string's signature");
    run_text(&r, "return^ typeof^(\"x\").signature\n");
    CHECK_STRING(&r, "string^");
    run_dispose(&r);

    LHAT_TEST("a bool's signature");
    run_text(&r, "return^ typeof^(true^).signature\n");
    CHECK_STRING(&r, "bool^");
    run_dispose(&r);

    LHAT_TEST("nil^'s signature");
    run_text(&r, "return^ typeof^(nil^).signature\n");
    CHECK_STRING(&r, "nil^");
    run_dispose(&r);

    // 14.16: typeof^ answers the checker's type for the instance --
    // fields and methods folded as the checker sees them, with 14.11's new
    // belonging to the definition rather than to what it makes.
    LHAT_TEST("an instance's signature carries fields and methods");
    run_checked_text(&r,
             "var^ Point = def^{ self^{ x := 0, y := 0 },\n"
             "  sum := f^self^ -> number^ { return^ self^.x + self^.y } }\n"
             "return^ typeof^(Point.new()).signature\n");
    CHECK_STRING(&r, "t^{ sum : f^self^ -> number^;, x : number^, y : number^ }");
    run_dispose(&r);

    // 14.7改: and the definition's says what it makes, in the self^{ … }
    // section a def^ writes its template as. What follows is its own -- new
    // and the static members -- and 13.13's Self^ inside is the instance,
    // which is what folds the ring the member's own type would otherwise be.
    LHAT_TEST("a definition's signature carries the instances in a section");
    run_checked_text(&r,
             "var^ Point = def^{ self^{ x := 0 },\n"
             "  add := f^self^, rhs:Self^ -> Self^ { return^ self^ },\n"
             "  origin := f^ -> number^ { return^ 0 } }\n"
             "return^ typeof^(Point).signature\n");
    CHECK_STRING(&r,
                 "t^{ self^{ add : f^self^, Self^ -> Self^;, x : number^ }, "
                 "new : f^ -> Self^;, origin : f^ -> number^; }");
    run_dispose(&r);

    // 14.4: the receiver is written as a parameter, so a signature with no
    // self^ in it is a plain subroutine and nothing else.
    LHAT_TEST("a function's signature carries its parameters and result");
    run_text(&r,
             "var^ f = f^ x:number^, y:string^ -> bool^ { return^ true^ }\n"
             "return^ typeof^(f).signature\n");
    CHECK_STRING(&r, "f^number^, string^ -> bool^;");
    run_dispose(&r);

    LHAT_TEST("and a procedure with no result says nothing after it");
    run_text(&r, "var^ p = p^ x:number^ { }\nreturn^ typeof^(p).signature\n");
    CHECK_STRING(&r, "p^number^;");
    run_dispose(&r);

    // 15.5: calling a yielding body does not run it -- it makes the coroutine
    // 13.9 describes, so that is what the signature answers. Reading `result`
    // alone would print 13.9's T, which is what the body returns rather than
    // what the caller is handed.
    LHAT_TEST("a yielding body answers the coroutine it makes");
    run_checked_text(&r,
                     "var^ g = p^ x:number^ { yield^ x }\n"
                     "return^ typeof^(g).signature\n");
    CHECK_STRING(&r, "p^number^ -> c^{p^ -> number^};");
    run_dispose(&r);

    // 13.9's third type is what a written result says, and it stays there.
    LHAT_TEST("and what it returns is the third of the three");
    run_checked_text(&r,
                     "var^ g = p^ x:number^ -> string^ { yield^ x return^ \"z\" }\n"
                     "return^ typeof^(g).signature\n");
    CHECK_STRING(&r, "p^number^ -> c^{p^ -> number^ -> string^};");
    run_dispose(&r);

    // 13.8改2: a tuple in a result position is written bare -- the same
    // reading the parameter side has always had, and the one the coroutine's
    // R already accepted. The parentheses are left to say the one thing they
    // still have to: that a union covers the whole tuple.
    LHAT_TEST("a tuple result is written without parentheses");
    run_checked_text(&r,
                     "var^ ny = f^ x:number^ {\n"
                     "    var^ _^:string^, _^:number^ = _yield^ \"a\", x\n"
                     "    return^ \"done\"\n"
                     "}\n"
                     "return^ typeof^(ny).signature\n");
    CHECK_STRING(&r,
                 "f^number^ -> c^{f^string^, number^ -> string^, number^ -> "
                 "string^};");
    run_dispose(&r);

    LHAT_TEST("and its resume says the same about what it sends and answers");
    run_checked_text(&r,
                     "var^ ny = f^ x:number^ {\n"
                     "    var^ _^:string^, _^:number^ = _yield^ \"a\", x\n"
                     "    return^ \"done\"\n"
                     "}\n"
                     "var^ c = ny(1)\n"
                     "return^ typeof^(c.resume).signature\n");
    CHECK_STRING(&r, "f^string^, number^ -> string^, number^|nil^;");
    run_dispose(&r);

    // 13.8改 with 04 の 8.2: an error stands beside a tuple rather than
    // folding into its positions, so the union is over the whole of it --
    // which is exactly what the parentheses say. Without them the text would
    // read as a union in the last position alone.
    LHAT_TEST("a union over the whole tuple keeps its parentheses");
    run_checked_text(&r,
                     "errordef^ E { Bad }\n"
                     "var^ risky = f^ -> (string^, number^)|E.Bad {\n"
                     "    return^ error^E.Bad{}\n"
                     "}\n"
                     "return^ typeof^(risky).signature\n");
    CHECK_STRING(&r, "f^ -> (string^, number^)|E.Bad;");
    run_dispose(&r);

    LHAT_TEST("and one over the last position alone does not");
    run_checked_text(&r,
                     "var^ loose = f^ -> string^, number^|nil^ {\n"
                     "    return^ \"a\", nil^\n"
                     "}\n"
                     "return^ typeof^(loose).signature\n");
    CHECK_STRING(&r, "f^ -> string^, number^|nil^;");
    run_dispose(&r);

    // 05 の 8.7: and the bare spelling reads back as the type it names.
    LHAT_TEST("the bare spelling round-trips as an annotation");
    run_checked_text(&r,
                     "var^ ny = f^ x:number^ {\n"
                     "    var^ _^:string^, _^:number^ = _yield^ \"a\", x\n"
                     "    return^ \"done\"\n"
                     "}\n"
                     "var^ held : c^{f^string^, number^ -> string^, "
                     "number^ -> string^} = ny(1)\n"
                     "return^ typeof^(held).signature\n");
    CHECK_STRING(&r,
                 "c^{f^string^, number^ -> string^, number^ -> string^}");
    run_dispose(&r);

    // 05 の 8.7: typeof^'s answer reads back as a type, so writing that answer
    // into the literal has to give the same answer again. The three slots the
    // proto carries are 13.9's three, never the c^{ … } around them -- holding
    // the written form whole would have tag_type (vm.c) wrap it a second time.
    LHAT_TEST("and writing that coroutine back gives the same signature");
    run_checked_text(&r,
                     "var^ g = p^ x:number^ -> c^{p^ -> number^ -> string^}\n"
                     "    { yield^ x return^ \"z\" }\n"
                     "return^ typeof^(g).signature\n");
    CHECK_STRING(&r, "p^number^ -> c^{p^ -> number^ -> string^};");
    run_dispose(&r);

    // 13.9: the three slots each say "none" in their own way, and typeof^
    // writes each one out as what it is -- '-' for a body that cannot end,
    // nothing at all for one that ends without a value or receives nothing.
    // 05 の 8.7 wants every one of these to read back as the type it names.
    LHAT_TEST("a body that cannot end writes its third slot '-'");
    run_checked_text(&r,
                     "var^ g = p^ { repeat^ { yield^ 1 } }\n"
                     "return^ typeof^(g).signature\n");
    CHECK_STRING(&r, "p^ -> c^{p^ -> number^ -> -};");
    run_dispose(&r);

    LHAT_TEST("and one that ends without a value leaves it empty");
    run_checked_text(&r,
                     "var^ g = p^ { yield^ 1 }\n"
                     "return^ typeof^(g).signature\n");
    CHECK_STRING(&r, "p^ -> c^{p^ -> number^};");
    run_dispose(&r);

    LHAT_TEST("a yield^ with no value still hands nil^ over, so Y says so");
    run_checked_text(&r,
                     "var^ g = p^ { yield^ }\n"
                     "return^ typeof^(g).signature\n");
    CHECK_STRING(&r, "p^ -> c^{p^ -> nil^};");
    run_dispose(&r);

    LHAT_TEST("and a var^ receiving one is what puts a type in the first slot");
    run_checked_text(&r,
                     "var^ g = p^ { var^ x : string^ = yield^ 1 }\n"
                     "return^ typeof^(g).signature\n");
    CHECK_STRING(&r, "p^ -> c^{p^string^ -> number^};");
    run_dispose(&r);

    // 04 の 2.4 sends a type mentioning an error to the instruction rather
    // than resolving it, so this is the path that reads the proto's slots.
    LHAT_TEST("and it holds when an error kind sends it to the instruction");
    run_checked_text(&r,
                     "errordef^ E { Bad }\n"
                     "var^ g = p^ -> c^{p^ -> number^ -> string^|E.Bad}\n"
                     "    { yield^ 1 return^ \"z\" }\n"
                     "return^ typeof^(g).signature\n");
    CHECK_STRING(&r, "p^ -> c^{p^ -> number^ -> string^|E.Bad};");
    run_dispose(&r);

    // The two typeof^ paths (03 の 5.11b's resolved one and the instruction)
    // answer the same thing about the same value, so the coroutine a call
    // makes is written exactly as the signature promised it.
    LHAT_TEST("the coroutine itself reads the same as the promise");
    run_checked_text(&r,
                     "var^ g = p^ x:number^ -> string^ { yield^ x return^ \"z\" }\n"
                     "return^ typeof^(g(1)).signature\n");
    CHECK_STRING(&r, "c^{p^ -> number^ -> string^}");
    run_dispose(&r);

    // 03 の 3.4: nothing was written, and the body decided it. 5.11b takes the
    // checker's answer here rather than walking the closure, which carries no
    // parameter types of its own beyond the written ones.
    LHAT_TEST("an inferred parameter type reaches the signature");
    run_checked_text(&r,
                     "var^ f = f^ x, y { x + y }\n"
                     "return^ typeof^(f).signature\n");
    CHECK_STRING(&r, "f^number^, number^ -> number^;");
    run_dispose(&r);

    // 02 の 14.10: what inference did not decide is written out as UNKNOWN,
    // which is deliberately not a type expression -- writing any^ would hide
    // both that it is undecided and which member wants an annotation.
    LHAT_TEST("what inference did not decide is written UNKNOWN");
    run_checked_text(&r,
                     "var^ h = p^ x { x.write() }\n"
                     "return^ typeof^(h).signature\n");
    CHECK_STRING(&r, "p^t^{ write : UNKNOWN };");
    run_dispose(&r);

    LHAT_TEST("and a parameter nothing demanded says so too");
    run_checked_text(&r,
                     "var^ g = f^ x { 1 }\n"
                     "return^ typeof^(g).signature\n");
    CHECK_STRING(&r, "f^UNKNOWN -> number^;");
    run_dispose(&r);

    // 5.11b without the checker: the closure carries only what was written, so
    // the walk answers any^ for the rest. What compiles without checking is
    // unchanged by any of the above (4.2).
    LHAT_TEST("compiled without checking, the same one walks the closure");
    run_text(&r,
             "var^ g = f^ x { 1 }\n"
             "return^ typeof^(g).signature\n");
    CHECK_STRING(&r, "f^any^;");
    run_dispose(&r);

    // 04 の 2.4: identity is the declaration, so typeof^ answers with the
    // kind's own qualified name -- the example 04-errors.md 136 gives.
    LHAT_TEST("an error's signature is its qualified kind name");
    run_text(&r,
             "errordef^ IOError { NotFound }\n"
             "var^ e = error^IOError.NotFound{ }\n"
             "return^ typeof^(e).signature\n");
    CHECK_STRING(&r, "IOError.NotFound");
    run_dispose(&r);

    // 14.10: the sequence half is written as bare types, in order, with no
    // names -- not as members keyed by their position.
    LHAT_TEST("a table literal's positional part has no names");
    run_checked_text(&r, "return^ typeof^({1, \"x\"}).signature\n");
    CHECK_STRING(&r, "t^{ number^, string^ }");
    run_dispose(&r);

    // 02 の 2811: typeof^(x) = typeof^(y) is 11.3's structural identity, not
    // the identity of the two LhatRuntimeType objects reflect_type built --
    // two separate calls never share one.
    LHAT_TEST("typeof^(x) = typeof^(y) compares structurally");
    run_text(&r, "return^ typeof^(5) = typeof^(6)\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("and answers false across different shapes");
    run_text(&r, "return^ typeof^(5) = typeof^(\"x\")\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    // 14.9: a name never takes part in identity -- two independently written
    // definitions of the same shape are the same type.
    LHAT_TEST("two unrelated definitions of the same shape are equal");
    run_checked_text(&r,
             "var^ A = def^{ self^{ n := 0 } }\n"
             "var^ B = def^{ self^{ n := 5 } }\n"
             "return^ typeof^(A.new()) = typeof^(B.new())\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("and unequal once the shapes differ");
    run_checked_text(&r,
             "var^ A = def^{ self^{ n := 0 } }\n"
             "var^ B = def^{ self^{ n := 0, extra := 1 } }\n"
             "return^ typeof^(A.new()) = typeof^(B.new())\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    // 11.3改: which operand the receiver is says which way round the operator
    // may be written, so two that differ in nothing else are still two types.
    LHAT_TEST("and an operator taking the other operand is another shape");
    run_checked_text(&r,
             "var^ A = def^{ self^{ n := 0 },\n"
             "  op^+ := f^self^, r:number^ -> number^ { return^ self^.n + r } }\n"
             "var^ B = def^{ self^{ n := 0 },\n"
             "  op^+ := f^l:number^, self^ -> number^ { return^ l + self^.n } }\n"
             "return^ typeof^(A.new()) = typeof^(B.new())\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    // 14.4 with 14.16: and the signature says which, by where self^ stands.
    LHAT_TEST("and the signature puts the receiver where it was written");
    run_checked_text(&r,
             "var^ B = def^{ self^{ n := 0 },\n"
             "  op^+ := f^l:number^, self^ -> number^ { return^ l + self^.n } }\n"
             "return^ typeof^(B.new()).signature\n");
    CHECK_STRING(&r, "t^{ + : f^number^, self^ -> number^;, n : number^ }");
    run_dispose(&r);

    // 14.10's round trip: what typeof^(x).signature answers with has to be
    // usable as a type annotation.
    LHAT_TEST("the signature parses back as an annotation");
    run_text(&r,
             "var^ Point = def^{ self^{ x := 0, y := 0 } }\n"
             "var^ p : t^{ x : number^, y : number^ } = Point.new()\n"
             "return^ p.x + p.y\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 14.5, 14.12: a multi-dispatched member is callable every way its arms
    // list, which is what '&' means -- matching 14.12's own example.
    LHAT_TEST("an overload^ed member's signature is an intersection");
    run_checked_text(&r,
             "var^ Foo = def^{ self^{}, foo := p^self^ { } }\n"
             "var^ Bar = Foo .. def^{ self^{},\n"
             "  overload^ foo := p^self^, x:string^ { } }\n"
             "return^ typeof^(Bar.new()).signature\n");
    CHECK_STRING(&r, "t^{ foo : p^self^; & p^self^, string^; }");
    run_dispose(&r);

    // 14.16: a cycle among VALUES is invisible to a static answer -- the
    // annotation said t^{}, and that is the type, however the tables ended
    // up holding each other. The walk that once had to survive this is gone.
    LHAT_TEST("a cycle between two tables is no concern of the type");
    run_checked_text(&r,
             "var^ a = { }\n"
             "var^ b = { }\n"
             "var^ a.next : t^{} = b\n"
             "var^ b.next : t^{} = a\n"
             "return^ typeof^(a).signature\n");
    CHECK_STRING(&r, "t^{ next : t^{} }");
    run_dispose(&r);

    LHAT_TEST("and between two instances of the same definition");
    run_checked_text(&r,
             "var^ Node = def^{\n"
             "  self^{ abstract^next : t^{} },\n"
             "  override^new := f^ { self^{ next := { } } },\n"
             "}\n"
             "var^ a = Node.new()\n"
             "var^ b = Node.new()\n"
             "a.next := b\n"
             "b.next := a\n"
             "return^ typeof^(a).signature\n");
    CHECK_STRING(&r, "t^{ next : t^{} }");
    run_dispose(&r);

    // 14.16 with 14.7改: a definition's type carries its instances in the
    // self^{ … } section, so a plain table that happens to spell the same
    // members is not the same type -- what an instance would be is part of
    // what a definition is.
    LHAT_TEST("a definition is not the shape of its members alone");
    run_checked_text(&r,
             "var^ D = def^{ self^{ n := 0 }, run := p^self^ { } }\n"
             "var^ fake = { new := f^ -> number^ { return^ 0 }, run := p^ { } }\n"
             "return^ typeof^(D) = typeof^(fake)\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    // A value reached twice without a cycle -- shared, not circular -- is not
    // cut short. Only revisiting something still on the current path is.
    LHAT_TEST("a value reached two ways (no cycle) is not cut short");
    run_checked_text(&r,
             "var^ shared = { v := 1 }\n"
             "var^ t = { a := shared, b := shared }\n"
             "return^ typeof^(t).signature\n");
    CHECK_STRING(&r, "t^{ a : t^{ v : number^ }, b : t^{ v : number^ } }");
    run_dispose(&r);

    // 02 の 12.6 with 14.4: a dispose() a def^ wrote declares the receiver,
    // so with^ calls it as a method. It used to be called plainly, which no
    // written dispose() could answer -- the coroutine's is a native member
    // and carries its receiver, so nothing caught this.
    LHAT_TEST("with^ calls a written dispose() as a method");
    run_checked_text(&r,
                     "var^ log = { n := 0 }\n"
                     "var^ C = def^{ self^{ v := 1 },\n"
                     "  dispose := p^self^ { log.n := self^.v } }\n"
                     "with^ c = C.new() { }\n"
                     "return^ log.n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 13.4 keeps self^ out of the parameter list, so one written without it
    // takes no receiver and is called just the same.
    LHAT_TEST("and one written without a self^ is called too");
    run_checked_text(&r,
                     "var^ log = { n := 0 }\n"
                     "var^ t = { dispose := p^ { log.n := 7 } }\n"
                     "with^ r = t { }\n"
                     "return^ log.n\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // 13.12: and the target may be a '_^', which is the shape written when
    // 12.6's disposal is the whole reason for the form.
    LHAT_TEST("a '_^' target is disposed the same way");
    run_checked_text(&r,
                     "var^ log = { n := 0 }\n"
                     "var^ C = def^{ self^{ v := 3 },\n"
                     "  dispose := p^self^ { log.n := self^.v } }\n"
                     "with^ _^ = C.new() { }\n"
                     "return^ log.n\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 12.1 with 02 の 13.2: the block answers for the subroutine around it,
    // and 12.6's disposal still runs on the way out. Written this way the
    // whole of 04 の 5.1's shape holds together -- which the checker refused
    // to look at until it counted a with^ among the ways a body ends.
    LHAT_TEST("a return^ inside a with^ answers, and still disposes");
    run_checked_text(&r,
                     "var^ log = { n := 0 }\n"
                     "var^ C = def^{ self^{ v := 3 },\n"
                     "  dispose := p^self^ { log.n := 1 } }\n"
                     "var^ f = f^ -> number^ {\n"
                     "  with^ c = C.new() { return^ c.v }\n"
                     "}\n"
                     "var^ answered = f()\n"
                     "return^ answered * 10 + log.n\n");
    CHECK_INTEGER(&r, 31);
    run_dispose(&r);
}

// The member a program answered, asked whether calling it hands it a
// receiver.
#define CHECK_TAKES_RECEIVER(r, expected)                                     \
    do {                                                                      \
        LHAT_CHECK_EQ_INT((r)->compiled, LHAT_COMPILE_OK);                    \
        LHAT_CHECK_EQ_INT((r)->ran.status, LHAT_RUN_OK);                      \
        LHAT_CHECK_EQ_INT(lhat_takes_receiver((r)->ran.value), (expected));   \
    } while (0)

// 02 の 14.7: what an instance may call and what belongs to the definition
// alone are told apart by one thing -- whether the member takes a receiver.
// A host reads the same answer through lhat_takes_receiver, which is what
// lets it say which members are static.
static void test_takes_receiver(void)
{
    Run r;

    LHAT_TEST("a member taking self^ is one an instance calls");
    run_text(&r,
             "var^ Foo = def^{ self^{ n := 0 },\n"
             "  add := f^self^, x:number^ -> number^ { return^ x } }\n"
             "return^ Foo.add\n");
    CHECK_TAKES_RECEIVER(&r, true);
    run_dispose(&r);

    LHAT_TEST("a member taking none is the definition's own");
    run_text(&r,
             "var^ Foo = def^{ self^{ n := 0 },\n"
             "  somestatic := p^{ } }\n"
             "return^ Foo.somestatic\n");
    LHAT_CHECK(lhat_is_object_kind(r.ran.value, LHAT_OBJECT_SUBROUTINE),
               "expected the member itself, not a missing key");
    CHECK_TAKES_RECEIVER(&r, false);
    run_dispose(&r);

    // 14.11: the new a definition gets when it writes none takes nothing,
    // so it falls on the same side as a static member.
    LHAT_TEST("the default new takes no receiver");
    run_text(&r,
             "var^ Foo = def^{ self^{ n := 0 } }\n"
             "return^ Foo.new\n");
    LHAT_CHECK(lhat_is_object_kind(r.ran.value, LHAT_OBJECT_SUBROUTINE),
               "expected the default new itself");
    CHECK_TAKES_RECEIVER(&r, false);
    run_dispose(&r);

    // 11.3改: an op^ may write its self^ last, which says the RIGHT operand
    // is the receiver. It is still a receiver.
    LHAT_TEST("a self^-last op^ takes one all the same");
    run_text(&r,
             "var^ V = def^{ self^{ n := 0 },\n"
             "  op^+ := f^lhs:number^, self^ -> number^ {\n"
             "    return^ lhs + self^.n } }\n"
             "return^ V[\"+\"]\n");
    CHECK_TAKES_RECEIVER(&r, true);
    run_dispose(&r);

    // 14.12: which arm a call means is settled at the call, so a group
    // answers for the call that would need a receiver arranged.
    LHAT_TEST("an overload^ group answers for any arm that takes one");
    run_text(&r,
             "var^ Foo = def^{ self^{ n := 0 },\n"
             "  m := p^{ },\n"
             "  overload^ m := p^self^, x:number^ { } }\n"
             "return^ Foo.m\n");
    CHECK_TAKES_RECEIVER(&r, true);
    run_dispose(&r);

    LHAT_TEST("nothing that cannot be called takes a receiver");
    run_text(&r, "return^ 1\n");
    CHECK_TAKES_RECEIVER(&r, false);
    run_dispose(&r);

    run_text(&r, "return^ { a := 1 }\n");
    CHECK_TAKES_RECEIVER(&r, false);
    run_dispose(&r);
}

// 02 の 14.7改2: delegate^ -- what a definition holds shows through as its
// own. The machine's half: a third leg of 14.7's walk, and the receiver a
// delegated member is called with.
//
// The receiver is the thing worth pinning. A delegated member belongs to the
// delegate, so it runs with the delegate as self^ -- reading the answer off
// the delegate while handing it the wrapper would compile, run, and write
// into the wrong object.
static void test_delegate(void)
{
    Run r;

    // 'self^.name': a field of the template, so each instance delegates to
    // its own.
    LHAT_TEST("a delegated member is reached through the instance's field");
    run_checked_text(&r,
                     "let^ Inner = def^{\n"
                     "    self^{ n = 0 },\n"
                     "    read = f^self^ -> number^ { self^.n }\n"
                     "}\n"
                     "let^ Outer = def^{\n"
                     "    self^{ abstract^ held : Inner },\n"
                     "    override^new = f^ { self^{ held = Inner.new() } },\n"
                     "    delegate^ self^.held\n"
                     "}\n"
                     "return^ Outer.new().read()\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // THE ONE THAT MATTERS. A mutating member run through the delegation has
    // to write into the delegate; handing it the wrapper instead would put
    // the field on the wrapper and read back what was never written.
    LHAT_TEST("self^ inside a delegated member is the delegate");
    run_checked_text(&r,
                     "let^ Inner = def^{\n"
                     "    self^{ n = 0 },\n"
                     "    read = f^self^ -> number^ { self^.n },\n"
                     "    bump = p^self^ { self^.n := self^.n + 5 }\n"
                     "}\n"
                     "let^ Outer = def^{\n"
                     "    self^{ abstract^ held : Inner },\n"
                     "    override^new = f^ { self^{ held = Inner.new() } },\n"
                     "    delegate^ self^.held\n"
                     "}\n"
                     "let^ o = Outer.new()\n"
                     "o.bump()\n"
                     // Read off the delegate itself, so nothing about the
                     // wrapper can make this look right by accident.
                     "return^ o.held.read()\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    // The bare name: a member of the definition, so every instance delegates
    // to the one value.
    LHAT_TEST("a bare name delegates to the definition's own member");
    run_checked_text(&r,
                     "let^ Inner = def^{\n"
                     "    self^{ n = 0 },\n"
                     "    read = f^self^ -> number^ { self^.n },\n"
                     "    bump = p^self^ { self^.n := self^.n + 5 }\n"
                     "}\n"
                     "let^ Shared = def^{\n"
                     "    self^{ },\n"
                     "    sink = Inner.new(),\n"
                     "    delegate^ sink\n"
                     "}\n"
                     "let^ a = Shared.new()\n"
                     "let^ b = Shared.new()\n"
                     "a.bump()\n"
                     // One value behind both, which is what a definition's
                     // member is (14.3).
                     "return^ b.read()\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    // 14.7's order, reaching through to the delegate last.
    LHAT_TEST("a member written here wins over the delegate's");
    run_checked_text(&r,
                     "let^ Inner = def^{ read = f^self^ -> number^ { 1 } }\n"
                     "let^ Outer = def^{\n"
                     "    self^{ abstract^ held : Inner },\n"
                     "    override^new = f^ { self^{ held = Inner.new() } },\n"
                     "    read = f^self^ -> number^ { 99 },\n"
                     "    delegate^ self^.held\n"
                     "}\n"
                     "return^ Outer.new().read()\n");
    CHECK_INTEGER(&r, 99);
    run_dispose(&r);

    // 14.2 fixes the chain at the definition, not the value in the field --
    // so writing another delegate in is an ordinary field write.
    LHAT_TEST("the field may be written, and the delegation follows it");
    run_checked_text(&r,
                     "let^ Inner = def^{\n"
                     "    self^{ n = 0 },\n"
                     "    read = f^self^ -> number^ { self^.n },\n"
                     "    bump = p^self^ { self^.n := self^.n + 5 }\n"
                     "}\n"
                     "let^ Outer = def^{\n"
                     "    self^{ abstract^ held : Inner },\n"
                     "    override^new = f^ { self^{ held = Inner.new() } },\n"
                     "    delegate^ self^.held\n"
                     "}\n"
                     "var^ o = Outer.new()\n"
                     "o.bump()\n"
                     "var^ other = Inner.new()\n"
                     "o.held := other\n"
                     "return^ o.read()\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 14.7: what an instance sees is what takes a receiver, and delegation
    // does not widen that -- a static member of the delegate stays the
    // delegate's.
    LHAT_TEST("a static member of the delegate is not delegated");
    run_text(&r,
             "let^ Inner = def^{ somestatic = p^{ } }\n"
             "let^ Outer = def^{\n"
             "    self^{ abstract^ held : Inner },\n"
             "    override^new = f^ { self^{ held = Inner.new() } },\n"
             "    delegate^ self^.held\n"
             "}\n"
             "return^ Outer.new().somestatic is^ nil^\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);
}

int main(void)
{
    test_definitions();
    test_isa();
    test_typeof();
    test_takes_receiver();
    test_delegate();
    return lhat_test_report("test_vm_def");
}
