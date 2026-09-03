// L^ (lhat) -- LSP server: textDocument/hover.

#include "hover.h"

#include <stdlib.h>
#include <string.h>

// The hover-building logic, disambiguated from this handler's own header of
// the same basename -- a bare "hover.h" resolves to this file's neighbour
// first, the way semantic_tokens.c already has to say.
#include "../hover.h"

#include "position.h"
#include "server.h"
#include "uri.h"
#include "workspace.h"

// The two halves are asked of two different units -- the one the cursor is
// in, and the one the definition was written in (hover.h) -- and
// lsp_workspace_with_unit takes the workspace lock itself, so they are two
// calls in a row rather than one inside the other, the way the definition
// handler already works. What crosses between them is the part: strings and
// offsets, no pointer into a unit the worker's next recheck may dispose.
typedef struct {
    int line;
    int character;
    bool located;
    LspHoverPart part;
} HoverRequest;

static void locate(void *context, const LhatUnit *unit)
{
    HoverRequest *request = (HoverRequest *)context;
    // The position is turned into an offset here rather than by the caller,
    // because only the unit has the text.
    uint32_t offset =
        lsp_unit_offset_at(unit, request->line, request->character);
    request->located = lsp_hover_locate(unit, offset, &request->part);
}

static void describe(void *context, const LhatUnit *unit)
{
    lsp_hover_describe(unit, (LspHoverPart *)context);
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

    HoverRequest request;
    memset(&request, 0, sizeof request);
    request.line = line->valueint;
    request.character = character->valueint;
    lsp_workspace_with_unit(&server->workspace, path, locate, &request);
    free(path);
    if (!request.located) {
        lsp_hover_part_dispose(&request.part);
        return NULL;  // a JSON null, which LSP reads as "nothing"
    }

    // A definition in another unit: its line and comments come from there.
    // A unit no checked root reaches leaves the sink uncalled (workspace.h),
    // and the hover then shows the type alone rather than a line from the
    // wrong file.
    if (request.part.definition_path != NULL) {
        lsp_workspace_with_unit(&server->workspace,
                                request.part.definition_path, describe,
                                &request.part);
    }
    cJSON *result = lsp_hover_render(&request.part);
    lsp_hover_part_dispose(&request.part);
    return result;
}
