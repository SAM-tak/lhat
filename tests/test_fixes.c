// L^ (lhat) -- tests for the fixes a diagnostic carries (07 §6).
//
// What is pinned is the bargain a quick fix makes: the edits land on the spot
// the writer would have changed, and applying one answers the diagnostic. A
// fix whose source still reports the same thing is worse than no fix at all,
// so every case here applies its edit and checks the result over again.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lhat/port.h"
#include "program_internal.h"
#include "testutil.h"

// The one file every text here is read as. The loader answers it whatever it
// is asked for, since nothing in these cases requires another unit.
static char *load_one(void *context, const char *path, size_t *length)
{
    (void)path;
    const char *text = (const char *)context;
    size_t size = strlen(text);
    char *copy = (char *)malloc(size + 1);
    if (copy != NULL) {
        memcpy(copy, text, size + 1);
        *length = size;
    }
    return copy;
}

// The unit `text` checks to, and the diagnostic whose ID is `id` in it. Answers
// the index, or the count when nothing of that ID was reported.
static size_t diagnostic_of(const LhatUnit *unit, const char *id)
{
    size_t said = lhat_unit_diagnostic_count(unit);
    for (size_t i = 0; i < said; i++) {
        const char *reported = lhat_unit_diagnostic_id(unit, i);
        if (reported != NULL && strcmp(reported, id) == 0) {
            return i;
        }
    }
    return said;
}

// Whether `text` checks with nothing to report -- what an applied fix has to
// leave behind. 03 の 3.1's strict reading, the one a tool asks for.
static bool is_clean(const char *text)
{
    LhatProgram program;
    lhat_program_init(&program, true, load_one, (void *)text);
    lhat_program_check(&program, "main.lh");
    bool clean = !lhat_program_has_errors(&program);
    lhat_program_dispose(&program);
    return clean;
}

// What an editor does with a fix: the edits applied to the source, in the
// order they come. `out` holds the result.
static void apply(const char *text, const LhatFix *fix, char *out,
                  size_t capacity)
{
    size_t written = 0;
    size_t copied = 0;
    for (size_t i = 0; i < fix->edit_count; i++) {
        const LhatFixEdit *edit = &fix->edits[i];
        while (copied < edit->offset && written + 1 < capacity) {
            out[written++] = text[copied++];
        }
        for (const char *w = edit->text; *w != '\0' && written + 1 < capacity;
             w++) {
            out[written++] = *w;
        }
        copied += edit->length;
    }
    while (text[copied] != '\0' && written + 1 < capacity) {
        out[written++] = text[copied++];
    }
    out[written] = '\0';
}

// One case: `text` reports `id`, its `which`th fix is read, applied, and the
// source it makes is checked again. `title` takes the sentence the fix is
// offered under and `patched` the source; both are the caller's buffers.
//
// Answers how many fixes the diagnostic carries, or SIZE_MAX when the
// diagnostic itself was not reported -- a case whose source stopped saying
// what it was written to say is a broken test, not a missing fix.
#define FIX_NONE ((size_t)-1)

static size_t fix_case(const char *text, const char *id, size_t which,
                       LhatFix *fix, char *title, size_t title_capacity,
                       char *patched, size_t patched_capacity)
{
    title[0] = '\0';
    patched[0] = '\0';
    fix->edit_count = 0;
    fix->confidence = LHAT_FIX_SUGGESTED;

    LhatProgram program;
    lhat_program_init(&program, true, load_one, (void *)text);
    lhat_program_check(&program, "main.lh");

    size_t count = FIX_NONE;
    const LhatUnit *unit = lhat_program_units(&program);
    size_t index = unit != NULL ? diagnostic_of(unit, id) : 0;
    if (unit != NULL && index < lhat_unit_diagnostic_count(unit)) {
        count = lhat_unit_diagnostic_fix_count(unit, index);
        if (which < count && lhat_unit_diagnostic_fix(unit, index, which, fix)) {
            lhat_unit_diagnostic_fix_title(unit, index, which, title,
                                           title_capacity);
            apply(text, fix, patched, patched_capacity);
        }
    }
    lhat_program_dispose(&program);
    return count;
}

// ---------------------------------------------------------------------------
// The parser's one fix (07 §6), through the same storage the checker uses.

static void test_parser(void)
{
    LHAT_TEST("a token that was expected is offered where it was wanted");
    {
        static const char *const text = "var^ f = p^ {\n    var^ x = 1\n";
        LhatFix fix;
        char title[128];
        char patched[256];
        LHAT_CHECK_EQ_INT(fix_case(text, "parse.expected-found", 0, &fix, title,
                                   sizeof title, patched, sizeof patched),
                          1);
        LHAT_CHECK_EQ_INT(fix.edit_count, 1);
        LHAT_CHECK(fix.confidence == LHAT_FIX_SUGGESTED,
                   "the parser noticed it here, which need not be where it "
                   "belongs");
        LHAT_CHECK_EQ_STR(title, strlen(title), "write '}'");
        LHAT_CHECK_EQ_STR(patched, strlen(patched),
                          "var^ f = p^ {\n    var^ x = 1\n}");
        LHAT_CHECK(is_clean(patched), "and the source it makes is clean");
    }
}

