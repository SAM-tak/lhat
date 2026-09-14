// L^ (lhat) -- LSP server tests: lhat/toggleDisabledCode (disabled_code.c),
// which switches statements off with '#[~ ... ]#' and on again (01 の 6.5).

#include <stdlib.h>
#include <string.h>

#include "lhat/lexer.h"
#include "lhat/source.h"
#include "parser.h"

#include "disabled_code.h"
#include "position.h"
#include "testutil.h"

typedef struct {
    uint32_t from;
    uint32_t to;
    const char *text;
} Edit;

static uint32_t offset_of(const char *text, size_t length, cJSON *range,
                          const char *end)
{
    cJSON *at = cJSON_GetObjectItemCaseSensitive(range, end);
    return lsp_offset_at(
        text, length, cJSON_GetObjectItemCaseSensitive(at, "line")->valueint,
        cJSON_GetObjectItemCaseSensitive(at, "character")->valueint);
}

// What the edits make of `text`. They never overlap, so taken in order of
// where they start they compose into one pass over it.
static char *apply(const char *text, cJSON *edits)
{
    size_t length = strlen(text);
    Edit list[4];
    int count = cJSON_GetArraySize(edits);
    if (count > 4) {
        return NULL;
    }
    size_t grown = length + 1;
    for (int i = 0; i < count; i++) {
        cJSON *edit = cJSON_GetArrayItem(edits, i);
        cJSON *range = cJSON_GetObjectItemCaseSensitive(edit, "range");
        Edit e = {offset_of(text, length, range, "start"),
                  offset_of(text, length, range, "end"),
                  cJSON_GetObjectItemCaseSensitive(edit, "newText")->valuestring};
        int at = i;
        while (at > 0 && list[at - 1].from > e.from) {
            list[at] = list[at - 1];
            at--;
        }
        list[at] = e;
        grown += strlen(e.text);
    }

    char *out = (char *)malloc(grown);
    size_t used = 0;
    uint32_t cursor = 0;
    for (int i = 0; i < count; i++) {
        memcpy(out + used, text + cursor, list[i].from - cursor);
        used += list[i].from - cursor;
        memcpy(out + used, list[i].text, strlen(list[i].text));
        used += strlen(list[i].text);
        cursor = list[i].to;
    }
    memcpy(out + used, text + cursor, length - cursor + 1);
    return out;
}

// `marked` is the text with '$' at the selection: once for a cursor, twice
// around a selection. A NULL `want` means the command refuses.
static void expect_toggle(const char *marked, const char *want)
{
    char text[256];
    uint32_t marks[2] = {0, 0};
    int count = 0;
    size_t length = 0;
    for (const char *c = marked; *c != '\0' && length + 1 < sizeof text; c++) {
        if (*c == '$' && count < 2) {
            marks[count++] = (uint32_t)length;
        } else {
            text[length++] = *c;
        }
    }
    text[length] = '\0';

    LhatUnit unit;
    memset(&unit, 0, sizeof unit);
    unit.path = (char *)"test.lh";
    unit.loaded = true;
    lhat_source_init_from_string(&unit.source, "test.lh", text, length);
    lhat_lexer_init(&unit.lexer, &unit.source);
    lhat_parse(&unit.lexer, &unit.parsed);

    cJSON *answer = lsp_disabled_code_toggle(
        &unit, marks[0], count == 2 ? marks[1] : marks[0]);
    cJSON *edits = cJSON_GetObjectItemCaseSensitive(answer, "edits");
    if (want == NULL) {
        LHAT_CHECK(cJSON_IsString(
                       cJSON_GetObjectItemCaseSensitive(answer, "refusal")),
                   "expected a refusal for \"%s\"", marked);
    } else if (!cJSON_IsArray(edits)) {
        LHAT_CHECK(false, "no edits for \"%s\"", marked);
    } else {
        char *after = apply(text, edits);
        LHAT_CHECK(after != NULL && strcmp(after, want) == 0,
                   "for \"%s\"\n got \"%s\"\nwant \"%s\"", marked,
                   after != NULL ? after : "(null)", want);
        free(after);
    }

    cJSON_Delete(answer);
    lhat_parse_result_dispose(&unit.parsed);
    lhat_lexer_dispose(&unit.lexer);
    lhat_source_dispose(&unit.source);
}

