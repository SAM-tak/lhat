// L^ (lhat) -- LSP server: the method tables dispatch.c walks.
//
// A new request or notification is one entry here plus one handlers/*.c --
// dispatch.c and server.c stay unchanged.

#include "dispatch.h"

#include "handlers/initialize.h"
#include "handlers/text_document_sync.h"

const LspRequestEntry LSP_REQUEST_TABLE[] = {
    {"initialize", lsp_handle_initialize},
    {"shutdown", lsp_handle_shutdown},
    {NULL, NULL},
};

const LspNotificationEntry LSP_NOTIFICATION_TABLE[] = {
    {"initialized", lsp_handle_initialized},
    {"exit", lsp_handle_exit},
    {"textDocument/didOpen", lsp_handle_did_open},
    {"textDocument/didChange", lsp_handle_did_change},
    {"textDocument/didClose", lsp_handle_did_close},
    {NULL, NULL},
};
