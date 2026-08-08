// L^ (lhat) -- LSP server: textDocument/semanticTokens/full.

#include "semantic_tokens.h"

#include <stdlib.h>

// The token-building logic, disambiguated from this handler's own header of
// the same basename -- a bare "semantic_tokens.h" would resolve to this
// file's neighbour (lsp/handlers/semantic_tokens.h) first.
#include "../semantic_tokens.h"

#include "server.h"
#include "uri.h"
#include "workspace.h"

static void collect(void *context, const LhatUnit *unit)
{
    cJSON **out = (cJSON **)context;
    *out = lsp_semantic_tokens_for_unit(unit);
}

cJSON *lsp_handle_semantic_tokens_full(LspServer *server, const cJSON *params)
{
    if (params == NULL) {
        return NULL;
    }
    const cJSON *text_document =
        cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    if (text_document == NULL) {
        return NULL;
    }
    const cJSON *uri_item = cJSON_GetObjectItemCaseSensitive(text_document, "uri");
    if (!cJSON_IsString(uri_item)) {
        return NULL;
    }

    char *path = lsp_uri_to_absolute_path(uri_item->valuestring);
    if (path == NULL) {
        return NULL;
    }

    cJSON *data = NULL;
    lsp_workspace_with_unit(&server->workspace, path, collect, &data);
    free(path);

    if (data == NULL) {
        return NULL;  // not (yet) checked -- a JSON null result
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "data", data);
    return result;
}
