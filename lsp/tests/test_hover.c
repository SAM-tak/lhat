// L^ (lhat) -- LSP server tests: what a hover answers with (hover.c).
// 07 の 4 章.

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "lexer.h"
#include "parser.h"
#include "lhat/source.h"

#include "hover.h"
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

/** The offset of the last occurrence of `needle`, which is the use site. */
static uint32_t last_offset(const Checked *c, const char *needle)
{
    const char *found = NULL;
    const char *at = c->source.text;
    while ((at = strstr(at, needle)) != NULL) {
        found = at;
        at++;
    }
    return found != NULL ? (uint32_t)(found - c->source.text) : 0;
}

static char *hover_text(const Checked *c, uint32_t offset)
{
    cJSON *hover = lsp_hover_for_unit(&c->unit, offset);
    if (hover == NULL) {
        return NULL;
    }
    cJSON *contents = cJSON_GetObjectItemCaseSensitive(hover, "contents");
    cJSON *value = cJSON_GetObjectItemCaseSensitive(contents, "value");
    char *copy = value != NULL && cJSON_IsString(value)
        ? _strdup(value->valuestring) : NULL;
    cJSON_Delete(hover);
    return copy;
}

static void expect_contains(const char *text, const char *needle)
{
    LHAT_CHECK(text != NULL && strstr(text, needle) != NULL,
               "expected a hover containing \"%s\", got %s",
               needle, text != NULL ? text : "(nothing)");
}

static void test_definition(void)
{
    Checked c;

    LHAT_TEST("a hover shows the definition a name reaches");
    check_text(&c, "let^ answer = 42\nprint(answer)\n");
    char *text = hover_text(&c, last_offset(&c, "answer"));
    expect_contains(text, "let^ answer = 42");
    free(text);
    check_dispose(&c);

    // The body of a definition may run for pages; the first line is what
    // says what it is.
    LHAT_TEST("only the first line of a definition is shown");
    check_text(&c,
               "let^ twice = f^n:number^ -> number^ {\n"
               "    return^ n * 2\n"
               "}\n"
               "print(twice(1))\n");
    text = hover_text(&c, last_offset(&c, "twice"));
    expect_contains(text, "let^ twice = f^n:number^ -> number^ {");
    LHAT_CHECK(text != NULL && strstr(text, "return^") == NULL,
               "the body leaked into the hover: %s", text ? text : "");
    free(text);
    check_dispose(&c);

    // The annotation as written is what shows, which is where a type comes
    // from until the doc-comment half of hover exists (07 の 4).
    LHAT_TEST("an annotation shows as it was written");
    check_text(&c, "let^ n:number^ = 1\nprint(n)\n");
    text = hover_text(&c, last_offset(&c, "n"));
    expect_contains(text, "number^");
    free(text);
    check_dispose(&c);

    // 07 の what the checker settled on, under the line as written. A
    // definition with no annotation has only this.
    LHAT_TEST("the inferred type is shown");
    check_text(&c, "let^ n = 1\nprint(n)\n");
    text = hover_text(&c, last_offset(&c, "n"));
    expect_contains(text, "number^");
    free(text);
    check_dispose(&c);

    check_text(&c,
               "let^ twice = f^n:number^ -> number^ {\n"
               "    return^ n * 2\n"
               "}\n"
               "print(twice(1))\n");
    text = hover_text(&c, last_offset(&c, "twice"));
    expect_contains(text, "f^number^ -> number^;");
    free(text);
    check_dispose(&c);

    LHAT_TEST("a table type is written with its members");
    check_text(&c, "let^ it = { sku := \"a\", count := 2 }\nprint(it)\n");
    text = hover_text(&c, last_offset(&c, "it"));
    expect_contains(text, "t^{");
    expect_contains(text, "sku : string^");
    expect_contains(text, "count : number^");
    free(text);
    check_dispose(&c);

    // 07 の a name from another unit is bound by a require^ or import^,
    // and that line names the module -- so showing the definition shows where
    // the name came from, without anything extra being tracked.
    LHAT_TEST("a name from elsewhere shows the module it came from");
    check_text(&c, "let^ other = require^ \"lib/util.lh\"\nprint(other)\n");
    text = hover_text(&c, last_offset(&c, "other"));
    expect_contains(text, "require^ \"lib/util.lh\"");
    free(text);
    check_dispose(&c);

    LHAT_TEST("a parameter resolves to where it was declared");
    check_text(&c,
               "let^ f = f^step:number^ -> number^ {\n"
               "    return^ step + 1\n"
               "}\n");
    text = hover_text(&c, last_offset(&c, "step"));
    expect_contains(text, "step");
    free(text);
    check_dispose(&c);
}

