// L^ (lhat) -- LSP server tests: LhatFix -> LSP CodeAction[] (code_action.c).
//
// What is checked here is the translation, not the fix: that a diagnostic
// which knows one is offered with its title, its kind and an edit at the
// right place, that a diagnostic which knows none is offered nothing, and
// that the range the editor asked about is what decides who is asked.

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "lhat/lexer.h"
#include "lhat/source.h"
#include "parser.h"

#include "code_action.h"
#include "testutil.h"

// `text` as a standalone unit -- lhat_check takes a lexer and a tree, so no
// program graph is needed (test_diagnostics.c does the same).
typedef struct {
    LhatSource source;
    LhatLexer lexer;
    LhatParseResult parsed;
    LhatCheckResult checked;
    LhatUnit unit;
} Checked;

static void check_text(Checked *c, const char *text)
{
    memset(c, 0, sizeof *c);
    lhat_source_init_from_string(&c->source, "test.lh", text, strlen(text));
    lhat_lexer_init(&c->lexer, &c->source);
    lhat_parse(&c->lexer, &c->parsed);
    lhat_check(c->parsed.root, &c->lexer, true, &c->checked);
    c->unit.path = (char *)"test.lh";
    c->unit.loaded = true;
    c->unit.source = c->source;
    c->unit.lexer = c->lexer;
    c->unit.parsed = c->parsed;
    c->unit.checked = c->checked;
}

static void dispose(Checked *c)
{
    lhat_check_result_dispose(&c->checked);
    lhat_parse_result_dispose(&c->parsed);
    lhat_lexer_dispose(&c->lexer);
    lhat_source_dispose(&c->source);
}

static const char *string_at(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

// The one edit of the first action, as the editor would read it.
static const cJSON *only_edit(const cJSON *action, const char *uri)
{
    const cJSON *edit = cJSON_GetObjectItemCaseSensitive(action, "edit");
    const cJSON *changes = cJSON_GetObjectItemCaseSensitive(edit, "changes");
    const cJSON *edits = cJSON_GetObjectItemCaseSensitive(changes, uri);
    return cJSON_IsArray(edits) && cJSON_GetArraySize(edits) == 1
               ? cJSON_GetArrayItem(edits, 0)
               : NULL;
}

int main(void)
{
    const char *uri = "file:///test.lh";

    LHAT_TEST("a token the parser wanted is offered as a fix");
    {
        // The '}' the body never closed: the parser notices at the end of the
        // input, which is where the fix writes it.
        Checked c;
        check_text(&c, "var^ f = p^ {\n  var^ a = 1\n");
        cJSON *actions = lsp_code_actions_for_unit(&c.unit, uri, 0,
                                                  (uint32_t)c.source.length);
        LHAT_CHECK(actions != NULL && cJSON_GetArraySize(actions) >= 1,
                   "one action at least");
        if (actions != NULL && cJSON_GetArraySize(actions) >= 1) {
            const cJSON *action = cJSON_GetArrayItem(actions, 0);
            const char *title = string_at(action, "title");
            LHAT_CHECK(title != NULL && strcmp(title, "write '}'") == 0,
                       "the title names what it writes: '%s'",
                       title != NULL ? title : "(none)");
            const char *kind = string_at(action, "kind");
            LHAT_CHECK(kind != NULL && strcmp(kind, "quickfix") == 0,
                       "it is a quick fix");
            LHAT_CHECK(cJSON_GetObjectItemCaseSensitive(action,
                                                        "isPreferred") == NULL,
                       "a suggested fix is not the preferred one");
            const cJSON *edit = only_edit(action, uri);
            LHAT_CHECK(edit != NULL, "one edit, under the document's own URI");
            if (edit != NULL) {
                const char *wrote = string_at(edit, "newText");
                LHAT_CHECK(wrote != NULL && strcmp(wrote, "}") == 0,
                           "it writes the token");
                const cJSON *range =
                    cJSON_GetObjectItemCaseSensitive(edit, "range");
                const cJSON *start =
                    cJSON_GetObjectItemCaseSensitive(range, "start");
                const cJSON *end =
                    cJSON_GetObjectItemCaseSensitive(range, "end");
                LHAT_CHECK_EQ_INT(
                    cJSON_GetObjectItemCaseSensitive(start, "line")->valueint,
                    cJSON_GetObjectItemCaseSensitive(end, "line")->valueint);
                LHAT_CHECK_EQ_INT(
                    cJSON_GetObjectItemCaseSensitive(start, "character")
                        ->valueint,
                    cJSON_GetObjectItemCaseSensitive(end, "character")
                        ->valueint);
            }
        }
        cJSON_Delete(actions);
        dispose(&c);
    }

    LHAT_TEST("a diagnostic with no fix is offered nothing");
    {
        Checked c;
        check_text(&c, "var^ x : string^ = 1\n");
        cJSON *actions = lsp_code_actions_for_unit(&c.unit, uri, 0,
                                                   (uint32_t)c.source.length);
        LHAT_CHECK(actions != NULL && cJSON_GetArraySize(actions) == 0,
                   "the checker works none out yet");
        cJSON_Delete(actions);
        dispose(&c);
    }

    LHAT_TEST("the range asked about is what decides who is asked");
    {
        Checked c;
        check_text(&c, "var^ f = p^ {\n  var^ a = 1\n");
        // The first line holds no diagnostic of its own: the missing '}' is
        // noticed at the end of the input.
        cJSON *actions = lsp_code_actions_for_unit(&c.unit, uri, 0, 5);
        LHAT_CHECK(actions != NULL && cJSON_GetArraySize(actions) == 0,
                   "nothing is offered where nothing was reported");
        cJSON_Delete(actions);
        dispose(&c);
    }

    LHAT_TEST("a unit with nothing wrong offers nothing");
    {
        Checked c;
        check_text(&c, "var^ x = 1\n");
        cJSON *actions = lsp_code_actions_for_unit(&c.unit, uri, 0,
                                                   (uint32_t)c.source.length);
        LHAT_CHECK(actions != NULL && cJSON_GetArraySize(actions) == 0,
                   "an empty array, not nothing at all");
        cJSON_Delete(actions);
        dispose(&c);
    }

    return lhat_test_report("test_code_action");
}
