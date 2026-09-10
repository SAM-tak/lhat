// L^ (lhat) -- LSP server: initialize / initialized / shutdown / exit.

#include "initialize.h"

#include <stdlib.h>
#include <string.h>

#include "../semantic_tokens.h"  // LSP_SEMANTIC_TOKEN_TYPES/MODIFIERS

#include "queue.h"
#include "server.h"
#include "uri.h"

static cJSON *string_array(const char *const *items, size_t count)
{
    cJSON *array = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON_AddItemToArray(array, cJSON_CreateString(items[i]));
    }
    return array;
}

typedef struct {
    char **paths;
    size_t count;
    size_t capacity;
} WorkspacePaths;

static void workspace_paths_dispose(WorkspacePaths *paths)
{
    for (size_t i = 0; i < paths->count; i++) {
        free(paths->paths[i]);
    }
    free(paths->paths);
}

static void workspace_paths_add(WorkspacePaths *paths, const char *uri)
{
    char *path = lsp_uri_to_absolute_path(uri);
    if (path == NULL) {
        return;
    }
    for (size_t i = 0; i < paths->count; i++) {
        if (strcmp(paths->paths[i], path) == 0) {
            free(path);
            return;
        }
    }
    if (paths->count == paths->capacity) {
        size_t grown = paths->capacity ? paths->capacity * 2 : 4;
        char **bigger = (char **)realloc(paths->paths, grown * sizeof *bigger);
        if (bigger == NULL) {
            free(path);
            return;
        }
        paths->paths = bigger;
        paths->capacity = grown;
    }
    paths->paths[paths->count++] = path;
}

// workspaceFolders is the current LSP representation: rootUri is only the
// older single-folder fallback. Keep every valid folder, rather than silently
// treating the first one as the whole editor workspace.
static WorkspacePaths read_workspace_paths(const cJSON *params)
{
    WorkspacePaths paths = {0};
    if (params == NULL) {
        return paths;
    }
    const cJSON *folders =
        cJSON_GetObjectItemCaseSensitive(params, "workspaceFolders");
    if (cJSON_IsArray(folders)) {
        cJSON *folder = NULL;
        cJSON_ArrayForEach(folder, folders) {
            const cJSON *uri = cJSON_GetObjectItemCaseSensitive(folder, "uri");
            if (cJSON_IsString(uri)) {
                workspace_paths_add(&paths, uri->valuestring);
            }
        }
    }
    if (paths.count == 0) {
        const cJSON *root_uri =
            cJSON_GetObjectItemCaseSensitive(params, "rootUri");
        if (cJSON_IsString(root_uri)) {
            workspace_paths_add(&paths, root_uri->valuestring);
        }
    }
    return paths;
}

cJSON *lsp_handle_initialize(LspServer *server, const cJSON *params)
{
    WorkspacePaths paths = read_workspace_paths(params);

    // lsp_server_init already gave the workspace an empty start; this is the
    // one point the client folders replace it.
    lsp_workspace_dispose(&server->workspace);
    lsp_workspace_init(&server->workspace, (const char *const *)paths.paths,
                       paths.count);
    workspace_paths_dispose(&paths);

    cJSON *result = cJSON_CreateObject();
    cJSON *capabilities = cJSON_CreateObject();
    cJSON *sync = cJSON_CreateObject();
    cJSON_AddBoolToObject(sync, "openClose", true);
    cJSON_AddNumberToObject(sync, "change", 1);  // Full document sync
    cJSON_AddItemToObject(capabilities, "textDocumentSync", sync);

    // semantic_tokens.c's legend -- the token type/modifier indices it
    // emits are indices into these same two arrays, so this has to echo
    // them exactly rather than write its own copy.
    cJSON *semantic_tokens = cJSON_CreateObject();
    cJSON *legend = cJSON_CreateObject();
    cJSON_AddItemToObject(legend, "tokenTypes",
        string_array(LSP_SEMANTIC_TOKEN_TYPES, LSP_SEMANTIC_TOKEN_TYPES_COUNT));
    cJSON_AddItemToObject(legend, "tokenModifiers",
        string_array(LSP_SEMANTIC_TOKEN_MODIFIERS,
                     LSP_SEMANTIC_TOKEN_MODIFIERS_COUNT));
    cJSON_AddItemToObject(semantic_tokens, "legend", legend);
    cJSON_AddBoolToObject(semantic_tokens, "full", true);
    cJSON_AddItemToObject(capabilities, "semanticTokensProvider", semantic_tokens);

    // 07 の 4 章: the definition a name reaches, and what was written about it.
    cJSON_AddBoolToObject(capabilities, "hoverProvider", true);
    // 07 の 4 章: the checker recorded where every name it resolved was
    // declared, which is the whole of what going to one needs.
    cJSON_AddBoolToObject(capabilities, "definitionProvider", true);
    // 07 の 4 章: what may stand where the cursor is. The dot is the one
    // character that starts it on its own; the quote is what opens a
    // require^'s path. Anything else is the editor asking on demand.
    cJSON *completion = cJSON_CreateObject();
    cJSON_AddItemToObject(capabilities, "completionProvider", completion);
    cJSON_AddBoolToObject(completion, "resolveProvider", false);
    static const char *const triggers[] = {".", "\""};
    cJSON_AddItemToObject(completion, "triggerCharacters",
                          string_array(triggers, 2));

    // The outline, from the tree alone (document_symbol.h).
    cJSON_AddBoolToObject(capabilities, "documentSymbolProvider", true);

    // 07 の 5 章: the same record read the other way round -- every use
    // pointing at one declaration rather than one use pointing at its.
    cJSON_AddBoolToObject(capabilities, "referencesProvider", true);
    // prepareRename is answered too, so a client asks whether a rename is
    // possible before it opens a box: what the host registered and what
    // the language answers itself were declared nowhere this can edit.
    cJSON *rename = cJSON_CreateObject();
    cJSON_AddItemToObject(capabilities, "renameProvider", rename);
    cJSON_AddBoolToObject(rename, "prepareProvider", true);

    cJSON *workspace = cJSON_CreateObject();
    cJSON *folders = cJSON_CreateObject();
    cJSON_AddBoolToObject(folders, "supported", true);
    // Folder changes are not registered yet; the initialized list is fully
    // supported, and the capability must not promise more than that.
    cJSON_AddBoolToObject(folders, "changeNotifications", false);
    cJSON_AddItemToObject(workspace, "workspaceFolders", folders);
    cJSON_AddItemToObject(capabilities, "workspace", workspace);

    cJSON_AddItemToObject(result, "capabilities", capabilities);

    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", "lhatls");
    cJSON_AddItemToObject(result, "serverInfo", info);
    return result;
}

// Everything slow happens on the worker (worker.c), not here: this walks the
// folders only to find project boundaries and roots (I/O, but no
// lhat_program_check). The worker checks them all as its first act.
void lsp_handle_initialized(LspServer *server, const cJSON *params)
{
    (void)params;
    lsp_workspace_discover_projects(&server->workspace);
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