// ---------------------------------------------------------------------------
// 8.9: the word a name was bound with, rewritten where it stands.

static void test_words(void)
{
    LHAT_TEST("a write to a let^ offers the var^ the message names");
    {
        static const char *const text = "let^ x = 1\nx := 2\n";
        LhatFix fix;
        char title[128];
        char patched[256];
        LHAT_CHECK_EQ_INT(fix_case(text, "check.assign-to-let", 0, &fix, title,
                                   sizeof title, patched, sizeof patched),
                          1);
        LHAT_CHECK(fix.confidence == LHAT_FIX_SUGGESTED,
                   "not writing to the name is the other way out");
        LHAT_CHECK_EQ_STR(title, strlen(title),
                          "write var^ where the name is bound");
        LHAT_CHECK_EQ_STR(patched, strlen(patched), "var^ x = 1\nx := 2\n");
        LHAT_CHECK(is_clean(patched), "and the source it makes is clean");
    }

    // 12.1 and 16.3改2 bind without a word being written, so there is none to
    // rewrite -- the same reason the message does not offer var^ either.
    LHAT_TEST("a write to a name a construct bound offers nothing");
    {
        static const char *const text =
            "var^ t = { 1, 2 }\nfor^ v in^ t { v := 3 }\n";
        LhatFix fix;
        char title[128];
        char patched[256];
        LHAT_CHECK_EQ_INT(fix_case(text, "check.assign-to-form", 0, &fix, title,
                                   sizeof title, patched, sizeof patched),
                          0);
    }

    // 05 の 4 章: a public^ takes the node's start, so the word is not where
    // the diagnostic is -- which is why a binding remembers where it stands.
    LHAT_TEST("a published var^ offers the let^ it has to be");
    {
        static const char *const text = "public^ var^ x = 1\n";
        LhatFix fix;
        char title[128];
        char patched[256];
        LHAT_CHECK_EQ_INT(fix_case(text, "check.public-is-immutable", 0, &fix,
                                   title, sizeof title, patched,
                                   sizeof patched),
                          1);
        LHAT_CHECK_EQ_STR(title, strlen(title), "bind with let^");
        LHAT_CHECK_EQ_STR(patched, strlen(patched), "public^ let^ x = 1\n");
        LHAT_CHECK(is_clean(patched), "and the source it makes is clean");
    }
}

// ---------------------------------------------------------------------------
// The spellings that are one word short, or one word too many.

static void test_spellings(void)
{
    // 14.10: the one fix that may be applied without being read -- the word
    // is right and only the braces are missing.
    LHAT_TEST("a bare t^ offers its braces, and may be applied unread");
    {
        static const char *const text = "var^ t : t^ = { }\n";
        LhatFix fix;
        char title[128];
        char patched[256];
        LHAT_CHECK_EQ_INT(fix_case(text, "check.bare-table-type", 0, &fix,
                                   title, sizeof title, patched,
                                   sizeof patched),
                          1);
        LHAT_CHECK(fix.confidence == LHAT_FIX_MACHINE,
                   "there is one thing this could have been");
        LHAT_CHECK_EQ_STR(title, strlen(title), "write 't^{}'");
        LHAT_CHECK_EQ_STR(patched, strlen(patched), "var^ t : t^{} = { }\n");
        LHAT_CHECK(is_clean(patched), "and the source it makes is clean");
    }

    // 8.7 with 01 の 8 章: the sigil is glued to the name, and what comes off
    // is the sigil alone.
    LHAT_TEST("a specifier on a let^ offers its own removal");
    {
        static const char *const text = "var^ x = 1\ndo^{ var^ $^x = 2 }\n";
        LhatFix fix;
        char title[128];
        char patched[256];
        LHAT_CHECK_EQ_INT(fix_case(text, "check.scope-on-define", 0, &fix,
                                   title, sizeof title, patched,
                                   sizeof patched),
                          1);
        LHAT_CHECK_EQ_STR(title, strlen(title), "remove the scope specifier");
        LHAT_CHECK_EQ_STR(patched, strlen(patched),
                          "var^ x = 1\ndo^{ var^ x = 2 }\n");
        LHAT_CHECK(is_clean(patched), "and the source it makes is clean");
    }
}

// ---------------------------------------------------------------------------
// 14.12: the marker that says how a member joins the group.

static const char *const BASE =
    "var^ Foo = def^{\n"
    "    self^{ a := 0 },\n"
    "    foo := p^self^, x:string^ { },\n"
    "}\n";

