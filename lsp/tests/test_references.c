// L^ (lhat) -- LSP server tests: every place one name was written
// (references.c). 07 の 5 章.
//
// What is pinned is the identity a reference has: a name is the place it was
// declared, not the way it is spelt. Two `count`s in two scopes are two
// names and must not answer for each other, and one name reached from two
// files is one name. Nothing here reads 8 章's scoping -- the checker's own
// record is what says which is which, and these tests are what keep that
// reading honest.

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "lhat/lexer.h"
#include "parser.h"
#include "lhat/source.h"

#include "references.h"
#include "testutil.h"

typedef struct {
    LhatSource source;
    LhatLexer lexer;
    LhatParseResult parsed;
    LhatCheckResult checked;
    LhatUnit unit;
} Checked;

// A standalone unit, the way test_completion.c makes one. Cross-unit
// answers need a program and are measured against the running server
// instead; what is asked here is the rule, which is the same either way.
static void check_text(Checked *c, const char *path, const char *text)
{
    lhat_source_init_from_string(&c->source, path, text, strlen(text));
    lhat_lexer_init(&c->lexer, &c->source);
    lhat_parse(&c->lexer, &c->parsed);
    lhat_check(c->parsed.root, &c->lexer, true, &c->checked);

    memset(&c->unit, 0, sizeof c->unit);
    c->unit.path = (char *)path;
    c->unit.loaded = true;
    c->unit.source = c->source;
    c->unit.lexer = c->lexer;
    c->unit.parsed = c->parsed;
    c->unit.checked = c->checked;
}

static void check_dispose(Checked *c)
{
    lhat_check_result_dispose(&c->checked);
    lhat_parse_result_dispose(&c->parsed);
    lhat_lexer_dispose(&c->lexer);
    lhat_source_dispose(&c->source);
}

// Every place found, so a test can say how many and where.
typedef struct {
    uint32_t from[32];
    uint32_t to[32];
    size_t count;
} Places;

static void note(void *context, const LhatUnit *unit, uint32_t from,
                 uint32_t to)
{
    (void)unit;
    Places *places = (Places *)context;
    if (places->count < 32) {
        places->from[places->count] = from;
        places->to[places->count] = to;
        places->count++;
    }
}

// The offset just past the last `needle` -- where a cursor stands having
// typed it. The LAST, so a needle naming a use is not answered by the
// declaration that shares its spelling.
static uint32_t past_last(const Checked *c, const char *needle)
{
    const char *found = NULL;
    for (const char *at = strstr(c->source.text, needle); at != NULL;
         at = strstr(at + 1, needle)) {
        found = at;
    }
    LHAT_CHECK(found != NULL, "expected \"%s\" to be in the source", needle);
    return found != NULL
               ? (uint32_t)(found - c->source.text) + (uint32_t)strlen(needle)
               : 0;
}

// Every use of whatever the cursor past `needle` is about, plus the
// declaration when it stands here too.
static Places asked_at(Checked *c, const char *needle, bool *found)
{
    Places places;
    memset(&places, 0, sizeof places);
    LspReferenceTarget target;
    *found = lsp_references_target(&c->unit, past_last(c, needle), &target);
    if (!*found) {
        return places;
    }
    uint32_t from = 0;
    uint32_t to = 0;
    if (lsp_references_declaration_in(&c->unit, &target, &from, &to)) {
        note(&places, &c->unit, from, to);
    }
    lsp_references_in_unit(&c->unit, &target, note, &places);
    return places;
}

static void test_a_name_is_where_it_was_declared(void)
{
    Checked c;
    bool found = false;

    LHAT_TEST("07 の 5 章: a declaration and every use of it");
    check_text(&c, "a.lh",
               "let^ counter = 1\n"
               "let^ a = counter + 1\n"
               "let^ b = counter * 2\n");
    {
        Places places = asked_at(&c, "let^ counter", &found);
        LHAT_CHECK(found, "expected the declaration to be about something");
        LHAT_CHECK_EQ_INT((int)places.count, 3);
    }

    LHAT_TEST("and standing on a use finds the same three");
    {
        Places places = asked_at(&c, "let^ b = counter", &found);
        LHAT_CHECK(found, "expected the use to be about something");
        LHAT_CHECK_EQ_INT((int)places.count, 3);
    }

    // A cursor just past a name is still on it: that is where it stands when
    // the name has only now been typed.
    LHAT_TEST("and a cursor one byte past the name is still on it");
    {
        Places places = asked_at(&c, "counter", &found);
        LHAT_CHECK(found, "expected a cursor past the name to answer");
        LHAT_CHECK_EQ_INT((int)places.count, 3);
    }
    check_dispose(&c);
}

