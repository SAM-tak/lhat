// L^ (lhat) -- LSP server tests: LhatUnit diagnostics -> LSP Diagnostic[],
// in particular the UTF-16 position conversion (diagnostics.c).

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "lexer.h"
#include "parser.h"
#include "source.h"

#include "diagnostics.h"
#include "testutil.h"

typedef struct {
    int line;
    int character;
} Pos;

// Checks `text` as a standalone unit (no program.h graph needed -- lhat_check
// takes a lexer and a tree directly) and reads back the first diagnostic's
// start position.
static bool first_diagnostic_start(const char *text, Pos *out)
{
    LhatSource source;
    lhat_source_init_from_string(&source, "test.lh", text, strlen(text));
    LhatLexer lexer;
    lhat_lexer_init(&lexer, &source);
    LhatParseResult parsed;
    lhat_parse(&lexer, &parsed);
    LhatCheckResult checked;
    lhat_check(parsed.root, &lexer, true, &checked);

    LhatUnit unit;
    memset(&unit, 0, sizeof unit);
    unit.path = (char *)"test.lh";
    unit.loaded = true;
    unit.source = source;
    unit.lexer = lexer;
    unit.parsed = parsed;
    unit.checked = checked;

    cJSON *diags = lsp_diagnostics_for_unit(&unit);
    bool found = false;
    if (diags != NULL && cJSON_GetArraySize(diags) > 0) {
        cJSON *first = cJSON_GetArrayItem(diags, 0);
        cJSON *range = cJSON_GetObjectItemCaseSensitive(first, "range");
        cJSON *start = cJSON_GetObjectItemCaseSensitive(range, "start");
        out->line = cJSON_GetObjectItemCaseSensitive(start, "line")->valueint;
        out->character =
            cJSON_GetObjectItemCaseSensitive(start, "character")->valueint;
        found = true;
    }
    cJSON_Delete(diags);

    lhat_check_result_dispose(&checked);
    lhat_parse_result_dispose(&parsed);
    lhat_lexer_dispose(&lexer);
    lhat_source_dispose(&source);
    return found;
}

static void test_ascii_position(void)
{
    LHAT_TEST("undefined name: ascii column count");
    //         0    1    2    3    4    5    6    7    8    9
    //         l    e    t    ^    _    x    _    =    _    n(owhere)
    Pos pos;
    bool found = first_diagnostic_start("let^ x = nowhere\n", &pos);
    LHAT_CHECK(found, "expected a diagnostic");
    if (found) {
        LHAT_CHECK_EQ_INT(pos.line, 0);
        LHAT_CHECK_EQ_INT(pos.character, 9);
    }
}

static void test_surrogate_pair_position(void)
{
    LHAT_TEST("a supplementary-plane character counts as two UTF-16 units");
    // U+1F600 (a face emoji) is one Unicode code point but a UTF-16
    // surrogate pair -- two code units. DesignDocuments/03-compilation-
    // pipeline.md's 1.3節 already keeps a code-point column apart from a
    // terminal-cell one; this is the LSP server's own third count.
    Pos pos;
    bool found = first_diagnostic_start(
        "let^ s = \"\xF0\x9F\x98\x80\" .. nowhere\n", &pos);
    LHAT_CHECK(found, "expected a diagnostic");
    if (found) {
        LHAT_CHECK_EQ_INT(pos.line, 0);
        // Code points up to "nowhere": l e t ^ _ s _ = _ " 😀 " _ . . _  = 16
        // UTF-16 units: the same walk, but 😀 counts twice -> 17.
        LHAT_CHECK_EQ_INT(pos.character, 17);
    }
}

int main(void)
{
    test_ascii_position();
    test_surrogate_pair_position();
    return lhat_test_report("test_lsp_diagnostics");
}