static void test_markers(void)
{
    LHAT_TEST("a member of a name the base holds offers both markers");
    {
        char text[512];
        snprintf(text, sizeof text, "%s%s", BASE,
                 "var^ Bar = Foo .. def^{ self^{}, "
                 "foo := p^self^, x:string^ { } }\n");
        LhatFix fix;
        char title[128];
        char patched[512];
        LHAT_CHECK_EQ_INT(fix_case(text, "check.member-exists", 0, &fix, title,
                                   sizeof title, patched, sizeof patched),
                          2);
        LHAT_CHECK_EQ_STR(title, strlen(title), "write override^");
        LHAT_CHECK(strstr(patched, "override^ foo :=") != NULL,
                   "the word goes in front of the member: %s", patched);
        LHAT_CHECK(is_clean(patched), "and the source it makes is clean");

        // The second is the other reading, and neither is machine-appliable:
        // which of them was meant is the writer's to say.
        LHAT_CHECK_EQ_INT(fix_case(text, "check.member-exists", 1, &fix, title,
                                   sizeof title, patched, sizeof patched),
                          2);
        LHAT_CHECK(fix.confidence == LHAT_FIX_SUGGESTED,
                   "two answers means neither is applied unread");
        LHAT_CHECK_EQ_STR(title, strlen(title), "write overload^");
    }

    LHAT_TEST("a marker over nothing offers to come off");
    {
        char text[512];
        snprintf(text, sizeof text, "%s%s", BASE,
                 "var^ Bar = Foo .. def^{ self^{}, "
                 "overload^ nope := p^self^ { } }\n");
        LhatFix fix;
        char title[128];
        char patched[512];
        LHAT_CHECK_EQ_INT(fix_case(text, "check.nothing-to-override", 0, &fix,
                                   title, sizeof title, patched,
                                   sizeof patched),
                          1);
        LHAT_CHECK_EQ_STR(title, strlen(title), "remove the marker");
        LHAT_CHECK(strstr(patched, "self^{}, nope :=") != NULL,
                   "the word and its space go: %s", patched);
        LHAT_CHECK(is_clean(patched), "and the source it makes is clean");
    }
}

// ---------------------------------------------------------------------------
// 04 の 8.3 and 15.8: a statement that drops what the call answered.

static void test_dropped(void)
{
    LHAT_TEST("a dropped failure offers the try^ that hands it back");
    {
        static const char *const text =
            "errordef^ IOError { NotFound }\n"
            "var^ open = p^ -> number^|IOError { return^ 0 }\n"
            "var^ user = p^ -> nil^|IOError {\n"
            "    open()\n"
            "}\n";
        LhatFix fix;
        char title[128];
        char patched[512];
        LHAT_CHECK_EQ_INT(fix_case(text, "check.error-dropped", 0, &fix, title,
                                   sizeof title, patched, sizeof patched),
                          1);
        LHAT_CHECK_EQ_STR(title, strlen(title),
                          "write try^ to hand the failure back");
        LHAT_CHECK(strstr(patched, "    try^ open()\n") != NULL,
                   "in front of the call: %s", patched);
        LHAT_CHECK(is_clean(patched), "and the source it makes is clean");
    }

    LHAT_TEST("a dropped coroutine offers the await^ that runs it");
    {
        static const char *const text =
            "var^ tick = f^ {\n"
            "    yield^ 1\n"
            "}\n"
            "var^ user = p^ {\n"
            "    tick()\n"
            "}\n";
        LhatFix fix;
        char title[128];
        char patched[512];
        LHAT_CHECK_EQ_INT(fix_case(text, "check.coroutine-dropped", 0, &fix,
                                   title, sizeof title, patched,
                                   sizeof patched),
                          1);
        LHAT_CHECK_EQ_STR(title, strlen(title), "write await^ to delegate");
        LHAT_CHECK(strstr(patched, "    await^ tick()\n") != NULL,
                   "in front of the call: %s", patched);
        LHAT_CHECK(is_clean(patched), "and the source it makes is clean");
    }
}

// ---------------------------------------------------------------------------
// 07 §6: a misspelling, offered the nearest name that is there. The candidates
// are the lists completion offers from the same spot, so a suggestion is
// never a name the checker would refuse for another reason.

// The one near name `text` draws for `id`, applied: answers whether the
// source it makes is `expected` and checks clean.
static void near_case(const char *text, const char *id, const char *near,
                      const char *expected)
{
    LhatFix fix;
    char title[128];
    char patched[512];
    char wanted[160];
    LHAT_CHECK_EQ_INT(fix_case(text, id, 0, &fix, title, sizeof title,
                               patched, sizeof patched),
                      1);
    LHAT_CHECK(fix.confidence == LHAT_FIX_SUGGESTED,
               "a near name is a guess at what was meant");
    snprintf(wanted, sizeof wanted, "change to '%s'", near);
    LHAT_CHECK_EQ_STR(title, strlen(title), wanted);
    LHAT_CHECK_EQ_STR(patched, strlen(patched), expected);
    LHAT_CHECK(is_clean(patched), "and the source it makes is clean");
}

