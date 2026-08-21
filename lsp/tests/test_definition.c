// L^ (lhat) -- LSP server tests: where the name at a position was written
// (definition.c). 07 の 4 章.
//
// A name a scope holds and a member found in a type are two different
// questions -- 8 章 answers the first and 14.10 the second -- and going to a
// definition has to answer both. The unit here is standalone, so what is
// pinned is the same-unit half; the half that crosses units is in
// tests/test_program.c, where a program with two of them can be built.

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "lhat/lexer.h"
#include "parser.h"
#include "lhat/source.h"

#include "definition.h"
#include "testutil.h"

typedef struct {
    LhatSource source;
    LhatLexer lexer;
    LhatParseResult parsed;
    LhatCheckResult checked;
    LhatUnit unit;
} Checked;

// A standalone unit -- lhat_check takes a lexer and a tree directly, so no
// program.h graph is needed.
static void check_text(Checked *c, const char *text)
{
    lhat_source_init_from_string(&c->source, "test.lh", text, strlen(text));
    lhat_lexer_init(&c->lexer, &c->source);
    lhat_parse(&c->lexer, &c->parsed);
    lhat_check(c->parsed.root, &c->lexer, true, &c->checked);

    memset(&c->unit, 0, sizeof c->unit);
    c->unit.path = (char *)"test.lh";
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

static uint32_t offset_of(const Checked *c, const char *needle)
{
    const char *found = strstr(c->source.text, needle);
    return found != NULL ? (uint32_t)(found - c->source.text) : 0;
}

// Standing on `from`, the definition should be where `to` begins.
static void expect_definition(Checked *c, const char *from, const char *to)
{
    LspDefinitionSite site;
    memset(&site, 0, sizeof site);
    bool found = lsp_definition_for_unit(&c->unit, offset_of(c, from), &site);
    LHAT_CHECK(found, "expected a definition for \"%s\"", from);
    if (!found) {
        return;
    }
    LHAT_CHECK(site.path == NULL,
               "\"%s\": expected the same unit, got %s", from, site.path);
    LHAT_CHECK_EQ_INT(site.offset, offset_of(c, to));
}

static void test_a_name_a_scope_holds(void)
{
    Checked c;

    LHAT_TEST("8 章: a use goes to the let^ that bound the name");
    check_text(&c,
               "let^ answer = 42\n"
               "let^ doubled = answer * 2\n");
    expect_definition(&c, "answer * 2", "answer = 42");
    check_dispose(&c);

    LHAT_TEST("13.1: and a parameter to where the signature declared it");
    check_text(&c, "let^ twice = f^ n:number^ -> number^ { return^ n * 2 }\n");
    expect_definition(&c, "n * 2", "n:number^");
    check_dispose(&c);
}

// 14.10 looks a member up in a type rather than in a scope, so the place is
// the type's to know (type.h's declared_at) -- and until it did, this was the
// half of going to a definition that could not be answered at all.
static void test_a_member_found_in_a_type(void)
{
    Checked c;

    LHAT_TEST("14.10: a member goes to where the def^ wrote it");
    check_text(&c,
               "let^ Reader = def^{\n"
               "    self^{ at = 1 },\n"
               "    peek = f^self^ -> number^ { return^ self^.at },\n"
               "}\n"
               "let^ r = Reader.new()\n"
               "let^ n = r.peek()\n");
    expect_definition(&c, "peek()", "peek = f^self^");
    // 14.11's template field is a member of the instance, written in the
    // self^{ } section.
    expect_definition(&c, "at },", "at = 1");
    // And the name the def^ was bound to is a name a scope holds, so both
    // kinds answer on one line.
    expect_definition(&c, "Reader.new()", "Reader = def^");
    check_dispose(&c);
}

// 14.9: a name written where a type is names the same thing it names in an
// expression, so it goes to the same place.
static void test_a_written_type_name(void)
{
    Checked c;

    LHAT_TEST("14.9: a type name goes to the def^ it names");
    check_text(&c,
               "let^ Reader = def^{ self^{ at = 1 } }\n"
               "let^ hold = f^ r:Reader -> number^ { return^ r.at }\n");
    expect_definition(&c, "Reader -> number^", "Reader = def^");
    check_dispose(&c);
}

static void test_nothing_to_point_at(void)
{
    Checked c;
    LspDefinitionSite site;

    LHAT_TEST("a position that names nothing answers nothing");
    check_text(&c, "let^ answer = 42\n");
    LHAT_CHECK(!lsp_definition_for_unit(&c.unit, offset_of(&c, "= 42"), &site),
               "expected no definition on the '='");

    // Standing on the declaration itself: the place to go to is already
    // here, and the record a use left points back at this very spot -- so
    // answering would be going the wrong way round.
    LHAT_CHECK(!lsp_definition_for_unit(&c.unit, offset_of(&c, "answer"),
                                        &site),
               "expected no definition on the declaration itself");
    check_dispose(&c);

    // 14.19: a built-in is the language's own, declared nowhere a reader
    // could be sent to.
    LHAT_TEST("and neither does a built-in member");
    check_text(&c, "let^ n = \"text\".length\n");
    LHAT_CHECK(!lsp_definition_for_unit(&c.unit, offset_of(&c, "length"),
                                        &site),
               "expected no definition for a built-in member");
    check_dispose(&c);
}

// 01 の 3.1 says which bytes a name is made of; the record keeps only where
// it starts, so the range is made by walking to the end of one.
static void test_the_name_ends_where_the_name_ends(void)
{
    LHAT_TEST("01 の 3.1: the range covers the name and stops");

    static const char *text = "let^ answer = 42\n";
    size_t length = strlen(text);
    LHAT_CHECK_EQ_INT(lsp_definition_name_end(text, length, 5), 11);
    // A position past the end is one an editor may ask about.
    LHAT_CHECK_EQ_INT(lsp_definition_name_end(text, length, 9999),
                      (int)length);
}

int main(void)
{
    test_a_name_a_scope_holds();
    test_a_member_found_in_a_type();
    test_a_written_type_name();
    test_nothing_to_point_at();
    test_the_name_ends_where_the_name_ends();
    return lhat_test_report("test_definition");
}
