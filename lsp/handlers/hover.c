// L^ (lhat) -- LSP server: textDocument/hover.

#include "hover.h"

#include <stdlib.h>

// The hover-building logic, disambiguated from this handler's own header of
// the same basename -- a bare "hover.h" resolves to this file's neighbour
// first, the way semantic_tokens.c already has to say.
#include "../hover.h"

#include "position.h"
#include "server.h"
#include "uri.h"
#include "workspace.h"

// The position is turned into an offset here rather than by the caller,
// because only the unit has the text -- and it is only safe to touch while
// the workspace lock is held (workspace.h).
typedef struct {
    int line;
    int character;
    cJSON *result;
} HoverRequest;

static void collect(void *context, const LhatUnit *unit)
{
    HoverRequest *request = (HoverRequest *)context;
    uint32_t offset =
        lsp_unit_offset_at(unit, request->line, request->character);
    request->result = lsp_hover_for_unit(unit, offset);
}

cJSON *lsp_handle_hover(LspServer *server, const cJSON *params)
{
    if (params == NULL) {
        return NULL;
    }
    const cJSON *text_document =
        cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    const cJSON *position = cJSON_GetObjectItemCaseSensitive(params, "position");
    if (text_document == NULL || position == NULL) {
        return NULL;
    }
    const cJSON *uri_item = cJSON_GetObjectItemCaseSensitive(text_document, "uri");
    const cJSON *line = cJSON_GetObjectItemCaseSensitive(position, "line");
    const cJSON *character =
        cJSON_GetObjectItemCaseSensitive(position, "character");
    if (!cJSON_IsString(uri_item) || !cJSON_IsNumber(line) ||
        !cJSON_IsNumber(character)) {
        return NULL;
    }

    char *path = lsp_uri_to_absolute_path(uri_item->valuestring);
    if (path == NULL) {
        return NULL;
    }

    HoverRequest request = {line->valueint, character->valueint, NULL};
    lsp_workspace_with_unit(&server->workspace, path, collect, &request);
    free(path);

    return request.result;  // NULL is a JSON null, which LSP reads as "nothing"
}