static void test_near(void)
{
    LHAT_TEST("a name no scope holds offers the one it was near");
    near_case("let^ count = 1\nlet^ y = cuont + 1\n", "check.undefined",
              "count", "let^ count = 1\nlet^ y = count + 1\n");

    // A third of what was written, and at least one: four letters apart
    // from everything in reach is not a misspelling of any of it.
    LHAT_TEST("and nothing when nothing is near");
    {
        LhatFix fix;
        char title[128];
        char patched[256];
        LHAT_CHECK_EQ_INT(fix_case("let^ count = 1\nlet^ y = zzzq + 1\n",
                                   "check.undefined", 0, &fix, title,
                                   sizeof title, patched, sizeof patched),
                          0);
    }

    // 14.19: the built-ins are no type's list, so they are asked of the
    // checker one spelling at a time -- the same way completion asks.
    LHAT_TEST("a member no receiver holds offers a built-in one");
    near_case("let^ s = \"abc\"\nlet^ n : number^ = s.lenght\n",
              "check.no-member.named", "length",
              "let^ s = \"abc\"\nlet^ n : number^ = s.length\n");

    LHAT_TEST("and a written one, through the definition's chain");
    {
        char text[512];
        char expected[512];
        snprintf(text, sizeof text, "%s%s", BASE,
                 "var^ o = Foo.new()\no.fo(\"x\")\n");
        snprintf(expected, sizeof expected, "%s%s", BASE,
                 "var^ o = Foo.new()\no.foo(\"x\")\n");
        near_case(text, "check.no-member.named", "foo", expected);
    }

    // 14.11: what a construction writes is a field, so a method of the
    // definition is not offered for one.
    LHAT_TEST("a field the template does not hold offers one it does");
    near_case("var^ C = def^{\n"
              "    self^{ value := 1 },\n"
              "    override^new := f^ n:number^ { self^{ valeu := n } },\n"
              "}\n"
              "var^ c = C.new(5)\n",
              "check.no-member.named", "value",
              "var^ C = def^{\n"
              "    self^{ value := 1 },\n"
              "    override^new := f^ n:number^ { self^{ value := n } },\n"
              "}\n"
              "var^ c = C.new(5)\n");

    LHAT_TEST("a field an error kind does not declare offers one it does");
    near_case("errordef^ IOError { NotFound { path : string^ } }\n"
              "var^ e = error^ IOError.NotFound { paht = \"x\" }\n",
              "check.no-member.named", "path",
              "errordef^ IOError { NotFound { path : string^ } }\n"
              "var^ e = error^ IOError.NotFound { path = \"x\" }\n");
}

// ---------------------------------------------------------------------------
// The codes that know no fix, and the readings past the end.

static void test_nothing(void)
{
    LHAT_TEST("a diagnostic with no fix answers none, and so does past the end");
    {
        LhatProgram program;
        static const char *const text = "var^ v : number^ = \"text\"\n";
        lhat_program_init(&program, true, load_one, (void *)text);
        lhat_program_check(&program, "main.lh");

        const LhatUnit *unit = lhat_program_units(&program);
        LHAT_REQUIRE(unit != NULL, "the unit is there");
        LHAT_CHECK_EQ_INT(lhat_unit_diagnostic_count(unit), 1);
        LHAT_CHECK_EQ_INT(lhat_unit_diagnostic_fix_count(unit, 0), 0);
        LHAT_CHECK_EQ_INT(lhat_unit_diagnostic_fix_count(unit, 7), 0);

        LhatFix fix;
        LHAT_CHECK_EQ_BOOL(lhat_unit_diagnostic_fix(unit, 0, 0, &fix), false);
        LHAT_CHECK_EQ_BOOL(lhat_unit_diagnostic_fix(unit, 7, 0, &fix), false);

        // A title of nothing is the empty sentence, measured the usual way.
        char title[32];
        LHAT_CHECK_EQ_INT(
            lhat_unit_diagnostic_fix_title(unit, 0, 0, NULL, 0), 0);
        LHAT_CHECK_EQ_INT(
            lhat_unit_diagnostic_fix_title(unit, 0, 0, title, sizeof title), 0);
        LHAT_CHECK_EQ_STR(title, strlen(title), "");
        lhat_program_dispose(&program);
    }
}

int main(void)
{
    test_parser();
    test_words();
    test_spellings();
    test_markers();
    test_dropped();
    test_near();
    test_nothing();
    return lhat_test_report("test_fixes");
}
