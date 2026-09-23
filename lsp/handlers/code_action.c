// L^ (lhat) -- LSP server: textDocument/codeAction.
//
// Answered from the checked unit, since a fix is the checker's or the
// parser's to work out (07 §6) and the unit is what carries it. The client's
// `context.diagnostics` is not read: what this offers comes from the same
// check the diagnostics came from, so reading them back would only be a
// second way to say the same thing -- and a stale one, if the file moved on.

#include "code_action.h"

#include <stdlib.h>
#include <string.h>

// The answer itself, disambiguated from this handler's own header of the same
// basename, the way definition.c and hover.c already have to say.
#include "../code_action.h"

#include "position.h"
#include "server.h"
#include "uri.h"
#include "workspace.h"

typedef struct {
    const char *uri;
    int start_line;
    int start_character;
    int end_line;
    int end_character;
    cJSON *actions;  // the array to answer with, or NULL until one is made
} CodeActionRequest;

static void offer(void *context, const LhatUnit *unit)
{
    CodeActionRequest *request = (CodeActionRequest *)context;
    uint32_t from =
        lsp_unit_offset_at(unit, request->start_line, request->start_character);
    uint32_t to =
        lsp_unit_offset_at(unit, request->end_line, request->end_character);
    request->actions =
        lsp_code_actions_for_unit(unit, request->uri, from, to);
}

// One end of the range the editor asked about. Absent or malformed reads as
// the start of the file, which offers what stands there rather than nothing.
static void read_position(const cJSON *range, const char *which, int *line,
                          int *character)
{
    const cJSON *at = cJSON_GetObjectItemCaseSensitive(range, which);
    const cJSON *line_item = cJSON_GetObjectItemCaseSensitive(at, "line");
    const cJSON *character_item =
        cJSON_GetObjectItemCaseSensitive(at, "character");
    *line = cJSON_IsNumber(line_item) ? (int)line_item->valuedouble : 0;
    *character =
        cJSON_IsNumber(character_item) ? (int)character_item->valuedouble : 0;
}

cJSON *lsp_handle_code_action(LspServer *server, const cJSON *params)
{
    if (params == NULL) {
        return cJSON_CreateArray();
    }
    const cJSON *text_document =
        cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    const cJSON *uri_item =
        cJSON_GetObjectItemCaseSensitive(text_document, "uri");
    if (!cJSON_IsString(uri_item)) {
        return cJSON_CreateArray();
    }
    char *path = lsp_uri_to_absolute_path(uri_item->valuestring);
    if (path == NULL) {
        return cJSON_CreateArray();
    }

    CodeActionRequest request;
    memset(&request, 0, sizeof request);
    request.uri = uri_item->valuestring;
    const cJSON *range = cJSON_GetObjectItemCaseSensitive(params, "range");
    read_position(range, "start", &request.start_line, &request.start_character);
    read_position(range, "end", &request.end_line, &request.end_character);

    lsp_workspace_with_unit(&server->workspace, path, offer, &request);
    free(path);
    return request.actions != NULL ? request.actions : cJSON_CreateArray();
}
