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

    const cJSON *text_item = cJSON_GetObjectItemCaseSensitive(text_document, "text");
    if (cJSON_IsString(text_item)) {
        const cJSON *version_item =
            cJSON_GetObjectItemCaseSensitive(text_document, "version");
        int version = cJSON_IsNumber(version_item) ? version_item->valueint : 0;
        char *text = lsp_strdup(text_item->valuestring);
        lsp_document_store_put(&server->workspace.documents, path, text,
                               strlen(text_item->valuestring), version);
    }
    lsp_queue_mark_dirty(&server->queue, path);
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
        const cJSON *text_item = cJSON_GetObjectItemCaseSensitive(last, "text");
        if (cJSON_IsString(text_item)) {
            const cJSON *version_item =
                cJSON_GetObjectItemCaseSensitive(text_document, "version");
            int version =
                cJSON_IsNumber(version_item) ? version_item->valueint : 0;
            char *text = lsp_strdup(text_item->valuestring);
            lsp_document_store_put(&server->workspace.documents, path, text,
                                   strlen(text_item->valuestring), version);
        }
    }
    lsp_queue_mark_dirty(&server->queue, path);
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
    lsp_queue_mark_dirty(&server->queue, path);  // re-evaluate against disk
    free(path);
}
