// L^ (lhat) -- LSP server: initialize / initialized / shutdown / exit.

#include "initialize.h"

#include <stdlib.h>

#include "queue.h"
#include "server.h"
#include "uri.h"

static char *read_root_path(const cJSON *params)
{
    if (params == NULL) {
        return NULL;
    }
    const cJSON *root_uri = cJSON_GetObjectItemCaseSensitive(params, "rootUri");
    if (cJSON_IsString(root_uri)) {
        return lsp_uri_to_absolute_path(root_uri->valuestring);
    }
    const cJSON *folders =
        cJSON_GetObjectItemCaseSensitive(params, "workspaceFolders");
    if (cJSON_IsArray(folders) && cJSON_GetArraySize(folders) > 0) {
        const cJSON *first = cJSON_GetArrayItem(folders, 0);
        const cJSON *uri = cJSON_GetObjectItemCaseSensitive(first, "uri");
        if (cJSON_IsString(uri)) {
            return lsp_uri_to_absolute_path(uri->valuestring);
        }
    }
    return NULL;
}

cJSON *lsp_handle_initialize(LspServer *server, const cJSON *params)
{
    char *root_path = read_root_path(params);

    // lsp_server_init already gave the workspace an empty (root_path ==
    // NULL) start; this is the one point a real root can replace it.
    lsp_workspace_dispose(&server->workspace);
    lsp_workspace_init(&server->workspace, root_path);
    free(root_path);

    cJSON *result = cJSON_CreateObject();
    cJSON *capabilities = cJSON_CreateObject();
    cJSON *sync = cJSON_CreateObject();
    cJSON_AddBoolToObject(sync, "openClose", true);
    cJSON_AddNumberToObject(sync, "change", 1);  // Full document sync
    cJSON_AddItemToObject(capabilities, "textDocumentSync", sync);
    cJSON_AddItemToObject(result, "capabilities", capabilities);

    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", "lhat-lsp");
    cJSON_AddItemToObject(result, "serverInfo", info);
    return result;
}

// Everything slow happens on the worker (worker.c), not here: this walks
// the workspace only to find *.lh files (I/O, but no lhat_program_check).
// The worker checks them all (lsp_workspace_recheck_all) as its first act
// once started.
void lsp_handle_initialized(LspServer *server, const cJSON *params)
{
    (void)params;
    lsp_workspace_discover_roots(&server->workspace);
    lsp_server_start_worker(server);
}

cJSON *lsp_handle_shutdown(LspServer *server, const cJSON *params)
{
    (void)params;
    server->shutdown_requested = true;
    return NULL;  // a JSON null result, per the spec
}

void lsp_handle_exit(LspServer *server, const cJSON *params)
{
    (void)params;
    server->should_exit = true;
    lsp_queue_shutdown(&server->queue);
}
