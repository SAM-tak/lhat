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

// 02 の 16.3. The focus of an in^ loop is bound from what the walk yields,
// which is the one place a for^ header defines names rather than reading
// them. Until this, they were read -- and found nothing.
static void test_walking(void)
{
    Unit u;

    LHAT_TEST("the focus of an in^ loop is in scope inside the body");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "for^ x in^ gen() { var^ n = x }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and carries what the coroutine yields");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "for^ x in^ gen() { var^ n : number^ = x }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("so the wrong type is caught in the body");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "for^ x in^ gen() { var^ s : string^ = x }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 16.7: the focus belongs to the loop, which is a scope of its own.
    LHAT_TEST("and it does not outlive the loop");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "for^ x in^ gen() { }\n"
               "var^ n = x\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
    unit_dispose(&u);

    // 16.3: the annotation form of the focus.
    LHAT_TEST("a written focus type is what the name gets");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "for^ x:number^ in^ gen() { var^ n = x }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and it has to admit what the walk yields");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "for^ x:string^ in^ gen() { var^ n = x }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 16.3: a coroutine answers iterate() with itself, so one held in a name
    // walks without being called again.
    LHAT_TEST("a coroutine walks as itself");
    check_text(&u,
               "var^ gen = p^ { yield^ 1 }\n"
               "var^ c = gen()\n"
               "for^ x in^ c { var^ n : number^ = x }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 16.3 with 13.8改: one name over a table takes the sequence half's
    // values in order -- 'for^ i from^ 1 to^ the length { t[i] }' written as
    // a walk. The keyed half is not visited, and no pair exists to receive.
    LHAT_TEST("one name over a table takes the values");
    check_text(&u,
               "var^ t = { 1, 2 }\n"
               "for^ v in^ t { var^ n : number^ = v }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and their type is the elements', not the pair's");
    check_text(&u,
               "var^ t = { 1, 2 }\n"
               "for^ v in^ t { var^ s : string^ = v }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 13.10: several names take the value apart by position, and 14 章 makes
    // position 1 the key and position 2 the value of the pair.
    LHAT_TEST("several names take the pair apart by position");
    check_text(&u,
               "var^ t = { 10, 20 }\n"
               "for^ k, v in^ t { var^ n : number^ = v }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("so the wrong type is caught for a position too");
    check_text(&u,
               "var^ t = { 10, 20 }\n"
               "for^ k, v in^ t { var^ s : string^ = v }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 14 章: a table is a sequence and a mapping at once, so what a key can
    // be depends on which halves the table has.
    LHAT_TEST("the key of a walk over the dense part is a number^");
    check_text(&u,
               "var^ t = { 10, 20 }\n"
               "for^ k, v in^ t { var^ n : number^ = k }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and of one over written members a string^");
    check_text(&u,
               "var^ t = { a := \"x\" }\n"
               "for^ k, v in^ t { var^ s : string^ = k }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a table with both halves yields either");
    check_text(&u,
               "var^ t = { 10, a := \"x\" }\n"
               "for^ k, v in^ t { var^ x : number^|string^ = v }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and neither half alone covers it");
    check_text(&u,
               "var^ t = { 10, a := \"x\" }\n"
               "for^ k, v in^ t { var^ n : number^ = v }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 16.3: a written focus type is checked against the position it takes.
    LHAT_TEST("a focus annotation is checked against its position");
    check_text(&u,
               "var^ t = { 10, 20 }\n"
               "for^ k, v:string^ in^ t { var^ n = v }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("a written iterate is what the walk comes from");
    check_text(&u,
               "var^ t = { iterate^ := f^ { return^ p^ { yield^ 9 }() } }\n"
               "for^ v in^ t { var^ s : string^ = v }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and a definition may answer with one");
    check_text(&u,
               "var^ Range = def^{\n"
               "  self^{ upto := 0 },\n"
               "  override^new := f^ n { self^{ upto := n } },\n"
               "  iterate^ := f^self^ {\n"
               "    return^ p^ { yield^ 1 }()\n"
               "  },\n"
               "}\n"
               "for^ v in^ Range.new(4) { var^ n : number^ = v }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 16.3: what in^ walks has to answer with a coroutine, the same demand
    // 15.8 makes of await^.
    LHAT_TEST("walking something with no iterate is reported");
    check_text(&u, "for^ x in^ 5 { var^ n = x }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_COROUTINE);
    unit_dispose(&u);

    LHAT_TEST("and so is an iterate that answers something else");
    check_text(&u,
               "var^ t = { iterate^ := f^ -> number^ { return^ 1 } }\n"
               "for^ x in^ t { var^ n = x }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_COROUTINE);
    unit_dispose(&u);
}

// 02 の 14 章 makes a table a sequence as well as a mapping. The keyed half
// was described by name from the start; the sequence half was dropped on the
// floor, so nothing downstream of it -- t[1], a walk's pair -- had
// anything to read.
static void test_positions(void)
{
    Unit u;

    LHAT_TEST("a positional entry carries its type");
    check_text(&u,
               "var^ t = { 10, 20 }\n"
               "var^ n : number^ = t[1]\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("so the wrong type at a position is caught");
    check_text(&u,
               "var^ t = { 10, 20 }\n"
               "var^ s : string^ = t[1]\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and each position keeps its own");
    check_text(&u,
               "var^ t = { 10, \"a\" }\n"
               "var^ s : string^ = t[2]\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The machine numbers the dense part in written order, skipping the
    // entries that carry a key -- so the types have to be counted the same.
    LHAT_TEST("a keyed entry takes no position from the ones after it");
    check_text(&u,
               "var^ t = { 10, a := \"x\", 20 }\n"
               "var^ n : number^ = t[2]\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 04 の 11.3: a position the type does not list is one it does not
    // promise, so what comes back is what it does say the sequence half
    // holds, or nil^. Reading is bounded by what was declared -- 't.foo' for
    // a member the type never mentions is a static error, and a position is
    // that same question asked with digits. 14.10's "at least these" says
    // which values fit a type, not what may be read off one.
    LHAT_TEST("a position the type does not mention may be absent");
    check_text(&u,
               "var^ t = { 10 }\n"
               "var^ s : string^ = t[9]\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and what it does say is what the rest of it is");
    check_text(&u,
               "var^ t = { 10 }\n"
               "var^ n : number^ = t[9] ?? 0\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a written key reaches the member of that name");
    check_text(&u,
               "var^ t = { a := 1 }\n"
               "var^ s : string^ = t[\"a\"]\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 14.14改: '[ ... ] :=' writes a key that 01 の 6 章 leaves unwritable as
    // a name. A literal one still tells the type what it named.
    LHAT_TEST("an integer key reaches the sequence half");
    check_text(&u,
               "var^ t = { [0] := 7 }\n"
               "var^ s : string^ = t[0]\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // Lua's rule, which 14 章 follows: t.a and t["a"] are one key.
    LHAT_TEST("and a string key is the member of that name");
    check_text(&u,
               "var^ t = { [\"a\"] := 7 }\n"
               "var^ s : string^ = t.a\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 14.10 lets a table carry more than its type lists, so a key only known
    // when it runs leaves the type saying nothing about it.
    LHAT_TEST("a computed key leaves the type quiet");
    check_text(&u,
               "var^ k = 3\n"
               "var^ t = { [k] := 7 }\n"
               "var^ s : string^ = t[3]\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("but the key itself is checked");
    check_text(&u, "var^ t = { [nowhere] := 7 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
    unit_dispose(&u);

    // 04 の 11.3: nil^ spells absence, so it cannot also be a key. A key that
    // can only ever be one is decided here rather than left to the machine.
    LHAT_TEST("a key that can only be nil^ is reported");
    check_text(&u, "var^ t = { [nil^] := 7 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_BAD_KEY);
    unit_dispose(&u);

    LHAT_TEST("while one that merely might be is left to run time");
    check_text(&u,
               "var^ x : number^|nil^ = 1\n"
               "var^ t = { [x] := 7 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A keyed entry takes no place in the sequence, exactly as a named one
    // does not.
    LHAT_TEST("a computed key takes no position");
    check_text(&u,
               "var^ t = { 10, [\"k\"] := 20, 30 }\n"
               "var^ s : string^ = t[2]\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 14.10: the sequence half can be written down as well as inferred --
    // types listed with no name in front of them, in order.
    LHAT_TEST("a written type may list its positions");
    check_text(&u, "var^ t : t^{ number^, string^ } = { 1, \"a\" }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and each one is checked");
    check_text(&u, "var^ t : t^{ number^, string^ } = { 1, 2 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("a position the value does not fill is missing");
    check_text(&u, "var^ t : t^{ number^, string^ } = { 1 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 14.10 asks for at least what is listed, positions included.
    LHAT_TEST("but more positions than were asked for are fine");
    check_text(&u, "var^ t : t^{ number^ } = { 1, \"a\" }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("names and positions mix in one type");
    check_text(&u,
               "var^ t : t^{ number^, a : string^ } = { 1, a := \"x\" }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A named entry takes no place in the sequence, in the type as in the
    // literal -- so the count does not depend on where it was written.
    LHAT_TEST("and a named one takes no position, wherever it stands");
    check_text(&u,
               "var^ t : t^{ a : string^, number^ } = { 1, a := \"x\" }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 8.6: two values on the right pair off with two names on the left, and
    // 13.8改 left that path exactly as it was.
    LHAT_TEST("a multiple definition still pairs the two sides off");
    check_text(&u,
               "var^ a, b = 1, \"x\"\n"
               "var^ n : number^ = a\n"
               "var^ s : string^ = b\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and still catches a pair that does not fit");
    check_text(&u,
               "var^ a, b = 1, \"x\"\n"
               "var^ s : string^ = a\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);
}

// 04 の 11.3: 't.foo' is the static half and answers T; 't[k]' is the dynamic
// one and answers 'T|nil^'. What T is, is what the type says a key of that
// kind reaches -- and which kind decides which half of 14 章's table is being
// asked about, since a number names a position and a string names a member.
//
// A type that says nothing about that kind is left alone: there is no T to
// make the union out of, and 14.10 leaves what an undeclared table holds
// unsaid. Closing that would need a spelling for an open keyed half, which
// 't^{ ...:T }' is not -- it describes the sequence.
static void test_dynamic_key(void)
{
    Unit u;

    LHAT_TEST("a dynamic key over listed positions may be absent");
    check_text(&u,
               "var^ t = { 10, 20, 30 }\n"
               "var^ i = 2\n"
               "var^ n : number^ = t[i]\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and '??' is what 11.3 offers for it");
    check_text(&u,
               "var^ t = { 10, 20, 30 }\n"
               "var^ i = 2\n"
               "var^ n : number^ = t[i] ?? 0\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The static half is untouched: a key written out that the type does
    // list resolves to exactly that member, with no nil^ beside it.
    LHAT_TEST("a written key the type lists still answers exactly");
    check_text(&u,
               "var^ t = { 10, 20, 30 }\n"
               "var^ n : number^ = t[2]\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14 章: a number names a position and a string names a member, so the
    // two halves answer separate questions.
    LHAT_TEST("a string key reads the keyed half");
    check_text(&u,
               "var^ t = { a := 1 }\n"
               "var^ s = \"a\"\n"
               "var^ x : number^ = t[s]\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("a number key over a keyed-only type says nothing");
    check_text(&u,
               "var^ t = { a := 1 }\n"
               "var^ i = 1\n"
               "var^ s : string^ = t[i]\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a string key over a sequence-only type says nothing");
    check_text(&u,
               "var^ t = { 10 }\n"
               "var^ s = \"a\"\n"
               "var^ x : string^ = t[s]\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 3.5: a key whose own type is not decided could be either, so both
    // halves answer. Picking one would be claiming to know which.
    LHAT_TEST("an undecided key reads both halves");
    check_text(&u,
               "var^ f = f^ k -> number^ {\n"
               "    var^ t = { 10, a := \"x\" }\n"
               "    var^ n : number^ = t[k]\n"
               "    return^ 1\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("a table whose type says nothing is left alone");
    check_text(&u,
               "var^ f = f^ t:t^{}, i:number^ -> number^ { return^ t[i] }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.7's unbounded tail reached this answer already, and goes on
    // reaching it now that one road serves both.
    LHAT_TEST("an unbounded tail answers the same as before");
    check_text(&u,
               "var^ f = f^ t:t^{ ...:number^ }, i:number^ -> number^ {\n"
               "    return^ t[i]\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // The answer carries a nil^ arm, so what is written around it is judged
    // against one -- an index is not a place the checking stops.
    LHAT_TEST("and what follows an index is judged against that answer");
    check_text(&u,
               "var^ f = f^ t:t^{ string^ }, i:number^ -> bool^ {\n"
               "    return^ t[i] .. \"a\"\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL);
    unit_dispose(&u);
}

// 8.6.4. 04 の 11.3 makes what a key reads out of a table a 'T|nil^', which
// leaves a plain compound assignment with nothing to add to -- the arm has no
// operator on it. The '?' spelling says the write is skipped when the place is
// absent, so the operator is asked of the rest.
static void test_nil_safe_compound(void)
{
    Unit u;

    LHAT_TEST("a plain compound has nothing to add to an absent place");
    check_text(&u, "var^ f = f^ -> t^{ ...:number^ } { return^ { 1 } }\n"
                   "var^ t = f()\n"
                   "t[1] += 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL);
    unit_dispose(&u);

    LHAT_TEST("and the '?' spelling is what answers that");
    check_text(&u, "var^ f = f^ -> t^{ ...:number^ } { return^ { 1 } }\n"
                   "var^ t = f()\n"
                   "t[1] ?+= 1\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The nil^ comes off the place, not off everything of that shape: what
    // stands in the right-hand side is a read like any other.
    LHAT_TEST("the right-hand side keeps its own nil^");
    check_text(&u, "var^ f = f^ -> t^{ ...:number^ } { return^ { 1 } }\n"
                   "var^ t = f()\n"
                   "t[1] ?+= t[2] + 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL);
    unit_dispose(&u);

    // 11.7改's posture, carried over: a '?.' whose target can never be absent
    // is let through rather than reported, and so is this.
    LHAT_TEST("a place that can never be absent takes it anyway");
    check_text(&u, "var^ n = 1\n"
                   "n ?+= 1\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // Only the nil^ is taken off. An operator the rest does not answer is
    // still an operator the rest does not answer.
    LHAT_TEST("what is left still has to answer the operator");
    check_text(&u, "var^ f = f^ -> t^{ ...:string^ } { return^ { \"a\" } }\n"
                   "var^ t = f()\n"
                   "t[1] ?+= 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    LHAT_TEST("the concatenating one reaches a string the same way");
    check_text(&u, "var^ f = f^ -> t^{ ...:string^ } { return^ { \"a\" } }\n"
                   "var^ t = f()\n"
                   "t[1] ?..= \"b\"\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A name that was written optional is a place like any other.
    LHAT_TEST("a name declared optional takes it too");
    check_text(&u, "var^ x : number^|nil^ = 1\n"
                   "x ?+= 1\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the plain spelling on that name still reports");
    check_text(&u, "var^ x : number^|nil^ = 1\n"
                   "x += 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL);
    unit_dispose(&u);

    // 8.6.4's '?:=' asks nothing of the place -- it writes rather than reads,
    // so there is no operator to be short of and nothing here to report. What
    // it changes is which places get written, which only the machine sees.
    LHAT_TEST("the plain nil-safe assignment checks like ':='");
    check_text(&u, "var^ f = f^ -> t^{ ...:number^ } { return^ { 1 } }\n"
                   "var^ t = f()\n"
                   "t[1] ?:= 2\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and takes a place that can never be absent, as the eight do");
    check_text(&u, "var^ n = 1\n"
                   "n ?:= 2\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The value is held to what the place was declared to hold, exactly as a
    // ':=' would be -- the '?' says when the write happens, not what fits.
    LHAT_TEST("and the value still has to fit the place");
    check_text(&u, "var^ f = f^ -> t^{ ...:number^ } { return^ { 1 } }\n"
                   "var^ t = f()\n"
                   "t[1] ?:= \"text\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 8.9: a let^ binds a name nothing may write again, whichever spelling
    // asks.
    LHAT_TEST("and a let^ name refuses it the way it refuses ':='");
    check_text(&u, "let^ n = 1\n"
                   "n ?:= 2\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ASSIGN_TO_LET);
    unit_dispose(&u);
}

// 04 の 11.3: only a table holds keys. A settled type that is not one
// answers a key with nothing -- the machine refuses it, so the checker says
// so first.
static void test_not_indexable(void)
{
    Unit u;

    LHAT_TEST("a string^ takes no key -- its characters answer at(i)");
    check_text(&u,
               "let^ s = \"abc\"\n"
               "var^ ch = s[2]\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_INDEXABLE);
    unit_dispose(&u);

    LHAT_TEST("nor does a number^ or a subroutine");
    check_text(&u, "var^ x = (1)[1]\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_INDEXABLE);
    unit_dispose(&u);

    check_text(&u,
               "let^ f = f^ -> number^ { return^ 1 }\n"
               "var^ x = f[1]\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_INDEXABLE);
    unit_dispose(&u);

    LHAT_TEST("a table still does, and an undecided target stays quiet");
    check_text(&u,
               "let^ t = { 10, 20 }\n"
               "var^ x = t[1]\n"
               "let^ g = f^ v { return^ v[1] }\n");
    CHECK_NOT_REPORTED(&u, LHAT_CHECK_ERR_NOT_INDEXABLE);
    unit_dispose(&u);
}

// 14.4: the self^ receiver works in a plain table literal's methods too --
// 8.7改 took the bound name out of the initialiser, and this is the door
// that stays. self^'s type is the literal's own.
static void test_table_methods(void)
{
    Unit u;

    // 03 の 3.1③ at the focus: a walk the table's type cannot describe
    // (computed keys -- the dictionary type is still unwritten) leaves the
    // names undecided, so strict asks the focus to say them.
    LHAT_TEST("an undescribed walk asks the focus for annotations");
    check_text(&u,
               "var^ t = { [1 + 1] = \"a\" }\n"
               "for^ k, v in^ t { }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TYPE_UNDECIDED);
    unit_dispose(&u);

    LHAT_TEST("and the annotations settle it");
    check_text(&u,
               "var^ t = { [1 + 1] = \"a\" }\n"
               "var^ all = \"\"\n"
               "for^ k:number^, v:string^ in^ t { all := all .. v }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a described walk still needs none");
    check_text(&u,
               "var^ t = { a = 1, b = 2 }\n"
               "var^ n = 0\n"
               "for^ k, v in^ t { n := n + v }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a plain table's method reads itself through self^");
    check_text(&u,
               "let^ u = {\n"
               "    hidden = 42,\n"
               "    read^ = f^self^ -> number^ { return^ self^.hidden }\n"
               "}\n"
               "var^ n : number^ = u.read^()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The named signatures are seeded first, annotations only, so a body
    // can call a method declared after it.
    LHAT_TEST("and calls a method declared after it, when annotated");
    check_text(&u,
               "let^ u = {\n"
               "    first = f^self^ -> number^ { return^ self^.second() },\n"
               "    second = f^self^ -> number^ { return^ 2 }\n"
               "}\n"
               "var^ n : number^ = u.first()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 16.3: a written iterate^ makes the literal walkable, and its own body
    // reaches the raw walk through the projections.
    LHAT_TEST("a written iterate^ walks itself through keys^()");
    check_text(&u,
               "let^ t = {\n"
               "    a = 1,\n"
               "    iterate^ = f^self^{\n"
               "        for^k in^self^.keys^() { yield^k }\n"
               "    }\n"
               "}\n"
               "for^k in^t { }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.1改: a table-literal method's self^ meets the same write rule a
    // def^ method's does -- an f^ may not change the table it was handed.
    LHAT_TEST("an f^ method may not write through self^");
    check_text(&u,
               "let^ u = {\n"
               "    n = 0,\n"
               "    bump = f^self^ { self^.n := 1 }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_CHANGES_TABLE);
    unit_dispose(&u);

    LHAT_TEST("and a p^ method may");
    check_text(&u,
               "let^ u = {\n"
               "    n = 0,\n"
               "    bump = p^self^ { self^.n := 1 }\n"
               "}\n"
               "u.bump()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.11: the construction notation belongs to a written new, and a
    // plain table has none -- its methods write one field at a time.
    LHAT_TEST("but not with the construction notation");
    check_text(&u,
               "let^ u = {\n"
               "    n = 0,\n"
               "    bump = p^self^ { self^{ n = 1 } }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_SELF_TABLE_OUTSIDE_NEW);
    unit_dispose(&u);
}

// 02 の 14.22: the table's own operations, as the checker reads them. The
// machine's side is test_vm_data's.
static void test_builtin_operations(void)
{
    Unit u;

    // 3.4改 with 14.12: sort^'s two arms differ in arity alone, so the one
    // this call fits is settled by the count -- and the comparator literal
    // reads its parameters off the receiver's element type through it.
    LHAT_TEST("a comparator's parameters come from the element type");
    check_text(&u,
               "var^ t = {3, 1, 2}\n"
               "t.sort^(f^ a, b { a <=> b })\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the body is checked against what they were filled with");
    check_text(&u,
               "var^ t = {3, 1, 2}\n"
               "t.sort^(f^ a, b { a .. b })\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    LHAT_TEST("a push against a written element type is measured");
    check_text(&u,
               "var^ t : t^{ ...:number^ } = {1}\n"
               "t.push^(\"s\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 04 の 11.3's line: an empty table's pop is not an error, so the
    // answer carries the nil^ arm and a narrowing is owed.
    LHAT_TEST("pop^ answers the element type beside nil^");
    check_text(&u,
               "var^ t = {1, 2}\n"
               "var^ v : number^ = t.pop^()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 15.1改2: the mutating half write through the receiver alone, so an
    // f^ body may call them on a table it made itself.
    LHAT_TEST("an f^ sorts a table its own body made");
    check_text(&u,
               "let^ f = f^ -> number^ {\n"
               "    var^ t = {2, 1}\n"
               "    t.sort^()\n"
               "    return^ t[1]\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("but not one that arrived as an argument");
    check_text(&u,
               "let^ f = f^ t:t^{ ...:number^ } -> number^ {\n"
               "    t.sort^()\n"
               "    return^ t[1] ?? 0\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MUTATES_OUTSIDE);
    unit_dispose(&u);

    // And the reading half are f^, callable anywhere.
    LHAT_TEST("the reading half are functions");
    check_text(&u,
               "let^ f = f^ t:t^{ ...:number^ } -> string^ {\n"
               "    return^ t.slice^(1, 2).join^(\",\")\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.22 belongs to a plain table alone -- a def^'s names are the
    // writer's, and the hat is what keeps the built-in off the bare word.
    LHAT_TEST("a definition's instance carries none of them");
    check_text(&u,
               "let^ D = def^{ self^{ n = 0 } }\n"
               "var^ d = D.new()\n"
               "d.push^(1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // 15.1改3: clone^ and slice^ answer '-> fresh^…', so what they answer
    // is the body's own -- an f^ clones what arrived and mends the copy.
    LHAT_TEST("an f^ mends its own clone of what arrived");
    check_text(&u,
               "let^ mend = f^ src:t^{ ...:number^ } -> t^{ ...:number^ } {\n"
               "    var^ mine = src.clone^()\n"
               "    mine.sort^()\n"
               "    return^ mine\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and may chain straight off the fresh answer");
    check_text(&u,
               "let^ g = f^ src:t^{ ...:number^ } -> number^ {\n"
               "    return^ src.clone^().pop^() ?? -1\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a slice is the body's own the same way");
    check_text(&u,
               "let^ g = f^ src:t^{ ...:number^ } -> number^ {\n"
               "    var^ cut = src.slice^(1, 2)\n"
               "    cut.push^(9)\n"
               "    return^ cut.count^\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the bare spelling stays the writer's");
    check_text(&u,
               "var^ t = {1}\n"
               "t.push(2)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);
}

int main(void)
{
    test_walking();
    test_positions();
    test_dynamic_key();
    test_nil_safe_compound();
    test_not_indexable();
    test_table_methods();
    test_builtin_operations();
    return lhat_test_report("test_check_tables");
}
