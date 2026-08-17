// L^ (lhat) -- LSP server tests: the type at a position, as text to keep
// (signature.c). 07 の 4 章.

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "lhat/lexer.h"
#include "parser.h"
#include "lhat/source.h"

#include "signature.h"
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

static uint32_t first_offset(const Checked *c, const char *needle)
{
    const char *found = strstr(c->source.text, needle);
    return found != NULL ? (uint32_t)(found - c->source.text) : 0;
}

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

static void test_at_a_use(void)
{
    Checked c;

    LHAT_TEST("the type at a use comes back as text");
    check_text(&c, "let^ n = 1\nlet^ m = n + 1\n");
    char *signature = lsp_signature_for_unit(&c.unit, last_offset(&c, "n"));
    LHAT_CHECK(signature != NULL && strcmp(signature, "number^") == 0,
               "expected number^, got %s", signature ? signature : "(nothing)");
    free(signature);
    check_dispose(&c);
}

// Someone wanting to copy a signature stands on the declaration as often as
// on a use, and a declaration has no resolution of its own -- it binds a
// name rather than resolving one. What answers is a use pointing back at it.
static void test_at_a_declaration(void)
{
    Checked c;

    LHAT_TEST("and the type at a declaration, read back through a use");
    check_text(&c,
               "let^ twice = f^ n:number^ -> number^ { return^ n * 2 }\n"
               "let^ four = twice(2)\n");
    char *signature =
        lsp_signature_for_unit(&c.unit, first_offset(&c, "twice"));
    LHAT_CHECK(signature != NULL &&
                   strcmp(signature, "f^number^ -> number^;") == 0,
               "expected the signature, got %s",
               signature ? signature : "(nothing)");
    free(signature);
    check_dispose(&c);

    LHAT_TEST("from anywhere within the name, not only its first byte");
    check_text(&c,
               "let^ twice = f^ n:number^ -> number^ { return^ n * 2 }\n"
               "let^ four = twice(2)\n");
    signature =
        lsp_signature_for_unit(&c.unit, first_offset(&c, "twice") + 3);
    LHAT_CHECK(signature != NULL, "expected an answer inside the name");
    free(signature);
    check_dispose(&c);
}

// What a hover would elide, this writes out -- that is the whole reason it
// is a separate answer.
static void test_nothing_is_elided(void)
{
    Checked c;

    LHAT_TEST("a wide type comes back whole");
    check_text(&c,
               "let^ wide = { a = 1, b = 2, c = 3, d = 4, e = 5, f = 6,\n"
               "              g = 7, h = 8 }\n"
               "let^ n = wide.a\n");
    char *signature =
        lsp_signature_for_unit(&c.unit, last_offset(&c, "wide"));
    LHAT_CHECK(signature != NULL, "expected a signature");
    if (signature != NULL) {
        LHAT_CHECK(strstr(signature, "…") == NULL,
                   "expected nothing elided, got %s", signature);
        LHAT_CHECK(strstr(signature, "h : number^") != NULL,
                   "expected the last member, got %s", signature);
    }
    free(signature);
    check_dispose(&c);
}

static void test_nothing_there(void)
{
    Checked c;

    LHAT_TEST("a position that names nothing answers nothing");
    check_text(&c, "let^ n = 1\n");
    LHAT_CHECK(lsp_signature_for_unit(&c.unit, first_offset(&c, "= 1")) == NULL,
               "expected no signature on the '='");
    LHAT_CHECK(lsp_signature_for_unit(&c.unit, 100000) == NULL,
               "expected no signature past the end");
    check_dispose(&c);
}

int main(void)
{
    test_at_a_use();
    test_at_a_declaration();
    test_nothing_is_elided();
    test_nothing_there();
    return lhat_test_report("test_signature");
}