#if LHAT_WITH_COMMENTS
static void test_comments(void)
{
    Checked c;

    // 01 の 6.4: the comment block above a definition is what it says about
    // itself. The markers come off so it reads as prose.
    LHAT_TEST("a hover carries the comment written above the definition");
    check_text(&c,
               "# how many times to try\n"
               "let^ retries = 3\n"
               "print(retries)\n");
    char *text = hover_text(&c, last_offset(&c, "retries"));
    expect_contains(text, "let^ retries = 3");
    expect_contains(text, "how many times to try");
    LHAT_CHECK(text != NULL && strstr(text, "# how") == NULL,
               "the comment marker was left in: %s", text ? text : "");
    free(text);
    check_dispose(&c);

    LHAT_TEST("a block comment reads the same way");
    check_text(&c,
               "#[ the limit ]#\n"
               "let^ cap = 9\n"
               "print(cap)\n");
    text = hover_text(&c, last_offset(&c, "cap"));
    expect_contains(text, "the limit");
    free(text);
    check_dispose(&c);
}
#endif

#if LHAT_WITH_COMMENTS
static void test_module(void)
{
    Checked c;

    // 07 の module^ says what the unit is, so what is written against it
    // describes the unit. It declares rather than uses a name, so it is found
    // by position instead of through a resolution.
    LHAT_TEST("module^ carries the unit's own description");
    check_text(&c,
               "# 在庫を扱う\n"
               "module^ shop.inventory\n"
               "let^ x = 1\n");
    char *text = hover_text(&c, last_offset(&c, "shop.inventory"));
    expect_contains(text, "module^ shop.inventory");
    expect_contains(text, "在庫を扱う");
    free(text);
    check_dispose(&c);
}
#endif

static void test_nothing(void)
{
    Checked c;

    LHAT_TEST("nothing is answered where no name stands");
    check_text(&c, "let^ x = 1\n");
    // Inside the literal, which resolves to no binding.
    LHAT_CHECK(lsp_hover_for_unit(&c.unit, last_offset(&c, "1")) == NULL,
               "expected no hover on a literal");
    LHAT_CHECK(lsp_hover_for_unit(&c.unit, 100000) == NULL,
               "expected no hover past the end");
    check_dispose(&c);

    LHAT_TEST("a hover marks the range of the name it is about");
    check_text(&c, "let^ value = 1\nprint(value)\n");
    cJSON *hover = lsp_hover_for_unit(&c.unit, last_offset(&c, "value"));
    LHAT_CHECK(hover != NULL, "expected a hover");
    if (hover != NULL) {
        cJSON *range = cJSON_GetObjectItemCaseSensitive(hover, "range");
        cJSON *start = cJSON_GetObjectItemCaseSensitive(range, "start");
        cJSON *end = cJSON_GetObjectItemCaseSensitive(range, "end");
        // "print(value)" is line 1, and the name starts at character 6.
        LHAT_CHECK_EQ_INT(
            cJSON_GetObjectItemCaseSensitive(start, "line")->valueint, 1);
        LHAT_CHECK_EQ_INT(
            cJSON_GetObjectItemCaseSensitive(start, "character")->valueint, 6);
        LHAT_CHECK_EQ_INT(
            cJSON_GetObjectItemCaseSensitive(end, "character")->valueint, 11);
        cJSON_Delete(hover);
    }
    check_dispose(&c);
}

/** The offset of the first occurrence, which is where a name is declared. */
static uint32_t first_offset(const Checked *c, const char *needle)
{
    const char *found = strstr(c->source.text, needle);
    return found != NULL ? (uint32_t)(found - c->source.text) : 0;
}

// 14.10: a member is looked up in a type rather than in a scope, so there is
// no binding to point at -- and for a type from another unit or from a host
// registration there is nothing in this source to point at either. What it
// is is still known, and that is what a reader wanted.
static void test_member(void)
{
    Checked c;

    LHAT_TEST("a member answers with its type, having no line to show");
    check_text(&c,
               "let^ point = { x = 1 }\n"
               "print(point.x)\n");
    char *text = hover_text(&c, last_offset(&c, "x)"));
    expect_contains(text, "number^");
    free(text);
    check_dispose(&c);

    LHAT_TEST("and a member holding a subroutine shows its signature");
    check_text(&c,
               "let^ shape = { area = f^ -> number^ { return^ 1 } }\n"
               "print(shape.area())\n");
    text = hover_text(&c, last_offset(&c, "area"));
    expect_contains(text, "f^");
    free(text);
    check_dispose(&c);
}

