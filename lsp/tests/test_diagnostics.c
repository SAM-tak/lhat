// L^ (lhat) -- LSP server tests: LhatUnit diagnostics -> LSP Diagnostic[],
// in particular the UTF-16 position conversion (diagnostics.c).

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "lhat/lexer.h"
#include "parser.h"
#include "lhat/source.h"

#include "diagnostics.h"
#include "testutil.h"

typedef struct {
    int line;
    int character;
    int severity;
} Pos;

// Checks `text` as a standalone unit (no program.h graph needed -- lhat_check
// takes a lexer and a tree directly) and reads back the first diagnostic's
// start position and severity.
static bool first_diagnostic_start(const char *text, bool relaxed, Pos *out)
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

    cJSON *diags = lsp_diagnostics_for_unit(&unit, relaxed);
    bool found = false;
    if (diags != NULL && cJSON_GetArraySize(diags) > 0) {
        cJSON *first = cJSON_GetArrayItem(diags, 0);
        cJSON *range = cJSON_GetObjectItemCaseSensitive(first, "range");
        cJSON *start = cJSON_GetObjectItemCaseSensitive(range, "start");
        out->line = cJSON_GetObjectItemCaseSensitive(start, "line")->valueint;
        out->character =
            cJSON_GetObjectItemCaseSensitive(start, "character")->valueint;
        out->severity =
            cJSON_GetObjectItemCaseSensitive(first, "severity")->valueint;
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
    bool found = first_diagnostic_start("var^ x = nowhere\n", false, &pos);
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
        "var^ s = \"\xF0\x9F\x98\x80\" .. nowhere\n", false, &pos);
    LHAT_CHECK(found, "expected a diagnostic");
    if (found) {
        LHAT_CHECK_EQ_INT(pos.line, 0);
        // Code points up to "nowhere": l e t ^ _ s _ = _ " 😀 " _ . . _  = 16
        // UTF-16 units: the same walk, but 😀 counts twice -> 17.
        LHAT_CHECK_EQ_INT(pos.character, 17);
    }
}

// The compile stage's own refusal reaches the editor too -- it reports
// through lhat_program_compile_failure rather than the unit's diagnostics,
// and used to be invisible in the editor while the CLI showed it.
static void test_compile_failure(void)
{
    LHAT_TEST("a compile refusal becomes a diagnostic");

    LhatSource source;
    const char *text = "var^ s = 1\n";
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

    cJSON *diags = lsp_diagnostics_for_unit(&unit, false);
    LHAT_CHECK(diags != NULL && cJSON_GetArraySize(diags) == 0,
               "a clean unit reports nothing");

    LhatCompileResult failure;
    memset(&failure, 0, sizeof failure);
    failure.status = LHAT_COMPILE_UNSUPPORTED;
    failure.offset = 5;
    failure.line = 1;
    failure.column = 6;
    lsp_diagnostics_add_compile_failure(diags, &unit, failure);
    LHAT_CHECK_EQ_INT(cJSON_GetArraySize(diags), 1);
    if (cJSON_GetArraySize(diags) == 1) {
        cJSON *first = cJSON_GetArrayItem(diags, 0);
        const char *message =
            cJSON_GetObjectItemCaseSensitive(first, "message")->valuestring;
        LHAT_CHECK(strstr(message, "does not compile") != NULL,
                   "the compiler's own words");
    }

    // And LHAT_COMPILE_OK appends nothing.
    memset(&failure, 0, sizeof failure);
    lsp_diagnostics_add_compile_failure(diags, &unit, failure);
    LHAT_CHECK_EQ_INT(cJSON_GetArraySize(diags), 1);

    cJSON_Delete(diags);
    lhat_check_result_dispose(&checked);
    lhat_parse_result_dispose(&parsed);
    lhat_lexer_dispose(&lexer);
    lhat_source_dispose(&source);
}

// 03 の 3.1 with lhat_unit_diagnostic_relaxed_ok: a gap strict alone
// reports is Warning under a relaxed project and Error otherwise -- an
// ordinary mismatch is Error either way.
static void test_relaxed_severity(void)
{
    LHAT_TEST("a strict-only gap is a warning under a relaxed project");
    {
        const char *text = "let^ f = f^ n -> number^ { return^ 1 }\n";
        Pos strict_pos, relaxed_pos;
        LHAT_CHECK(first_diagnostic_start(text, false, &strict_pos),
                   "strict reports the gap");
        LHAT_CHECK_EQ_INT(strict_pos.severity, 1);
        LHAT_CHECK(first_diagnostic_start(text, true, &relaxed_pos),
                   "and so does a tool checking strict regardless");
        LHAT_CHECK_EQ_INT(relaxed_pos.severity, 2);
    }

    LHAT_TEST("but an ordinary mismatch stays an error under either");
    {
        const char *text = "var^ x : number^ = \"s\"\n";
        Pos strict_pos, relaxed_pos;
        LHAT_CHECK(first_diagnostic_start(text, false, &strict_pos), "strict");
        LHAT_CHECK_EQ_INT(strict_pos.severity, 1);
        LHAT_CHECK(first_diagnostic_start(text, true, &relaxed_pos), "relaxed");
        LHAT_CHECK_EQ_INT(relaxed_pos.severity, 1);
    }
}

int main(void)
{
    test_ascii_position();
    test_surrogate_pair_position();
    test_compile_failure();
    test_relaxed_severity();
    return lhat_test_report("test_lsp_diagnostics");
}