static void test_two_names_of_one_spelling(void)
{
    Checked c;
    bool found = false;

    // 8 章: the inner one is a name of its own. A search that matched on the
    // spelling would rename both, which is the mistake this whole design is
    // shaped to make impossible.
    LHAT_TEST("8 章: two scopes' names of one spelling are two names");
    check_text(&c, "a.lh",
               "let^ v = 1\n"
               "let^ f = p^ {\n"
               "    let^ v = 2\n"
               "    let^ inner = v\n"
               "}\n"
               "let^ outer = v\n");
    {
        // The inner one: its declaration and the use below it.
        Places places = asked_at(&c, "    let^ v", &found);
        LHAT_CHECK(found, "expected the inner declaration to answer");
        LHAT_CHECK_EQ_INT((int)places.count, 2);
    }
    {
        // The outer one: its declaration and the use at the end.
        Places places = asked_at(&c, "outer = v", &found);
        LHAT_CHECK(found, "expected the outer use to answer");
        LHAT_CHECK_EQ_INT((int)places.count, 2);
    }
    check_dispose(&c);
}

static void test_what_has_nowhere_to_edit(void)
{
    Checked c;
    bool found = false;

    // 01 の 2.3: a hatted word is the language's own. None was declared
    // anywhere a rename could reach, and answering a range for one would put
    // an editor's box over something bound to fail.
    LHAT_TEST("01 の 2.3: a hatted word is not a name that was declared");
    check_text(&c, "a.lh", "let^ twice = f^ n:number^ -> number^ { return^ n }\n");
    {
        asked_at(&c, "= f", &found);
        LHAT_CHECK(!found, "expected f^ to answer nothing");
        asked_at(&c, "let", &found);
        LHAT_CHECK(!found, "expected let^ to answer nothing");
        asked_at(&c, "n:number", &found);
        LHAT_CHECK(!found, "expected number^ to answer nothing");
    }
    // But the parameter it declares is a name like any other: its own place
    // and the use in the body.
    LHAT_TEST("13.1: but the parameter beside it is");
    {
        Places places = asked_at(&c, "return^ n", &found);
        LHAT_CHECK(found, "expected the parameter to answer");
        LHAT_CHECK_EQ_INT((int)places.count, 2);
    }
    check_dispose(&c);

    LHAT_TEST("and neither is what stands where no name does");
    check_text(&c, "a.lh", "let^ a = 1 + 2\n");
    {
        asked_at(&c, "1 +", &found);
        LHAT_CHECK(!found, "expected an operator to answer nothing");
    }
    check_dispose(&c);
}

static void test_a_member_is_a_name_too(void)
{
    Checked c;
    bool found = false;

    // 14.10: a member is looked up in a type rather than in a scope, and a
    // def^'s carries its written place all the same -- so the same reading
    // answers for it.
    LHAT_TEST("14.10: a member of a def^ is a declaration with uses");
    check_text(&c, "a.lh",
               "let^ Reader = def^{\n"
               "    self^{ at = 1 },\n"
               "    peek = f^self^ -> number^ { return^ self^.at },\n"
               "}\n"
               "let^ r = Reader.new()\n"
               "let^ a = r.at\n"
               "let^ b = r.at\n");
    {
        Places places = asked_at(&c, "let^ b = r.at", &found);
        LHAT_CHECK(found, "expected the member to answer");
        // Where it was written, the read inside peek, and the two after.
        LHAT_CHECK_EQ_INT((int)places.count, 4);
    }
    check_dispose(&c);

    // 05 の 8.7 and 14.19 declare nothing this source could edit, and a
    // plain table literal's member is the third of the same kind: the
    // checker records no place for it, and go-to-definition answers
    // nothing there either. What must not happen is this taking the use
    // for a declaration of its own -- a rename would then edit that one
    // place and leave every other one behind.
    LHAT_TEST("but what was declared nowhere here answers nothing");
    check_text(&c, "a.lh",
               "let^ point = { x = 1 }\n"
               "let^ a = point.x\n");
    {
        asked_at(&c, "let^ a = point.x", &found);
        LHAT_CHECK(!found, "expected a literal's member to answer nothing");
    }
    check_dispose(&c);
}

static void test_a_new_name_has_to_be_one(void)
{
    LHAT_TEST("01 の 3.1: a rename refuses what the lexer would not read back");
    LHAT_CHECK(lsp_references_is_name("total", 5), "a plain name");
    LHAT_CHECK(lsp_references_is_name("_two", 4), "an underscore leads");
    LHAT_CHECK(lsp_references_is_name("a1", 2), "a digit follows");
    LHAT_CHECK(!lsp_references_is_name("2bad", 4), "a digit leads");
    LHAT_CHECK(!lsp_references_is_name("has space", 9), "a space is not one");
    LHAT_CHECK(!lsp_references_is_name("hat^", 4), "01 の 2.3: nor is a hat");
    LHAT_CHECK(!lsp_references_is_name("", 0), "nor is nothing");
    LHAT_CHECK(!lsp_references_is_name(NULL, 4), "nor is nothing at all");
}

int main(void)
{
    test_a_name_is_where_it_was_declared();
    test_two_names_of_one_spelling();
    test_what_has_nowhere_to_edit();
    test_a_member_is_a_name_too();
    test_a_new_name_has_to_be_one();
    return lhat_test_report("test_references");
}