// A declaration binds a name rather than resolving one, so the checker
// records nothing against it and the answer is found by position instead.
// 14.15 with 14.11: a definition still holding a member nothing has provided
// is one to compose onto rather than one to make anything of, and 14.11
// refuses its new. Two definitions differing only in that read the same in
// the source until the self^{ } section is gone through line by line, so the
// hover says which this is.
static void test_an_abstract_definition_says_so(void)
{
    Checked c;

    LHAT_TEST("14.15: a definition with a hole in it says so");
    check_text(&c,
               "let^ Base = def^{\n"
               "    self^{ abstract^ slot : number^ },\n"
               "}\n"
               "let^ Filled = Base..def^{\n"
               "    self^{ slot = 1 },\n"
               "}\n"
               "let^ made = Filled.new()\n");
    // The name of the hole, which is the one 14.11's refusal names too.
    char *text = hover_text(&c, last_offset(&c, "Base.."));
    expect_contains(text, "*(abstract: slot)*");
    free(text);

    LHAT_TEST("and one with none says nothing");
    text = hover_text(&c, last_offset(&c, "Filled.new()"));
    LHAT_CHECK(text != NULL && strstr(text, "abstract") == NULL,
               "expected no note on a filled definition, got %s",
               text != NULL ? text : "(nothing)");
    free(text);
    check_dispose(&c);

    // 14.15改3: a written new gives a template field its value too, so a
    // definition built that way has no hole to say anything about. The note
    // is the same predicate 14.11's refusal asks, so the two agree here.
    LHAT_TEST("and a field the written new writes is not a hole");
    check_text(&c,
               "let^ Held = def^{\n"
               "    self^{ abstract^ slot : number^ },\n"
               "    override^new = f^v:number^ { self^{ slot = v } },\n"
               "}\n"
               "let^ made = Held.new(1)\n");
    text = hover_text(&c, last_offset(&c, "Held.new(1)"));
    LHAT_CHECK(text != NULL && strstr(text, "abstract") == NULL,
               "expected no note where the new writes the field, got %s",
               text != NULL ? text : "(nothing)");
    free(text);
    check_dispose(&c);
}

// A declaration binds a name rather than resolving one, so nothing is
// recorded against it -- and that is where a reader stands to ask what a name
// is. The answer is under another key (lsp/resolution.h), which is the same
// lookup Copy Signature makes: the two say the same thing about a position or
// one of them is wrong.
static void test_a_declaration_shows_its_type(void)
{
    Checked c;

    LHAT_TEST("07 の 4 章: a declaration answers with the type as well");
    check_text(&c, "let^ answer = 42\nprint(answer)\n");
    char *text = hover_text(&c, first_offset(&c, "answer"));
    expect_contains(text, "let^ answer = 42");
    expect_contains(text, ": number^");
    free(text);
    check_dispose(&c);
}

static void test_declaration(void)
{
    Checked c;

    LHAT_TEST("a hover on the name a let^ declares shows that let^");
    check_text(&c, "let^ answer = 42\nprint(answer)\n");
    char *text = hover_text(&c, first_offset(&c, "answer"));
    expect_contains(text, "let^ answer = 42");
    free(text);
    check_dispose(&c);

    LHAT_TEST("and one on a parameter shows what declared it");
    check_text(&c,
               "let^ twice = f^n:number^ -> number^ {\n"
               "    return^ n * 2\n"
               "}\n");
    text = hover_text(&c, first_offset(&c, "n:number^"));
    expect_contains(text, "n:number^");
    free(text);
    check_dispose(&c);

    // The span of a definition runs to the end of what it binds. Answering
    // anywhere inside it would put a hover over every space in a body pages
    // long, so only the name itself is asked about.
    LHAT_TEST("a position that is not on a name answers nothing");
    check_text(&c, "let^ wide = f^ -> number^ {\n    return^ 1\n}\n");
    LHAT_CHECK(lsp_hover_for_unit(&c.unit, first_offset(&c, "   return^")) ==
                   NULL,
               "expected no hover on the blank before a statement");
    check_dispose(&c);
}

// A name whose meaning comes from neither a scope nor a type: 15.10's this^
// is the subroutine running, given by the body around it. Nothing in the
// source declares it, so the type is the whole answer -- the same shape a
// host-bound name like print takes (05 の 8.2), which needs a registration
// behind it and is pinned against a running server instead.
static void test_named_by_the_form(void)
{
    Checked c;

    LHAT_TEST("15.10: this^ answers with the signature it stands for");
    check_text(&c,
               "let^ fact = f^n:number^ -> number^ {\n"
               "    if^ n < 2 { return^ 1 }\n"
               "    return^ n * this^(n - 1)\n"
               "}\n");
    char *text = hover_text(&c, last_offset(&c, "this^"));
    expect_contains(text, "f^number^ -> number^;");
    free(text);
    check_dispose(&c);

    // 07 の 4 章: the three literals are left unrecorded on purpose -- the
    // spelling is already the answer, so a hover would repeat the word.
    LHAT_TEST("and a literal keyword answers nothing, being its own answer");
    check_text(&c, "let^ yes = true^\n");
    LHAT_CHECK(lsp_hover_for_unit(&c.unit, last_offset(&c, "true^")) == NULL,
               "expected no hover on true^");
    check_dispose(&c);
}

int main(void)
{
    test_definition();
    test_member();
    test_declaration();
    test_a_declaration_shows_its_type();
    test_an_abstract_definition_says_so();
    test_named_by_the_form();
#if LHAT_WITH_COMMENTS
    test_comments();
    test_module();
#endif
    test_nothing();
    return lhat_test_report("test_hover");
}
