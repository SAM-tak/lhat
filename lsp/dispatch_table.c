// L^ (lhat) -- LSP server: the method tables dispatch.c walks.
//
// A new request or notification is one entry here plus one handlers/*.c --
// dispatch.c and server.c stay unchanged.

#include "dispatch.h"

#include "handlers/ast.h"
#include "handlers/hover.h"
#include "handlers/initialize.h"
#include "handlers/semantic_tokens.h"
#include "handlers/text_document_sync.h"

const LspRequestEntry LSP_REQUEST_TABLE[] = {
    {"initialize", lsp_handle_initialize},
    {"shutdown", lsp_handle_shutdown},
    {"textDocument/semanticTokens/full", lsp_handle_semantic_tokens_full},
    {"textDocument/hover", lsp_handle_hover},
    // 07 の 6 章: an extension of our own, so it is named under "lhat/".
    {"lhat/ast", lsp_handle_ast},
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
