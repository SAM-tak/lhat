// L^ (lhat) -- LSP server tests: a .lton file, checked as what it is.
//
// DesignDocuments/08-lton.md. Two things are pinned here, and they are the
// two an editor can see: that the text is wrapped the way stdlib/lton.c
// wraps it, and that what comes back is said in the file's own coordinates
// rather than the wrapped text's.

#include <stdlib.h>
#include <string.h>

#include "lhat/lexer.h"
#include "lhat/source.h"
#include "parser.h"
#include "check.h"

#include "lton.h"
#include "position.h"
#include "testutil.h"

// The wrapper is the sample library's, so this is the one place the two are
// compared -- if lton.c ever grew a spelling of its own, the format would
// have quietly become two formats.
#include "stdlib/lton.h"

static void test_which_paths_are_lton(void)
{
    LHAT_TEST("08 の 2: a .lton is an LTON file and nothing else is");
    LHAT_CHECK(lsp_lton_is_path("conf.lton"), "conf.lton");
    LHAT_CHECK(lsp_lton_is_path("c:/a/b/conf.lton"), "an absolute path");
    LHAT_CHECK(!lsp_lton_is_path("main.lh"), "a unit is not one");
    LHAT_CHECK(!lsp_lton_is_path("lton"), "the extension is not a name");
    LHAT_CHECK(!lsp_lton_is_path(".lton"), "and neither is a bare extension");
    LHAT_CHECK(!lsp_lton_is_path(NULL), "nothing is not one either");
}

static void test_the_wrapping_is_the_librarys(void)
{
    LHAT_TEST("08 の 7: the text goes inside the wrapper stdlib/lton.c uses");
    static const char *const body = "a = 1,\nb = 2,\n";
    size_t whole = 0;
    char *wrapped = lsp_lton_wrap(body, strlen(body), &whole);
    LHAT_CHECK(wrapped != NULL, "it wrapped");
    if (wrapped == NULL) {
        return;
    }
    LHAT_CHECK_EQ_INT(strlen(wrapped), whole);

    size_t prologue = strlen(LHATSTDLIB_LTON_PROLOGUE);
    LHAT_CHECK(strncmp(wrapped, LHATSTDLIB_LTON_PROLOGUE, prologue) == 0,
               "the prologue is the library's, got %.40s", wrapped);
    LHAT_CHECK(strcmp(wrapped + whole - strlen(LHATSTDLIB_LTON_EPILOGUE),
                      LHATSTDLIB_LTON_EPILOGUE) == 0,
               "and so is the epilogue");
    // The body verbatim in between: an LTON file is its own bytes and the
    // wrapper is around them, never through them.
    LHAT_CHECK(strncmp(wrapped + prologue, body, strlen(body)) == 0,
               "the body is laid down as it was written");

    // 08 の 7's own promise about diagnostics: the prologue takes no line of
    // its own, so a line in the file is a line in what the checker reads.
    LHAT_CHECK(strchr(LHATSTDLIB_LTON_PROLOGUE, '\n') == NULL,
               "the prologue stays on one line");
    LHAT_CHECK_EQ_INT(lsp_lton_prologue_length(), (uint32_t)prologue);

    free(wrapped);

    LHAT_TEST("and an empty file is an empty table");
    whole = 0;
    wrapped = lsp_lton_wrap("", 0, &whole);
    LHAT_CHECK(wrapped != NULL && whole == strlen(LHATSTDLIB_LTON_PROLOGUE) +
                                               strlen(LHATSTDLIB_LTON_EPILOGUE),
               "nothing between the two halves");
    free(wrapped);
}

// A unit built the way workspace.c builds one for a .lton: the wrapped text,
// under the file's own path. What the positions have to answer in is the
// file's, which is what makes the path matter here.
typedef struct {
    LhatSource source;
    LhatLexer lexer;
    LhatParseResult parsed;
    LhatCheckResult checked;
    LhatUnit unit;
    char *wrapped;
} Checked;

