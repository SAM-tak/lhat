// L^ (lhat) -- LSP server: didOpen / didChange / didClose.
//
// Every handler here only updates document_store and marks a path dirty --
// lhat_program_check never runs on this (the main) thread. The worker
// (worker.c) picks the path up off the queue.

#include "text_document_sync.h"

#include <stdlib.h>
#include <string.h>

#include "document_store.h"
#include "queue.h"
#include "server.h"
#include "uri.h"
#include "util.h"
#include "workspace.h"

static char *path_from_text_document(const cJSON *text_document)
{
    if (text_document == NULL) {
        return NULL;
    }
    const cJSON *uri = cJSON_GetObjectItemCaseSensitive(text_document, "uri");
    if (!cJSON_IsString(uri)) {
        return NULL;
    }
    return lsp_uri_to_absolute_path(uri->valuestring);
}

// A path worth waking the worker for: a unit, or the workspace's
// lhat-host.json, which the worker answers by reloading the host config
// (worker.c). Anything else the editor opens -- a README, this server's
// own sources -- has nothing for the checker and stays out of the queue.
static bool worth_rechecking(LspServer *server, const char *path)
{
    return lsp_workspace_is_unit_path(path) ||
           lsp_workspace_is_host_config_path(&server->workspace, path);
}

// The editor's copy of `path`, kept for everything downstream to read
// instead of the disk -- which is the whole of what this store is for
// (document_store.h).
//
// 05 の 10 章: except for a compiled unit. One is a *.lh like any other and
// an editor will happily show it, but what it shows has been through a
// decoder and the magic -- a byte no UTF-8 text can start on -- does not
// survive it. Kept, that text would reach the front end as a source file
// and be reported on line after line, while the file itself holds a unit
// the checker has nothing to say about. So it is not kept, and every reader
// falls through to the bytes on disk, where the library still recognises
// them (lsp_program_load, and program.c's own load). A copy already held
// goes too: the file may have been a source one when it was opened and been
// compiled over since.
static void remember_text(LspServer *server, const char *path,
                          const cJSON *text_item, const cJSON *version_item)
{
    if (!cJSON_IsString(text_item)) {
        return;
    }
    if (lsp_workspace_is_binary_unit(path)) {
        lsp_document_store_remove(&server->workspace.documents, path);
        return;
    }
    int version = cJSON_IsNumber(version_item) ? version_item->valueint : 0;
    char *text = lsp_strdup(text_item->valuestring);
    lsp_document_store_put(&server->workspace.documents, path, text,
                           strlen(text_item->valuestring), version);
}

void lsp_handle_did_open(LspServer *server, const cJSON *params)
{
    if (params == NULL) {
        return;
    }
    const cJSON *text_document =
        cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    char *path = path_from_text_document(text_document);
    if (path == NULL) {
        return;
    }

    remember_text(server, path,
                  cJSON_GetObjectItemCaseSensitive(text_document, "text"),
                  cJSON_GetObjectItemCaseSensitive(text_document, "version"));
    if (worth_rechecking(server, path)) {
        lsp_queue_mark_dirty(&server->queue, path);
    }
    free(path);
}

void lsp_handle_did_change(LspServer *server, const cJSON *params)
{
    if (params == NULL) {
        return;
    }
    const cJSON *text_document =
        cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    char *path = path_from_text_document(text_document);
    if (path == NULL) {
        return;
    }

    const cJSON *changes =
        cJSON_GetObjectItemCaseSensitive(params, "contentChanges");
    if (cJSON_IsArray(changes) && cJSON_GetArraySize(changes) > 0) {
        // Full sync (capabilities.textDocumentSync.change == 1, initialize.c):
        // the last entry's "text" is the whole new document, not a delta.
        const cJSON *last =
            cJSON_GetArrayItem(changes, cJSON_GetArraySize(changes) - 1);
        remember_text(server, path,
                      cJSON_GetObjectItemCaseSensitive(last, "text"),
                      cJSON_GetObjectItemCaseSensitive(text_document,
                                                       "version"));
    }
    if (worth_rechecking(server, path)) {
        lsp_queue_mark_dirty(&server->queue, path);
    }
    free(path);
}

void lsp_handle_did_close(LspServer *server, const cJSON *params)
{
    if (params == NULL) {
        return;
    }
    const cJSON *text_document =
        cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    char *path = path_from_text_document(text_document);
    if (path == NULL) {
        return;
    }

    lsp_document_store_remove(&server->workspace.documents, path);
    if (worth_rechecking(server, path)) {
        lsp_queue_mark_dirty(&server->queue, path);  // re-evaluate off disk
    }
    free(path);
}
