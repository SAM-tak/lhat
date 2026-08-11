// L^ (lhat) -- LSP server tests: what a hover answers with (hover.c).
// 07 の 4 章.

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "lexer.h"
#include "parser.h"
#include "source.h"

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

#ifdef LHAT_WITH_COMMENTS
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

#ifdef LHAT_WITH_COMMENTS
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

int main(void)
{
    test_definition();
#ifdef LHAT_WITH_COMMENTS
    test_comments();
    test_module();
#endif
    test_nothing();
    return lhat_test_report("test_hover");
}
