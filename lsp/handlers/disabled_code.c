// L^ (lhat) -- LSP server: lhat/toggleDisabledCode (01 の 6.5).
//
// Answered from the editor's own text, the way document_symbol.c answers:
// the command may be pressed on the keystroke after an edit, before the
// worker has checked anything, and the edits handed back must land on the
// text the editor holds. Only the tree is needed, so nothing is checked.

#include "disabled_code.h"

#include <stdlib.h>
#include <string.h>

#include "lhat/lexer.h"
#include "lhat/source.h"
#include "parser.h"

// The answer itself, disambiguated from this handler's own header of the
// same basename, the way ast.c and document_symbol.c already have to say.
#include "../disabled_code.h"

#include "lton.h"
#include "position.h"
#include "server.h"
#include "uri.h"
#include "workspace.h"

static bool position_of(const cJSON *range, const char *end, int *line,
                        int *character)
{
    const cJSON *at = cJSON_GetObjectItemCaseSensitive(range, end);
    const cJSON *l = cJSON_GetObjectItemCaseSensitive(at, "line");
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(at, "character");
    if (!cJSON_IsNumber(l) || !cJSON_IsNumber(c)) {
        return false;
    }
    *line = l->valueint;
    *character = c->valueint;
    return true;
}

cJSON *lsp_handle_toggle_disabled_code(LspServer *server, const cJSON *params)
{
    if (params == NULL) {
        return NULL;
    }
    const cJSON *text_document =
        cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    const cJSON *range = cJSON_GetObjectItemCaseSensitive(params, "range");
    const cJSON *uri_item = cJSON_GetObjectItemCaseSensitive(text_document, "uri");
    int from_line = 0;
    int from_character = 0;
    int to_line = 0;
    int to_character = 0;
    if (!cJSON_IsString(uri_item) ||
        !position_of(range, "start", &from_line, &from_character) ||
        !position_of(range, "end", &to_line, &to_character)) {
        return NULL;
    }

    char *path = lsp_uri_to_absolute_path(uri_item->valuestring);
    // 08 章: an LTON file is data, with no statements to switch off.
    if (path == NULL || lsp_lton_is_path(path)) {
        free(path);
        return NULL;
    }
    size_t length = 0;
    char *text = lsp_document_store_copy(&server->workspace.documents, path,
                                         &length);
    if (text == NULL) {
        free(path);
        return NULL;  // not open: no editor to hand edits to
    }

    LhatUnit unit;
    memset(&unit, 0, sizeof unit);
    unit.path = path;
    unit.loaded = true;
    cJSON *answer = NULL;
    if (lhat_source_init_from_string(&unit.source, path, text, length)) {
        lhat_lexer_init(&unit.lexer, &unit.source);
        lhat_parse(&unit.lexer, &unit.parsed);
        answer = (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params, "exact"))
                      ? lsp_disabled_code_toggle_exact : lsp_disabled_code_toggle)(
            &unit, lsp_unit_offset_at(&unit, from_line, from_character),
            lsp_unit_offset_at(&unit, to_line, to_character));
        lhat_parse_result_dispose(&unit.parsed);
        lhat_lexer_dispose(&unit.lexer);
        lhat_source_dispose(&unit.source);
    }
    free(text);
    free(path);
    return answer;
}
