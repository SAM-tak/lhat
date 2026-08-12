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
               "  override^new := f^ n { return^ self^{ upto := n } },\n"
               "  iterate^ := f^self^ {\n"
               "    return^ p^ { yield^ 1 }()\n"
               "  },\n"
               "}\n"
               "for^ v in^ Range.new(4) { var^ n : number^ = v }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 16.3: what in^ walks has to answer with a coroutine, the same demand
    // 15.8 makes of yieldall^.
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

    // 14.10 lets a table carry more than its type lists, so a position it
    // says nothing about is unknown rather than absent.
    LHAT_TEST("a position the type does not mention says nothing");
    check_text(&u,
               "var^ t = { 10 }\n"
               "var^ s : string^ = t[9]\n");
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

int main(void)
{
    test_walking();
    test_positions();
    return lhat_test_report("test_check_tables");
}
