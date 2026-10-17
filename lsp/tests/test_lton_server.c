// L^ (lhat) -- LSP server tests: a .lton file, checked as what it is.
//
// Syntax, source coordinates, and imported-name navigation share the native parser.

#include <stdlib.h>
#include <string.h>

#include "lhat/lexer.h"
#include "lhat/port.h"
#include "lhat/source.h"
#include "parser.h"
#include "check.h"
#include "references.h"

#include "lton.h"
#include "position.h"
#include "testutil.h"

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

typedef struct {
    LhatSource source;
    LhatLexer lexer;
    LhatParseResult parsed;
    LhatCheckResult checked;
    LhatUnit unit;
} Checked;

static void check_lton(Checked *c, const char *path, const char *body)
{
    size_t whole = strlen(body);
    lhat_source_init_from_string(&c->source, path, body, whole);
    lhat_lexer_init(&c->lexer, &c->source);
    lhat_parse_lton(&c->lexer, &c->parsed);
    memset(&c->checked, 0, sizeof c->checked);

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
    lhat_check_result_dispose(&c->unit.checked);
    lhat_parse_result_dispose(&c->parsed);
    lhat_lexer_dispose(&c->lexer);
    lhat_source_dispose(&c->source);
}

static void test_positions_are_the_files(void)
{
    Checked c;

    LHAT_TEST("08 の 7: a position is where the file has it, not the wrapper");
    static const char *const body = "first = 1,\nsecond = 2,\n";
    check_lton(&c, "conf.lton", body);

    // Native parsing does not insert text or shift any source coordinates.
    const char *second = strstr(c.source.text, "second");
    LHAT_CHECK(second != NULL, "expected the second line to be there");
    if (second != NULL) {
        LspPosition at =
            lsp_unit_position_at(&c.unit,
                                 (uint32_t)(second - c.source.text));
        LHAT_CHECK_EQ_INT(at.line, 1);
        LHAT_CHECK_EQ_INT(at.character, 0);
    }

    // First-line columns are unchanged as well.
    const char *first = strstr(c.source.text, "first");
    LHAT_CHECK(first != NULL, "expected the first line to be there");
    if (first != NULL) {
        uint32_t offset = (uint32_t)(first - c.source.text);
        LHAT_CHECK_EQ_INT(offset, 0);
        LspPosition at = lsp_unit_position_at(&c.unit, offset);
        LHAT_CHECK_EQ_INT(at.line, 0);
        LHAT_CHECK_EQ_INT(at.character, 0);

        // Cursor positions map directly back to source bytes.
        LHAT_CHECK_EQ_INT(lsp_unit_offset_at(&c.unit, 0, 0), 0);
    }
    check_dispose(&c);

    // Ordinary units use the same coordinate mapping.
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

// Editor validation is syntax-only. Runtime loading still checks semantics.
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

    LHAT_TEST("LTON editor validation does not resolve names or generate types");
    check_lton(&c, "conf.lton", "here = elsewhere,\n");
    LHAT_CHECK_EQ_INT(c.parsed.diagnostic_count, 0);
    LHAT_CHECK_EQ_INT(c.checked.diagnostic_count, 0);
    LHAT_CHECK(c.checked.types == NULL, "no semantic arena");
    check_dispose(&c);

    LHAT_TEST("LTON editor validation still diagnoses malformed table syntax");
    check_lton(&c, "conf.lton", "here = { nested = },\n");
    LHAT_CHECK(c.parsed.diagnostic_count > 0, "syntax error retained");
    check_dispose(&c);
}

#if LHAT_WITH_RESOLUTIONS
static char *load_enum_module(void *context, const char *path, size_t *length)
{
    (void)context;
    if (strcmp(path, "project/src/bar.lh") != 0) return NULL;
    const char *source = "module^game.bar\npublic^enum^Mode {FOO, OTHER}\n";
    *length = strlen(source);
    char *copy = lhat_alloc(*length + 1);
    if (copy != NULL) memcpy(copy, source, *length + 1);
    return copy;
}

static void count_reference(void *context, const LhatUnit *unit, uint32_t from, uint32_t to)
{
    (void)unit;
    (void)from;
    (void)to;
    (*(size_t *)context)++;
}

static void test_imported_names(void)
{
    LHAT_TEST("LTON resolves enum names for navigation and rename without inferring table types");
    LhatProgram *program = lhat_program_new(true, load_enum_module, NULL);
    LHAT_REQUIRE(program != NULL && lhat_program_set_project_root(program, "project"), "project configured");
    Checked c;
    static const char text[] = "let^b = require^\"$src/bar.lh\"\na = b.Mode.FOO,\nb = b.Mode.FOO,\n";
    check_lton(&c, "project/settings.lton", text);
    c.unit.program = program;
    lsp_lton_resolve(program, &c.unit);
    uint32_t offset = (uint32_t)(strstr(text, ".FOO") - text + 1);
    const LhatResolution *resolution = lhat_check_resolution_at(&c.unit.checked, offset);
    LHAT_REQUIRE(resolution != NULL && resolution->has_definition, "enum member resolves");
    LHAT_CHECK(strcmp(resolution->definition_path, "project/src/bar.lh") == 0, "declaration is in required file");
    LHAT_CHECK(c.unit.checked.types == NULL, "no LTON type arena allocated");
    LhatCompletionItem items[8];
    size_t offered = lhat_unit_completion_items(&c.unit, offset, items, 8);
    bool foo = false, other = false;
    for (size_t i = 0; i < offered && i < 8; i++) {
        if (strcmp(items[i].label, "FOO") == 0) foo = true;
        if (strcmp(items[i].label, "OTHER") == 0) other = true;
    }
    LHAT_CHECK(foo && other && c.unit.checked.types == NULL, "enum completion needs no data type arena");
    LspReferenceTarget target;
    LHAT_REQUIRE(lsp_references_target(&c.unit, offset + 1, &target), "rename target resolved");
    size_t count = 0;
    lsp_references_in_unit(&c.unit, &target, count_reference, &count);
    LHAT_CHECK_EQ_INT(count, 2);
    check_dispose(&c);

    static const char bare[] = "require^\"$src/bar.lh\"\na = game.bar.Mode.FOO,\n";
    check_lton(&c, "project/settings.lton", bare);
    c.unit.program = program;
    lsp_lton_resolve(program, &c.unit);
    offset = (uint32_t)(strstr(bare, ".FOO") - bare + 1);
    resolution = lhat_check_resolution_at(&c.unit.checked, offset);
    LHAT_CHECK(resolution != NULL && resolution->has_definition, "bare require uses the declared qualified module");
    check_dispose(&c);
    lhat_program_free(program);
}
#endif

int main(void)
{
    test_which_paths_are_lton();
#if LHAT_WITH_RESOLUTIONS
    test_imported_names();
#endif
    test_positions_are_the_files();
    test_what_may_be_written();
    return lhat_test_report("test_lton_server");
}
