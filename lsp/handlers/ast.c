// L^ (lhat) -- LSP server: lhat/ast.
//
// An extension of our own, so it is named under "lhat/" (07 の 6 章). The
// visual editor asks for the tree of one unit rather than parsing L^ itself.

#include "ast.h"

#include <stdlib.h>

// The serialiser, disambiguated from this handler's own header of the same
// basename -- a bare "ast_json.h" would resolve to lsp/handlers/ first if one
// ever appeared there, the way semantic_tokens.c already has to say.
#include "../ast_json.h"

#include "server.h"
#include "uri.h"
#include "workspace.h"

static void collect(void *context, const LhatUnit *unit)
{
    cJSON **out = (cJSON **)context;
    *out = lsp_ast_json_for_unit(unit);
}

cJSON *lsp_handle_ast(LspServer *server, const cJSON *params)
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

    // Held under the workspace lock: the worker thread rebuilds a root's whole
    // LhatProgram on the next recheck, so the tree must be serialised before
    // this returns rather than handed back as a pointer (workspace.h).
    cJSON *tree = NULL;
    lsp_workspace_with_unit(&server->workspace, path, collect, &tree);
    free(path);

    // Not part of any checked root yet -- a JSON null result, which the editor
    // reads as "ask again once diagnostics have arrived".
    return tree;
}