static void check_lton(Checked *c, const char *path, const char *body)
{
    size_t whole = 0;
    c->wrapped = lsp_lton_wrap(body, strlen(body), &whole);
    lhat_source_init_from_string(&c->source, path, c->wrapped, whole);
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
    free(c->wrapped);
}

static void test_positions_are_the_files(void)
{
    Checked c;

    LHAT_TEST("08 の 7: a position is where the file has it, not the wrapper");
    static const char *const body = "first = 1,\nsecond = 2,\n";
    check_lton(&c, "conf.lton", body);

    uint32_t prologue = lsp_lton_prologue_length();
    // `second` begins the second line of the file, and the wrapper took no
    // line, so it is the second line of the wrapped text too.
    const char *second = strstr(c.source.text, "second");
    LHAT_CHECK(second != NULL, "expected the second line to be there");
    if (second != NULL) {
        LspPosition at =
            lsp_unit_position_at(&c.unit,
                                 (uint32_t)(second - c.source.text));
        LHAT_CHECK_EQ_INT(at.line, 1);
        LHAT_CHECK_EQ_INT(at.character, 0);
    }

    // The first line is the one the prologue shares, so its columns are the
    // ones that move -- and this is what takes them back.
    const char *first = strstr(c.source.text, "first");
    LHAT_CHECK(first != NULL, "expected the first line to be there");
    if (first != NULL) {
        uint32_t offset = (uint32_t)(first - c.source.text);
        LHAT_CHECK_EQ_INT(offset, prologue);  // right after the wrapper
        LspPosition at = lsp_unit_position_at(&c.unit, offset);
        LHAT_CHECK_EQ_INT(at.line, 0);
        LHAT_CHECK_EQ_INT(at.character, 0);

        // And back the other way, which is what an editor's cursor arrives
        // as: the file's line 0 column 0 is the byte after the prologue.
        LHAT_CHECK_EQ_INT(lsp_unit_offset_at(&c.unit, 0, 0), prologue);
    }
    check_dispose(&c);

    // A unit is not wrapped, so nothing is taken off it. The path is the
    // whole of what tells the two apart.
    LHAT_TEST("and a unit's positions are left alone");
    check_lton(&c, "main.lh", body);
    {
        LspPosition at = lsp_unit_position_at(&c.unit, 0);
        LHAT_CHECK_EQ_INT(at.line, 0);
        LHAT_CHECK_EQ_INT(at.character, 0);
        LHAT_CHECK_EQ_INT(lsp_unit_offset_at(&c.unit, 0, 0), 0);
    }
    check_dispose(&c);
}

// 08 の 4: the body is read as an f^'s, and 15.1 says an f^ may call only an
// f^ -- so what has an effect cannot be written in one. Nothing here checks
// for that: the rule the language already had is the boundary.
static void test_what_may_be_written(void)
{
    Checked c;

    LHAT_TEST("08 の 4: arithmetic and nested tables go through");
    check_lton(&c, "conf.lton",
               "width = 480 * 2,\n"
               "name = \"lhat\" .. \"ove\",\n"
               "window = { title = \"a window\", size = { 1, 2 } },\n");
    LHAT_CHECK_EQ_INT(c.parsed.diagnostic_count, 0);
    LHAT_CHECK_EQ_INT(c.checked.diagnostic_count, 0);
    check_dispose(&c);

    LHAT_TEST("08 の 5: and no name from outside is in scope");
    check_lton(&c, "conf.lton", "here = elsewhere,\n");
    LHAT_CHECK(c.checked.diagnostic_count > 0,
               "expected the name to be refused");
    check_dispose(&c);
}

int main(void)
{
    test_which_paths_are_lton();
    test_the_wrapping_is_the_librarys();
    test_positions_are_the_files();
    test_what_may_be_written();
    return lhat_test_report("test_lton_server");
}
