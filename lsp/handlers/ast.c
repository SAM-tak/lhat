// L^ (lhat) -- LSP server: lhat/ast.
//
// An extension of our own, so it is named under "lhat/" (07 の 7 章). The
// visual editor asks for the tree of one unit rather than parsing L^ itself.

#include "ast.h"

#include <stdlib.h>

// The serialiser, disambiguated from this handler's own header of the same
// basename -- a bare "ast_json.h" would resolve to lsp/handlers/ first if one
// ever appeared there, the way semantic_tokens.c already has to say.
#include "../ast_json.h"
#include "../graph_types.h"
#include "position.h"

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

typedef struct { int line; int character; int result_index; cJSON *reply; } TypeRequest;
static void collect_types(void *context, const LhatUnit *unit)
{
    TypeRequest *request = context;
    request->reply = lsp_graph_type_options_result(unit, lsp_unit_offset_at(unit, request->line, request->character), request->result_index);
}

cJSON *lsp_handle_type_options(LspServer *server, const cJSON *params)
{
    const cJSON *document = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    const cJSON *uri = cJSON_GetObjectItemCaseSensitive(document, "uri");
    const cJSON *position = cJSON_GetObjectItemCaseSensitive(params, "position");
    const cJSON *line = cJSON_GetObjectItemCaseSensitive(position, "line");
    const cJSON *character = cJSON_GetObjectItemCaseSensitive(position, "character");
    if (!cJSON_IsString(uri) || !cJSON_IsNumber(line) || !cJSON_IsNumber(character) || line->valueint < 0 || character->valueint < 0) return NULL;
    char *path = lsp_uri_to_absolute_path(uri->valuestring);
    if (!path) return NULL;
    const cJSON *result_index = cJSON_GetObjectItemCaseSensitive(params, "resultIndex");
    if (result_index && (!cJSON_IsNumber(result_index) || result_index->valueint < 0 || result_index->valuedouble != result_index->valueint)) { free(path); return NULL; }
    TypeRequest request = {line->valueint, character->valueint, result_index ? result_index->valueint : -1, NULL};
    lsp_workspace_with_unit(&server->workspace, path, collect_types, &request);
    free(path);
    return request.reply;
}