static void test_wrap(void)
{
    LHAT_TEST("the cursor's statement is wrapped, each marker on its own line");
    expect_toggle("let^ a = 1\nprint(a)$\nprint(a)\n",
                  "let^ a = 1\n#[~\nprint(a)\n]#\nprint(a)\n");

    LHAT_TEST("the markers take the statement's indentation");
    expect_toggle("if^ true^ {\n    print(1)$\n}\n",
                  "if^ true^ {\n    #[~\n    print(1)\n    ]#\n}\n");

    LHAT_TEST("a selection is widened to the whole statements it touches");
    expect_toggle("f()\ng($)\nh($)\nk()\n", "f()\n#[~\ng()\nh()\n]#\nk()\n");

    LHAT_TEST("a statement on several lines is wrapped whole from any of them");
    expect_toggle("print(\n    1$\n)\n", "#[~\nprint(\n    1\n)\n]#\n");

    // There is no switching off half of 'if^ c {'.
    LHAT_TEST("a selection reaching a construct's own line takes all of it");
    expect_toggle("$if^ true^ {\n    f()$\n}\n",
                  "#[~\nif^ true^ {\n    f()\n}\n]#\n");
    expect_toggle("if^ true^ { f()$ }\n", "#[~\nif^ true^ { f() }\n]#\n");

    // Not at the head of the unit, where 02 の 18.4 makes one the unit's.
    LHAT_TEST("an annotation goes with its declaration");
    expect_toggle("f()\n@sample(1)\nlet^ y = 1$\n",
                  "f()\n#[~\n@sample(1)\nlet^ y = 1\n]#\n");

    LHAT_TEST("a comment line selected along with the statement goes in too");
    expect_toggle("$# why\nf()$\n", "#[~\n# why\nf()\n]#\n");

    LHAT_TEST("a block comment inside pairs up, so it does not stand in the way");
    expect_toggle("f()  #[ note ]#$\n", "#[~\nf()  #[ note ]#\n]#\n");
}

static void test_unwrap(void)
{
    LHAT_TEST("inside disabled code, the markers are taken away");
    expect_toggle("#[~\nf()$\n]#\ng()\n", "f()\ng()\n");
    expect_toggle("if^ true^ {\n    #[~\n    f()$\n    ]#\n}\n",
                  "if^ true^ {\n    f()\n}\n");

    LHAT_TEST("from the marker's own line too");
    expect_toggle("#[~$\nf()\n]#\n", "f()\n");

    LHAT_TEST("markers on the code's own line go with the space beside them");
    expect_toggle("#[~ f()$ ]#\n", "f()\n");

    LHAT_TEST("where they nest, only the innermost");
    expect_toggle("#[~\nf()\n#[~\ng()$\n]#\n]#\n", "#[~\nf()\ng()\n]#\n");
}

static void test_refusals(void)
{
    LHAT_TEST("nothing is wrapped where no statement is");
    expect_toggle("f()\n\n$\ng()\n", NULL);
    expect_toggle("# note$\nf()\n", NULL);
    expect_toggle("if^ true^ {\n    # note$\n    f()\n}\n", NULL);

    // 6.2: the lexer counts them inside a string as well.
    LHAT_TEST("nor where a '#[' or ']#' would not pair up");
    expect_toggle("let^ s = \"]#\"$\n", NULL);
    expect_toggle("let^ s = \"#[\"$\n", NULL);
}

int main(void)
{
#if LHAT_WITH_COMMENTS
    test_wrap();
    test_unwrap();
    test_refusals();
#endif
    return lhat_test_report("test_disabled_code");
}
