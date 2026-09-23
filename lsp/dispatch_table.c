// L^ (lhat) -- LSP server: the method tables dispatch.c walks.
//
// A new request or notification is one entry here plus one handlers/*.c --
// dispatch.c and server.c stay unchanged.

#include "dispatch.h"

#include "handlers/ast.h"
#include "handlers/code_action.h"
#include "handlers/completion.h"
#include "handlers/definition.h"
#include "handlers/disabled_code.h"
#include "handlers/document_symbol.h"
#include "handlers/hover.h"
#include "handlers/initialize.h"
#include "handlers/semantic_tokens.h"
#include "handlers/references.h"
#include "handlers/signature.h"
#include "handlers/text_document_sync.h"

const LspRequestEntry LSP_REQUEST_TABLE[] = {
    {"initialize", lsp_handle_initialize},
    {"shutdown", lsp_handle_shutdown},
    {"textDocument/semanticTokens/full", lsp_handle_semantic_tokens_full},
    {"textDocument/hover", lsp_handle_hover},
    {"textDocument/definition", lsp_handle_definition},
    {"textDocument/documentSymbol", lsp_handle_document_symbol},
    {"textDocument/references", lsp_handle_references},
    {"textDocument/prepareRename", lsp_handle_prepare_rename},
    {"textDocument/rename", lsp_handle_rename},
    {"textDocument/completion", lsp_handle_completion},
    {"textDocument/codeAction", lsp_handle_code_action},
    // 07 の 7 章: an extension of our own, so it is named under "lhat/".
    {"lhat/ast", lsp_handle_ast},
    {"lhat/typeOptions", lsp_handle_type_options},
    {"lhat/signature", lsp_handle_signature},
    {"lhat/toggleDisabledCode", lsp_handle_toggle_disabled_code},
    {NULL, NULL},
};

const LspNotificationEntry LSP_NOTIFICATION_TABLE[] = {
    {"initialized", lsp_handle_initialized},
    {"exit", lsp_handle_exit},
    {"textDocument/didOpen", lsp_handle_did_open},
    {"textDocument/didChange", lsp_handle_did_change},
    {"textDocument/didClose", lsp_handle_did_close},
    {"workspace/didChangeWatchedFiles", lsp_handle_did_change_watched_files},
    {NULL, NULL},
};
