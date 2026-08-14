// L^ (lhat) -- LSP server: lhat/signature (07 の 4 章).

#include "signature.h"

#include <stdlib.h>

// The answer itself, disambiguated from this handler's own header of the
// same basename -- a bare "signature.h" resolves to this file's neighbour
// first, the way hover.c already has to say.
#include "../signature.h"

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
    char *signature;  // owned; NULL when nothing there names a type
} SignatureRequest;

static void collect(void *context, const LhatUnit *unit)
{
    SignatureRequest *request = (SignatureRequest *)context;
    uint32_t offset = lsp_offset_at(unit->source.text, unit->source.length,
                                    request->line, request->character);
    request->signature = lsp_signature_for_unit(unit, offset);
}

cJSON *lsp_handle_signature(LspServer *server, const cJSON *params)
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

    SignatureRequest request = {line->valueint, character->valueint, NULL};
    lsp_workspace_with_unit(&server->workspace, path, collect, &request);
    free(path);

    if (request.signature == NULL) {
        return NULL;  // a JSON null: nothing is named there
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "signature", request.signature);
    free(request.signature);
    return result;
}
