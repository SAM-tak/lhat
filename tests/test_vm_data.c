// L^ (lhat) -- tests for the bytecode and the machine.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed. Programs are run end to end, since what is worth pinning is the
// answer rather than the instructions chosen to reach it -- 5.1 expects those
// to be replaced by specialised ones later.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "code.h"
#include "fixture.h"

// 01 の 3.1 and 3.3: a name written with backticks is a name, and id^name is
// that name's spelling as a string. The two are separate forms -- one is how
// a name may be written, the other is how a string may be.
static void test_names(void)
{
    Run r;

    LHAT_TEST("a name written with backticks is a name");
    run_text(&r, "let^ `a b` = 41\nreturn^ `a b` + 1\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    LHAT_TEST("and the delimiters are not part of it");
    run_text(&r, "let^ t = { `a b` = 7 }\nreturn^ t.`a b`\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("a doubled backtick is one, in the name it makes");
    run_text(&r, "let^ t = { `a``b` = 5 }\nreturn^ t[\"a`b\"]\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    // 3.3: nothing is looked up -- the form is about how the string is
    // written, so a name that is nowhere still spells itself.
    LHAT_TEST("id^ answers the spelling, of a name that need not exist");
    run_text(&r, "return^ id^nowhere\n");
    CHECK_STRING(&r, "nowhere");
    run_dispose(&r);

    LHAT_TEST("and the name may be written with backticks or a hat");
    run_text(&r, "return^ id^`a b` .. \"|\" .. id^tostring^\n");
    CHECK_STRING(&r, "a b|tostring^");
    run_dispose(&r);

    // 3.3's own example: the spelling answered by a match is the key.
    LHAT_TEST("what id^ answers reads as a key");
    run_text(&r,
             "let^ foo = { a = f^ { return^ 1 }, b = f^ { return^ 2 } }\n"
             "return^ foo[for^ 2: when^ 1: id^a other^: id^b;]()\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);
}

static void test_strings(void)
{
    Run r;

    LHAT_TEST("a string literal is a value");
    run_text(&r, "return^ \"hello\"\n");
    CHECK_STRING(&r, "hello");
    run_dispose(&r);

    // 01 の 5 章: the escapes are resolved before the compiler sees them.
    LHAT_TEST("the escapes are already resolved");
    run_text(&r, "return^ \"a\\tb\"\n");
    CHECK_STRING(&r, "a\tb");
    run_dispose(&r);

    // 02 の 11.6: a string is what it says.
    LHAT_TEST("two strings spelling the same thing are equal");
    run_text(&r, "return^ \"foo\" = \"foo\"\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("and different ones are not");
    run_text(&r, "return^ \"foo\" = \"bar\"\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    // 11.6改: is^ asks whether the two are the same object, not whether
    // they spell the same thing -- unlike '=', a string built at runtime is
    // never the same instance as one built separately even when it reads
    // the same.
    LHAT_TEST("is^ asks identity rather than spelling, even for strings");
    run_text(&r,
             "var^ a = \"f\" .. \"oo\"\n"
             "var^ b = \"f\" .. \"oo\"\n"
             "return^ a is^ b\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    LHAT_TEST("but a name for the same string is the same instance");
    run_text(&r, "var^ a = \"f\" .. \"oo\"\nvar^ b = a\nreturn^ a is^ b\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // 02 の 11.2: '..' is concatenation in general; strings are the case
    // that is settled, and 14.5's composition of definitions is the other.
    LHAT_TEST("'..' joins two strings");
    run_text(&r, "return^ \"ab\" .. \"cd\"\n");
    CHECK_STRING(&r, "abcd");
    run_dispose(&r);

    LHAT_TEST("and joins a run of them right to left");
    run_text(&r, "return^ \"a\" .. \"b\" .. \"c\"\n");
    CHECK_STRING(&r, "abc");
    run_dispose(&r);

    LHAT_TEST("an empty string joins to nothing");
    run_text(&r, "return^ \"\" .. \"x\" .. \"\"\n");
    CHECK_STRING(&r, "x");
    run_dispose(&r);

    LHAT_TEST("the result is a string like any other");
    run_text(&r, "return^ (\"a\" .. \"b\") = \"ab\"\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("neither operand is changed");
    run_text(&r,
             "var^ a = \"one\"\n"
             "var^ b = a .. \"two\"\n"
             "return^ a\n");
    CHECK_STRING(&r, "one");
    run_dispose(&r);

    // 11.1: an operator is a function, and 11.3 asks the left operand for
    // it. A string answers built in; anything else answers with a member,
    // which 14.4 hands the left operand as self^.
    LHAT_TEST("a definition answers '..' with its own op^");
    run_text(&r,
             "var^ Vec = def^{\n"
             "  self^{ tag := \"v\" },\n"
             "  op^.. := f^self^, other:string^ -> string^ {\n"
             "    return^ self^.tag .. other\n"
             "  },\n"
             "}\n"
             "var^ v = Vec.new()\n"
             "return^ v .. \"!\"\n");
    CHECK_STRING(&r, "v!");
    run_dispose(&r);

    LHAT_TEST("and what it answers is an ordinary value");
    run_text(&r,
             "var^ Vec = def^{\n"
             "  self^{ tag := \"v\" },\n"
             "  op^.. := f^self^, other:string^ -> string^ {\n"
             "    return^ self^.tag .. other\n"
             "  },\n"
             "}\n"
             "var^ v = Vec.new()\n"
             "return^ (v .. \"!\") .. \"?\"\n");
    CHECK_STRING(&r, "v!?");
    run_dispose(&r);

    // 11.4: the arithmetic operators ask the same question '..' does.
    LHAT_TEST("a definition answers arithmetic with its own op^");
    run_text(&r,
             "var^ Vec = def^{\n"
             "  self^{ n := 10 },\n"
             "  op^+ := f^self^, o:number^ -> number^ { return^ self^.n + o },\n"
             "  op^* := f^self^, o:number^ -> number^ { return^ self^.n * o },\n"
             "}\n"
             "var^ v = Vec.new()\n"
             "return^ (v + 5) * 100 + (v * 3)\n");
    CHECK_INTEGER(&r, 1530);  // 15, then 30
    run_dispose(&r);

    // 11.5 の (5): 'a < b < c' is '(a < b) and^ (b < c)'. Written out and
    // compiled as that and^ it would read `b` twice, which is why the chain
    // is one node -- these two pin what that buys.
    LHAT_TEST("a comparison chain reads as the mathematics does");
    run_text(&r,
             "if^ 1 <= 2 <= 3 and^ !(1 <= 5 <= 3) and^ !(5 <= 2 <= 3) "
             "{ return^ 1 }\n"
             "return^ 0\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("and mixes kinds along its length");
    run_text(&r,
             "if^ 1 < 2 = 2 < 3 and^ \"a\" < \"b\" < \"c\" and^ "
             "1 < 2 isa^ number^ { return^ 1 }\n"
             "return^ 0\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // The operand two links share is evaluated once. A call written there
    // would otherwise run twice, and how many times is not something the
    // reader of 'a < f() < b' expects to have to work out.
    LHAT_TEST("a shared operand runs once");
    run_text(&r,
             "var^ n = 0\n"
             "var^ bump = p^ -> number^ { n := n + 1 return^ 2 }\n"
             "var^ held = 1 < bump() < 3\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // and^ decides without the right side once the left has settled it, so a
    // link that fails ends the chain where it stands.
    LHAT_TEST("and a false link ends it");
    run_text(&r,
             "var^ m = 0\n"
             "var^ side = p^ -> number^ { m := m + 1 return^ 5 }\n"
             "var^ held = 5 < 1 < side()\n"
             "if^ held { return^ 99 }\n"
             "return^ m\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 11.9: the four orderings are read off the one comparison a type
    // writes, by asking which side of zero its answer falls on.
    LHAT_TEST("an ordering is read off op^<=>");
    run_text(&r,
             "var^ V = def^{\n"
             "  self^{ n := 0 },\n"
             "  op^<=> := f^self^, o:V -> number^ { return^ self^.n - o.n },\n"
             "}\n"
             "var^ a = V.new()\n"
             "var^ b = V.new()\n"
             "a.n := 3\n"
             "b.n := 7\n"
             "if^ a < b and^ b > a and^ a <= b { return^ 1 }\n"
             "return^ 0\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // and so is equality, for a type that says how it orders: two of them
    // that compare the same are equal, where 14.2 alone would say no.
    LHAT_TEST("and so is equality");
    run_text(&r,
             "var^ V = def^{\n"
             "  self^{ n := 0 },\n"
             "  op^<=> := f^self^, o:V -> number^ { return^ self^.n - o.n },\n"
             "}\n"
             "var^ a = V.new()\n"
             "var^ b = V.new()\n"
             "if^ a = b and^ !(a is^ b) { return^ 1 }\n"
             "return^ 0\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 14.2 still answers for a table that says nothing about how it orders.
    LHAT_TEST("a table with no op^<=> is equal only to itself");
    run_text(&r,
             "var^ t = { a := 1 }\n"
             "var^ u = { a := 1 }\n"
             "if^ t = t and^ !(t = u) { return^ 1 }\n"
             "return^ 0\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 11.9: number^ and string^ order their own, which is what a written
    // '"a" < "b"' had nothing to reach for before.
    LHAT_TEST("the built-in types order their own");
    run_text(&r,
             "if^ \"a\" < \"b\" and^ \"abc\" < \"abd\" and^ !(\"b\" < \"a\") "
             "{ return^ 1 <=> 2 }\n"
             "return^ 9\n");
    CHECK_INTEGER(&r, -1);
    run_dispose(&r);

    // 11.3改: with the built-in on the left there is no other way in --
    // number^ carries the arithmetic and takes nothing but its own kind, and
    // no program adds to what it carries. A self^ written last is the answer.
    LHAT_TEST("a self^ written last answers from the right");
    run_text(&r,
             "var^ Vec = def^{\n"
             "  self^{ n := 10 },\n"
             "  op^+ := f^lhs:number^, self^ -> number^ "
             "{ return^ lhs + self^.n },\n"
             "}\n"
             "return^ 1 + Vec.new()\n");
    CHECK_INTEGER(&r, 11);
    run_dispose(&r);

    // 11.3's order stands: the right side is reached only where the left
    // carries no answer for this operand. Both claiming the same pair is the
    // left one answering, with the other never reached.
    LHAT_TEST("and the left one still answers first");
    run_text(&r,
             "var^ Vec = def^{\n"
             "  self^{ n := 10 },\n"
             "  op^+ := f^self^, o:number^ -> number^ { return^ 1 },\n"
             "  overload^ op^+ := f^lhs:number^, self^ -> number^ "
             "{ return^ 2 },\n"
             "}\n"
             "var^ v = Vec.new()\n"
             "return^ (v + 1) * 10 + (1 + v)\n");
    CHECK_INTEGER(&r, 12);  // the left arm, then the right one
    run_dispose(&r);

    // 14.8's number^ carries all seven built in, so ordinary arithmetic keeps
    // the instructions it had and pays nothing for the lookup.
    LHAT_TEST("numbers keep their own instructions");
    run_text(&r, "return^ 1 + 2 * 3 - 4\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    LHAT_TEST("a structure with no op^ for it is refused");
    run_text(&r,
             "var^ t = { a := 1 }\n"
             "return^ t + 1\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    LHAT_TEST("a structure with no '..' cannot answer");
    run_text(&r,
             "var^ t = { a := 1 }\n"
             "var^ u = { b := 2 }\n"
             "return^ t .. u\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    // 11.3 leaves the rest to the operator's own definition, which needs
    // op^. 5.1 has the instruction refuse what it cannot do.
    LHAT_TEST("joining a number to a string is refused at run time");
    run_text(&r, "return^ \"a\" .. 1\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    LHAT_TEST("arithmetic on a string is refused at run time");
    run_text(&r, "return^ \"a\" + 1\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);
}

// 02 の 14 章: the one data structure.
static void test_tables(void)
{
    Run r;

    LHAT_TEST("an empty table is a table");
    run_text(&r, "var^ t = { }\nreturn^ t\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_OK);
    LHAT_CHECK(lhat_is_object_kind(r.ran.value, LHAT_OBJECT_TABLE), "a table");
    run_dispose(&r);

    LHAT_TEST("a named member reads back");
    run_text(&r, "var^ t = { a := 1, b := 2 }\nreturn^ t.b\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    // 04 の 11.3: t.foo and t[k] differ in what the checker knows, not in
    // where the machine looks.
    LHAT_TEST("the two spellings reach one place");
    run_text(&r, "var^ t = { a := 1 }\nreturn^ t[\"a\"]\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("a positional entry counts from one");
    run_text(&r, "var^ t = { 10, 20, 30 }\nreturn^ t.1 + t.3\n");
    CHECK_INTEGER(&r, 40);
    run_dispose(&r);

    LHAT_TEST("keyed and positional entries mix");
    run_text(&r, "var^ t = { 10, a := 1, 20 }\nreturn^ t.2 + t.a\n");
    CHECK_INTEGER(&r, 21);
    run_dispose(&r);

    LHAT_TEST("a value may be any expression");
    run_text(&r, "var^ n = 3\nvar^ t = { a := n * 2 }\nreturn^ t.a\n");
    CHECK_INTEGER(&r, 6);
    run_dispose(&r);

    LHAT_TEST("tables nest");
    run_text(&r, "var^ t = { a := { b := 5 } }\nreturn^ t.a.b\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    // 14.14改: an entry introduces a member, which 8.6 spells '='. What that
    // costs is that a comparison standing on its own has to say so.
    LHAT_TEST("an entry written with '=' is a member, not a comparison");
    run_text(&r,
             "var^ a = 1\n"
             "var^ t = { a = 0, b = 2 }\n"
             "return^ t.a * 10 + t.b\n");
    CHECK_INTEGER(&r, 2);  // t.a is 0, not the comparison's false^
    run_dispose(&r);

    LHAT_TEST("and one in brackets is the comparison");
    run_text(&r,
             "var^ a = 1\n"
             "var^ t = { (a = 1), (a = 2) }\n"
             "if^ t[1] {\n"
             "  if^ t[2] { return^ 0 }\n"
             "  return^ 1\n"
             "}\n"
             "return^ 2\n");
    CHECK_INTEGER(&r, 1);  // true^ then false^
    run_dispose(&r);

    // 11.3: a table is a mapping, so there is no out of range -- only a key
    // that is there and one that is not.
    LHAT_TEST("a missing key answers nil^ rather than failing");
    run_text(&r, "var^ t = { }\nreturn^ t[\"nowhere\"]\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    LHAT_TEST("a member is a place that ':=' reaches");
    run_text(&r, "var^ t = { a := 1 }\nt.a := 9\nreturn^ t.a\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    LHAT_TEST("':=' may make a member that was not there");
    run_text(&r, "var^ t = { }\nt.a := 7\nreturn^ t.a\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("an index is a place too");
    run_text(&r, "var^ t = { }\nvar^ k = \"key\"\nt[k] := 4\nreturn^ t[\"key\"]\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);

    LHAT_TEST("storing nil^ removes the key");
    run_text(&r, "var^ t = { a := 1 }\nt.a := nil^\nreturn^ t[\"a\"]\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    LHAT_TEST("nil^ cannot be a key");
    run_text(&r, "var^ t = { }\nvar^ k = nil^\nt[k] := 1\nreturn^ t\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_KEY);
    run_dispose(&r);

    LHAT_TEST("indexing something that is not a table is refused");
    run_text(&r, "var^ n = 1\nreturn^ n[\"a\"]\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    // 14.2 makes a table's identity what it is, so two literals of the same
    // shape are two tables.
    LHAT_TEST("a table is equal only to itself");
    run_text(&r, "var^ a = { x := 1 }\nvar^ b = { x := 1 }\nreturn^ a = b\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    LHAT_TEST("and a name for one is the same one");
    run_text(&r, "var^ a = { x := 1 }\nvar^ b = a\nreturn^ a = b\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // 11.6改: '=' and 'is^' agree here since 14.2 already makes a table's
    // '=' an identity comparison -- 'is^' means to keep meaning that once
    // '=' moves to comparing a table's contents instead (03 の 7 章、P7).
    LHAT_TEST("'is^' agrees with '=' on tables for now");
    run_text(&r, "var^ a = { x := 1 }\nvar^ b = { x := 1 }\nreturn^ a is^ b\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    LHAT_TEST("and a name for one is the same instance under 'is^' too");
    run_text(&r, "var^ a = { x := 1 }\nvar^ b = a\nreturn^ a is^ b\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // A table is a reference, so the two names reach one table (「シンボルはすべて参照」).
    LHAT_TEST("a table is shared rather than copied");
    run_text(&r, "var^ a = { x := 1 }\nvar^ b = a\nb.x := 5\nreturn^ a.x\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    LHAT_TEST("a table passes into a subroutine as itself");
    run_text(&r,
             "var^ bump = p^t { t.n := t.n + 1 }\n"
             "var^ t = { n := 0 }\n"
             "bump(t)\n"
             "bump(t)\n"
             "return^ t.n\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    LHAT_TEST("a subroutine may be a member");
    run_text(&r,
             "var^ t = { twice := f^n { return^ n * 2 } }\n"
             "return^ t.twice(4)\n");
    CHECK_INTEGER(&r, 8);
    run_dispose(&r);
}

// 01 の 5.4: the lexer and the parser have read $"..." from the start; this
// is what it compiles to. A hole is 02 の 14.17's tostring and the pieces
// are joined with 11.2's '..', so interpolation adds no way of building a
// string that writing it out by hand would not reach.
static void test_interpolation(void)
{
    Run r;

    LHAT_TEST("text with no hole is just the text");
    run_text(&r, "return^ $\"plain\"\n");
    CHECK_STRING(&r, "plain");
    run_dispose(&r);

    LHAT_TEST("and nothing at all is the empty string");
    run_text(&r, "return^ $\"\"\n");
    CHECK_STRING(&r, "");
    run_dispose(&r);

    LHAT_TEST("a hole is its value written down");
    run_text(&r, "var^ a = 5\nreturn^ $\"n = {a}\"\n");
    CHECK_STRING(&r, "n = 5");
    run_dispose(&r);

    // 5.4: no empty text run is made between two holes that meet.
    LHAT_TEST("two holes may meet");
    run_text(&r, "var^ a = 5\nreturn^ $\"{a}{a}\"\n");
    CHECK_STRING(&r, "55");
    run_dispose(&r);

    LHAT_TEST("a hole holds an expression, not just a name");
    run_text(&r, "return^ $\"{ 1 + 2 }\"\n");
    CHECK_STRING(&r, "3");
    run_dispose(&r);

    // 14.17: nil^ and bool^ are the bare words there, so they are here.
    LHAT_TEST("what a hole writes is what tostring answers");
    run_text(&r, "return^ $\"{nil^} {true^} {\"s\"} {3.0}\"\n");
    CHECK_STRING(&r, "nil true s 3.0");
    run_dispose(&r);

    // 5.4: the text after ':' is a format, and 14.17 hands it to a number^.
    LHAT_TEST("a format after ':' reaches tostring");
    run_text(&r, "var^ a = 5\nreturn^ $\"{a:%04d}\"\n");
    CHECK_STRING(&r, "0005");
    run_dispose(&r);

    LHAT_TEST("a bad format is the same runtime error as anywhere");
    run_text(&r, "return^ $\"{ 1 :%s}\"\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_FORMAT);
    run_dispose(&r);

    // 5.4: '{{' and '}}' spell the braces themselves.
    LHAT_TEST("doubled braces are the braces");
    run_text(&r, "return^ $\"{{literal}}\"\n");
    CHECK_STRING(&r, "{literal}");
    run_dispose(&r);

    // 5.4: a hole is scanned by the ordinary rules, so what is in it may be
    // a string literal, a table, or another interpolation.
    LHAT_TEST("a hole may hold a string of its own");
    run_text(&r, "return^ $\"{ \"in\" }\"\n");
    CHECK_STRING(&r, "in");
    run_dispose(&r);

    LHAT_TEST("and another interpolated string");
    run_text(&r, "var^ a = 5\nreturn^ $\"[{ $\"{a}\" }]\"\n");
    CHECK_STRING(&r, "[5]");
    run_dispose(&r);

    // 14.17改: a hole asks for tostring^ -- the spelling that reaches the
    // built-in on every value -- so that is also the name a written one
    // answers under. A bare `tostring` on a plain table is the writer's own
    // member and no part of this.
    LHAT_TEST("a written tostring^ is what a hole uses");
    run_text(&r,
             "var^ p = { tostring^ := f^self^ -> string^ { return^ \"P\" } }\n"
             "return^ $\"got {p}\"\n");
    CHECK_STRING(&r, "got P");
    run_dispose(&r);

    // 14.17改: and the bare one there is the writer's, so the built-in is
    // still what the hole gets. This is the line the search below stops at.
    LHAT_TEST("a bare tostring on a plain table is not what a hole uses");
    run_text(&r,
             "var^ p = { n := 1,\n"
             "  tostring := f^self^ -> string^ { return^ \"P\" } }\n"
             "return^ $\"{p.tostring()}\" .. \" \" .. $\"{p}\"\n");
    CHECK_STRING(&r, "P { n = 1, tostring = f^ }");
    run_dispose(&r);

    // 02 の 14.17 with 01 の 5.4: written first, built-in last. 14 章 reserves
    // the name on a def^, so there the bare spelling is the type's own and the
    // hole reaches it -- the same member '.tostring()' calls by hand.
    LHAT_TEST("a written tostring on a def^ instance is what a hole uses");
    run_text(&r,
             "let^ P = def^{ self^{ x := 7 },\n"
             "  tostring := f^self^ -> string^ { return^ \"P!\" } }\n"
             "return^ $\"{P.new()}\"\n");
    CHECK_STRING(&r, "P!");
    run_dispose(&r);

    LHAT_TEST("a hole runs where it stands, not where the string was made");
    run_text(&r,
             "var^ n = 1\n"
             "var^ show = f^ -> string^ { return^ $\"n is {n}\" }\n"
             "n := 2\n"
             "return^ show()\n");
    CHECK_STRING(&r, "n is 2");
    run_dispose(&r);
}

// 02 の 14.17: every value carries tostring. 14.16 above turns a value's type
// into text; this turns the value itself into text.
static void test_tostring(void)
{
    Run r;

    // The three that name themselves as literals read as words rather than
    // as the spelling a prompt would show, which is the whole of what
    // separates this from lhat_value_write.
    LHAT_TEST("nil^ is the word");
    run_text(&r, "return^ nil^.tostring()\n");
    CHECK_STRING(&r, "nil");
    run_dispose(&r);

    LHAT_TEST("and so are the two bools");
    run_text(&r, "return^ true^.tostring() .. false^.tostring()\n");
    CHECK_STRING(&r, "truefalse");
    run_dispose(&r);

    LHAT_TEST("a string^ is its own text, unquoted");
    run_text(&r, "return^ \"hi\".tostring()\n");
    CHECK_STRING(&r, "hi");
    run_dispose(&r);

    // 14.8: one type, two representations, and tostring keeps them apart the
    // same way a literal does -- 3 and 3.0 are not the same text.
    LHAT_TEST("a number^ says which representation it has");
    run_text(&r, "return^ (3).tostring() .. \" \" .. (3.0).tostring()\n");
    CHECK_STRING(&r, "3 3.0");
    run_dispose(&r);

    LHAT_TEST("a table is written the way 14 章 writes one");
    run_text(&r, "return^ { 1, \"a\" }.tostring^()\n");
    CHECK_STRING(&r, "{ 1, \"a\" }");
    run_dispose(&r);

    // The value handed over is read as text; one inside a table is written
    // as the table spells it, which is the table's spelling and not that
    // value's.
    LHAT_TEST("but what is inside it keeps the table's spelling");
    run_text(&r, "return^ { \"a\", true^ }.tostring^()\n");
    CHECK_STRING(&r, "{ \"a\", true^ }");
    run_dispose(&r);

    LHAT_TEST("a format writes the number through it");
    run_text(&r, "return^ (255).tostring(\"%x\")\n");
    CHECK_STRING(&r, "ff");
    run_dispose(&r);

    LHAT_TEST("and carries whatever text was written around it");
    run_text(&r, "return^ (42).tostring(\"n = %d units\")\n");
    CHECK_STRING(&r, "n = 42 units");
    run_dispose(&r);

    // 14.8: the conversion decides the reading, so either representation
    // answers either family.
    LHAT_TEST("the conversion decides how the number is read");
    run_text(&r,
             "return^ (3.9).tostring(\"%d\") .. \" \" .. (3).tostring(\"%.2f\")\n");
    CHECK_STRING(&r, "3 3.00");
    run_dispose(&r);

    LHAT_TEST("a per cent sign the writer wanted survives");
    run_text(&r, "return^ (50).tostring(\"%d%%\")\n");
    CHECK_STRING(&r, "50%");
    run_dispose(&r);

    // Handing snprintf a program's format unchecked would read the number as
    // a pointer, so the conversion is checked before anything is written.
    LHAT_TEST("a conversion a number^ cannot answer is refused");
    run_text(&r, "return^ (1).tostring(\"%s\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_FORMAT);
    run_dispose(&r);

    LHAT_TEST("and so is a second conversion, with one number to write");
    run_text(&r, "return^ (1).tostring(\"%d %d\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_FORMAT);
    run_dispose(&r);

    LHAT_TEST("and a format asking for no number at all");
    run_text(&r, "return^ (1).tostring(\"plain\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_FORMAT);
    run_dispose(&r);

    // The length is the machine's to supply -- a program spelling one would
    // be naming a width it cannot know.
    LHAT_TEST("and a length the writer spelled");
    run_text(&r, "return^ (1).tostring(\"%ld\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_FORMAT);
    run_dispose(&r);

    // '*' reads its width from a further argument, and there is only ever
    // the one number here.
    LHAT_TEST("and a width taken from an argument that is not there");
    run_text(&r, "return^ (1).tostring(\"%*d\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_FORMAT);
    run_dispose(&r);

    // 16.3's rule for iterate, which 14.17 follows: the built-in answers
    // only where nothing was written under that name.
    LHAT_TEST("a written tostring wins over the built-in");
    run_text(&r,
             "var^ t = { tostring := f^self^ -> string^ { return^ \"mine\" } }\n"
             "return^ t.tostring()\n");
    CHECK_STRING(&r, "mine");
    run_dispose(&r);

    LHAT_TEST("and one written into a def^ answers for its instances");
    run_text(&r,
             "var^ P = def^{ self^{ x := 7 },\n"
             "  tostring := f^self^ -> string^ { return^ \"P\" } }\n"
             "return^ P.new().tostring()\n");
    CHECK_STRING(&r, "P");
    run_dispose(&r);

    // Only a number^ carries the second signature, so this call is neither
    // of the two ways of writing the value down.
    LHAT_TEST("nothing but a number^ takes a format");
    run_text(&r, "return^ \"x\".tostring(\"%d\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_ARITY);
    run_dispose(&r);

    LHAT_TEST("and a third argument is not one of them either");
    run_text(&r, "return^ (1).tostring(\"%d\", \"%d\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_ARITY);
    run_dispose(&r);

    LHAT_TEST("a coroutine carries it beside its own operations");
    run_text(&r,
             "var^ gen = p^ { yield^ 1 }\n"
             "return^ gen().tostring()\n");
    CHECK_STRING(&r, "c^");
    run_dispose(&r);

    // 14.17改: on a table the writer wrote, every name is the writer's, so
    // the built-in is reached by the hat spelling alone. 14.11 already wrote
    // new that way and this is the same rule kept to.
    LHAT_TEST("a plain table answers the hat spelling");
    run_text(&r, "return^ { 1 }.tostring^()\n");
    CHECK_STRING(&r, "{ 1 }");
    run_dispose(&r);

    LHAT_TEST("and holds nothing under the bare one");
    run_text(&r,
             "var^ t = { 1 }\n"
             "return^ t.tostring isa^ nil^\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // A def^ is the other way round: 14 章 reserves these names on one, which
    // is what the hat was saying all along, so both spellings answer.
    LHAT_TEST("an instance answers either spelling");
    run_text(&r,
             "var^ P = def^{ self^{ x := 7 } }\n"
             "return^ P.new().tostring() .. \" \" .. P.new().tostring^()\n");
    CHECK_STRING(&r, "{ x = 7 } { x = 7 }");
    run_dispose(&r);

    // 14.9: the definition itself is a table nothing points at as one, so it
    // carries a mark of its own -- without it this would read as plain.
    LHAT_TEST("and so does the definition it came from");
    run_text(&r,
             "var^ P = def^{ self^{ x := 7 } }\n"
             "return^ P.tostring()\n");
    CHECK_STRING(&r, "{ new = f^ }");
    run_dispose(&r);

    // 01 の 2.3: the hat is part of the name, which is the whole of what
    // makes the two spellings two names.
    LHAT_TEST("a written tostring^ is what the hat spelling then finds");
    run_text(&r,
             "var^ t = { tostring^ := f^self^ -> string^ { return^ \"mine\" } }\n"
             "return^ t.tostring^()\n");
    CHECK_STRING(&r, "mine");
    run_dispose(&r);

    LHAT_TEST("and a bare tostring beside it is a different member");
    run_text(&r,
             "var^ t = { tostring := 1, tostring^ := f^self^ -> string^ {\n"
             "  return^ \"mine\" } }\n"
             "return^ t.tostring^() .. t.tostring.tostring^()\n");
    CHECK_STRING(&r, "mine1");
    run_dispose(&r);
}

// 02 の 14.18: the three that are not calls. A count is the shape of the
// value rather than an operation on it, so there are no parentheses to write
// -- which is what these pin, along with which reading each spelling is.
static void test_counting(void)
{
    Run r;

    LHAT_TEST("a table answers how long its run is");
    run_text(&r, "var^ t = { 1, 2, 3 }\nreturn^ t.length^ * 10 + t.len^\n");
    CHECK_INTEGER(&r, 33);
    run_dispose(&r);

    // 14.18: the run is the dense half (03 の 2.2), and count^ is both halves.
    LHAT_TEST("and how much it holds altogether");
    run_text(&r,
             "var^ t = { 1, 2, a := 9, b := 8 }\n"
             "return^ t.length^ * 10 + t.count^\n");
    CHECK_INTEGER(&r, 24);
    run_dispose(&r);

    LHAT_TEST("a table with keys alone has no run");
    run_text(&r,
             "var^ t = { a := 1 }\nreturn^ t.length^ * 10 + t.count^\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("and an empty one answers zero either way");
    run_text(&r, "var^ t = { }\nreturn^ t.length^ + t.count^\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 14.7: what an instance reads through its definition is shared, so it is
    // not one of that instance's own elements.
    LHAT_TEST("an instance counts its fields and not the members it sees");
    run_text(&r,
             "var^ P = def^{ self^{ x := 1, y := 2 },\n"
             "  m := f^self^ -> number^ { return^ 1 } }\n"
             "return^ P.new().count^\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    // 14.18 with 14.17改: written wins, the same order tostring^ is read in.
    LHAT_TEST("a written length^ is what answers");
    run_text(&r, "var^ t = { 1, 2, 3, length^ := 99 }\nreturn^ t.length^\n");
    CHECK_INTEGER(&r, 99);
    run_dispose(&r);

    // 14.18: the hat is not optional. `length` is a word a writer reaches for
    // first, so on a table the bare one is theirs whatever kind of table it is.
    LHAT_TEST("and a bare length is the writer's name");
    run_text(&r, "var^ t = { 1, 2, 3 }\nreturn^ t.length isa^ nil^\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // 01 の 1 章: the source is UTF-8, so the two readings of a string^ part
    // company the moment anything is not ASCII.
    LHAT_TEST("a string answers its code points and its bytes apart");
    run_text(&r,
             "return^ \"abc\".length * 1000 + \"abc\".size * 100 +\n"
             "       \"\\u{3042}\\u{3044}\\u{3046}\".length * 10 +\n"
             "       \"\\u{3042}\\u{3044}\\u{3046}\".size\n");
    CHECK_INTEGER(&r, 3339);
    run_dispose(&r);

    LHAT_TEST("len reads a string the same way length does");
    run_text(&r, "return^ \"\\u{3042}bc\".len\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 14.18改: and the hat spelling is not a second way of writing it. The
    // hat is what keeps a built-in off a name the writer may mean for
    // something else, and nothing can be written on a string^.
    LHAT_TEST("a string^ has no hat spelling of these");
    run_text(&r, "return^ \"abc\".length^\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    // 14.18: the count is fixed when the string is made, and joining two adds
    // the two counts -- so a string built at run time answers what a literal
    // spelling the same bytes answers.
    LHAT_TEST("a joined string counts what its halves counted");
    run_text(&r,
             "var^ s = \"\\u{3042}\" .. \"bc\" .. \"\\u{3044}\"\n"
             "return^ s.length * 100 + s.size\n");
    CHECK_INTEGER(&r, 408);
    run_dispose(&r);

    LHAT_TEST("the three readings of a string^ are three words");
    run_text(&r,
             "return^ \"\\u{3042}bc\".length * 100 + \"\\u{3042}bc\".len * 10 +\n"
             "       \"\\u{3042}bc\".size\n");
    CHECK_INTEGER(&r, 335);
    run_dispose(&r);
}

// 02 の 14.19: a run of a string^'s characters. Ordinals start at 1 and count
// from the end when negative, both ends are included, and a range that does
// not stand answers the empty string rather than an error.
static void test_substring(void)
{
    Run r;
    // あいうえお throughout, so a wrong unit (bytes for characters) answers a
    // wrong string rather than a shorter one.
    static const char *five = "var^ s = \"\\u{3042}\\u{3044}\\u{3046}\\u{3048}\\u{304a}\"\n";
    char text[512];

    LHAT_TEST("one ordinal takes the rest of the string");
    snprintf(text, sizeof text, "%s%s", five,
             "return^ s.substr(1) .. \"|\" .. s.substr(3) .. \"|\" ..\n"
             "        s.substr(-2)\n");
    run_text(&r, text);
    CHECK_STRING(&r, "あいうえお|うえお|えお");
    run_dispose(&r);

    LHAT_TEST("two ordinals take a run, both ends included");
    snprintf(text, sizeof text, "%s%s", five,
             "return^ s.substr(2, 3) .. \"|\" .. s.substr(-4, 3) .. \"|\" ..\n"
             "        s.substr(1, -1) .. \"|\" .. s.substr(2, -2)\n");
    run_text(&r, text);
    CHECK_STRING(&r, "いう|いう|あいうえお|いうえ");
    run_dispose(&r);

    // 14.19: one member under three names.
    LHAT_TEST("the three names are one member");
    snprintf(text, sizeof text, "%s%s", five,
             "return^ s.substring(2, 3) .. s.substr(2, 3) .. s.sub(2, 3)\n");
    run_text(&r, text);
    CHECK_STRING(&r, "いういういう");
    run_dispose(&r);

    // 14.18改: three names, and no hat spelling of any of them.
    LHAT_TEST("and the hat spelling is not a fourth");
    snprintf(text, sizeof text, "%s%s", five, "return^ s.sub^(2, 3)\n");
    run_text(&r, text);
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    // 04 の 11.3's line: what is not there is not a mistake. A range that
    // does not stand is empty, not an error and not nil^.
    LHAT_TEST("a range that does not stand is the empty string");
    snprintf(text, sizeof text, "%s%s", five,
             "return^ \"[\" .. s.substr(0) .. s.substr(9) .. s.substr(3, 2) ..\n"
             "        s.substr(1, 0) .. \"\".substr(1) .. \"]\"\n");
    run_text(&r, text);
    CHECK_STRING(&r, "[]");
    run_dispose(&r);

    // 14.19: a division answers a real, and an ordinal that came out of one
    // is read rather than refused. floor(x + 0.5), so a half goes to the
    // far end -- the one rounding that commutes with resolving a negative.
    LHAT_TEST("a fractional ordinal is rounded, halves to the far end");
    snprintf(text, sizeof text, "%s%s", five,
             "return^ s.substr(2.5) .. \"|\" .. s.substr(2.4) .. \"|\" ..\n"
             "        s.substr(-2.5)\n");
    run_text(&r, text);
    CHECK_STRING(&r, "うえお|いうえお|えお");
    run_dispose(&r);

    // A string never changes, so the whole of one is that one -- a copy
    // would be a second name for the same bytes.
    LHAT_TEST("the whole of a string is the string itself");
    snprintf(text, sizeof text, "%s%s", five,
             "if^ s.substr(1) is^ s and^ s.substr(1, -1) is^ s and^\n"
             "   !(s.substr(2) is^ s) { return^ 1 }\n"
             "return^ 0\n");
    run_text(&r, text);
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 14.18 and 14.19 read the same unit: one character of あいうえお is one
    // character and three bytes.
    LHAT_TEST("the run is measured in characters, not bytes");
    snprintf(text, sizeof text, "%s%s", five,
             "return^ s.substr(2, 2).size * 10 + s.substr(2, 2).length\n");
    run_text(&r, text);
    CHECK_INTEGER(&r, 31);
    run_dispose(&r);

    // 14.19改: at(i) is substr(i, i), so every rule above is its rule too.
    LHAT_TEST("at takes the one character the ordinal names");
    snprintf(text, sizeof text, "%s%s", five,
             "return^ s.at(2) .. \"|\" .. s.at(-1) .. \"|\" .. s.at(2.5) ..\n"
             "        \"|\" .. s.at(1)\n");
    run_text(&r, text);
    CHECK_STRING(&r, "い|お|う|あ");
    run_dispose(&r);

    LHAT_TEST("and answers the empty string where there is no character");
    snprintf(text, sizeof text, "%s%s", five,
             "return^ \"[\" .. s.at(0) .. s.at(6) .. s.at(-9) ..\n"
             "        \"\".at(1) .. \"]\"\n");
    run_text(&r, text);
    CHECK_STRING(&r, "[]");
    run_dispose(&r);

    LHAT_TEST("one ordinal is the whole of its shape");
    snprintf(text, sizeof text, "%s%s", five, "return^ s.at(1, 2)\n");
    run_text(&r, text);
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_ARITY);
    run_dispose(&r);

    // 14.19改 takes the name only where 14.19's member is, so a table keeps
    // `at` for whatever the writer put under it -- a field being the ordinary
    // case, as sample/24.lh's reading position is.
    LHAT_TEST("at on a table is the writer's own");
    run_text(&r,
             "var^ P = def^{ self^{ at := 7 } }\n"
             "return^ P.new().at\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);
}

// 02 の 14.17改2: tostring read backwards, and only a string^ carries it. The
// argument-less form runs 01 の 10 章's own grammar, so most of these are
// pinning that the literal grammar is what arrived rather than a second one
// written beside it.
static void test_tonumber(void)
{
    Run r;

    LHAT_TEST("digits are the number they spell");
    run_text(&r, "return^ \"123\".tonumber()\n");
    CHECK_INTEGER(&r, 123);
    run_dispose(&r);

    // 14.8: one type, two representations. Which one a text names is the
    // literal grammar's answer, not a rounding this makes up.
    LHAT_TEST("and a fraction is the real the same digits spell");
    run_text(&r, "return^ \"3.5\".tonumber()\n");
    CHECK_REAL(&r, 3.5);
    run_dispose(&r);

    LHAT_TEST("an integer stays an integer, so the two write apart");
    run_text(&r,
             "return^ \"3\".tonumber().tostring^() .. \" \" ..\n"
             "        \"3.0\".tonumber().tostring^()\n");
    CHECK_STRING(&r, "3 3.0");
    run_dispose(&r);

    // 01 の 10.1 and 10.2, reached without a word of grammar written here.
    LHAT_TEST("the bases the literal grammar has");
    run_text(&r, "return^ \"0xff\".tonumber()\n");
    CHECK_INTEGER(&r, 255);
    run_dispose(&r);

    run_text(&r, "return^ \"0b1010\".tonumber()\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    run_text(&r, "return^ \"0o777\".tonumber()\n");
    CHECK_INTEGER(&r, 511);
    run_dispose(&r);

    LHAT_TEST("the separator it allows");
    run_text(&r, "return^ \"1_000\".tonumber()\n");
    CHECK_INTEGER(&r, 1000);
    run_dispose(&r);

    LHAT_TEST("and the exponent, which makes a real of it");
    run_text(&r, "return^ \"1e3\".tonumber()\n");
    CHECK_REAL(&r, 1000.0);
    run_dispose(&r);

    // Section 10 has no sign in it -- '-1' is 11 章's unary minus applied to
    // a literal -- so tonumber reads the sign itself rather than leaving a
    // text no writer would call anything but a number unreadable.
    LHAT_TEST("a sign in front is read");
    run_text(&r, "return^ \"-5\".tonumber()\n");
    CHECK_INTEGER(&r, -5);
    run_dispose(&r);

    run_text(&r, "return^ \"+5\".tonumber()\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    run_text(&r, "return^ \"-2.5\".tonumber()\n");
    CHECK_REAL(&r, -2.5);
    run_dispose(&r);

    LHAT_TEST("but only one, and glued to the digits");
    run_text(&r, "return^ \"- 5\".tonumber()\n");
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    run_text(&r, "return^ \"--5\".tonumber()\n");
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    LHAT_TEST("whitespace around it is allowed");
    run_text(&r, "return^ \"  12  \".tonumber()\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    // Trivia is skipped by the scan, so leaving the trimming to it would
    // make a comment part of a number. It is not one.
    LHAT_TEST("a comment beside it is not");
    run_text(&r, "return^ \"12 # hi\".tonumber()\n");
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    LHAT_TEST("nor is a second number");
    run_text(&r, "return^ \"1 2\".tonumber()\n");
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    // 14.17改2 answers nil^ rather than faulting: a text holding no number^
    // is data, and reading data is not a mistake.
    LHAT_TEST("a text that names no number^ answers nil^");
    run_text(&r, "return^ \"abc\".tonumber()\n");
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    run_text(&r, "return^ \"\".tonumber()\n");
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    // 10.3 refuses this in a unit, and the same refusal arrives here.
    run_text(&r, "return^ \"12abc\".tonumber()\n");
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    LHAT_TEST("and so does one naming a number no number^ can hold");
    run_text(&r, "return^ \"99999999999999999999\".tonumber()\n");
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    run_text(&r, "return^ \"9223372036854775808\".tonumber()\n");
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    LHAT_TEST("the far end of the range is held, sign and all");
    run_text(&r, "return^ \"-9223372036854775808\".tonumber()\n");
    CHECK_INTEGER(&r, INT64_MIN);
    run_dispose(&r);

    run_text(&r, "return^ \"9223372036854775807\".tonumber()\n");
    CHECK_INTEGER(&r, INT64_MAX);
    run_dispose(&r);

    // 14.17's format written the other way round.
    LHAT_TEST("a format reads the number through it");
    run_text(&r, "return^ \"ff\".tonumber(\"%x\")\n");
    CHECK_INTEGER(&r, 255);
    run_dispose(&r);

    LHAT_TEST("and matches whatever text was written around it");
    run_text(&r, "return^ \"n = 42 units\".tonumber(\"n = %d units\")\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    LHAT_TEST("the two are a pair -- what one writes the other reads");
    run_text(&r, "return^ (255).tostring(\"%x\").tonumber(\"%x\")\n");
    CHECK_INTEGER(&r, 255);
    run_dispose(&r);

    // 14.8: the conversion decides the reading, on this side too.
    LHAT_TEST("a real conversion answers a real");
    run_text(&r, "return^ \"3.5\".tonumber(\"%f\")\n");
    CHECK_REAL(&r, 3.5);
    run_dispose(&r);

    run_text(&r, "return^ \"3\".tonumber(\"%f\").tostring^()\n");
    CHECK_STRING(&r, "3.0");
    run_dispose(&r);

    LHAT_TEST("text the format does not match is nil^, not an error");
    run_text(&r, "return^ \"zz\".tonumber(\"%x\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    LHAT_TEST("nor does a match that leaves bytes over count");
    run_text(&r, "return^ \"42 units\".tonumber(\"%d\")\n");
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    // The other half of 14.17's line: the format is the writer's, so a bad
    // one is the writer's mistake and faults where a bad text does not.
    LHAT_TEST("a format a number^ cannot be read through is an error");
    run_text(&r, "return^ \"1\".tonumber(\"%s\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_FORMAT);
    run_dispose(&r);

    run_text(&r, "return^ \"1 2\".tonumber(\"%d %d\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_FORMAT);
    run_dispose(&r);

    run_text(&r, "return^ \"1\".tonumber(\"plain\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_FORMAT);
    run_dispose(&r);

    // scanf reads '*' as "match it and assign nothing", which would leave no
    // number at all, and has no precision to read '.' as.
    run_text(&r, "return^ \"1\".tonumber(\"%*d\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_FORMAT);
    run_dispose(&r);

    run_text(&r, "return^ \"1\".tonumber(\"%.2f\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_FORMAT);
    run_dispose(&r);

    run_text(&r, "return^ \"1\".tonumber(\"%ld\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_FORMAT);
    run_dispose(&r);

    LHAT_TEST("a width is the one thing scanf carries that this passes on");
    run_text(&r, "return^ \"1234\".tonumber(\"%2d34\")\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    LHAT_TEST("'%%' is a per cent sign the writer wanted");
    run_text(&r, "return^ \"50%\".tonumber(\"%d%%\")\n");
    CHECK_INTEGER(&r, 50);
    run_dispose(&r);

    LHAT_TEST("an unsigned conversion stops at what a number^ can hold");
    run_text(&r, "return^ \"ffffffffffffffff\".tonumber(\"%x\")\n");
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    LHAT_TEST("a third argument is neither of the two signatures");
    run_text(&r, "return^ \"1\".tonumber(\"%d\", \"%d\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_ARITY);
    run_dispose(&r);

    LHAT_TEST("the format has to be a string^");
    run_text(&r, "return^ \"1\".tonumber(2)\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    // 14.18改: a string^ is a value a writer cannot add names to, so there is
    // nothing here for a hat to keep the built-in off -- and what only a
    // string^ carries has no hat spelling at all.
    LHAT_TEST("there is no hat spelling of it");
    run_text(&r, "return^ \"7\".tonumber^()\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    // Only a string^ carries it -- it is the one value a number^ can be read
    // out of, where every value can be written down.
    LHAT_TEST("nothing else carries it");
    run_text(&r, "return^ (1).tonumber()\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    run_text(&r, "return^ nil^.tonumber()\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    LHAT_TEST("and on a table it is a name like any other");
    run_text(&r, "return^ { n := 1 }.tonumber^ isa^ nil^\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);
}

int main(void)
{
    test_names();
    test_strings();
    test_tables();
    test_interpolation();
    test_tostring();
    test_counting();
    test_substring();
    test_tonumber();
    return lhat_test_report("test_vm_data");
}
