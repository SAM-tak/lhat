// L^ (lhat) -- LSP server: textDocument/documentSymbol.

#ifndef LSP_HANDLERS_DOCUMENT_SYMBOL_H
#define LSP_HANDLERS_DOCUMENT_SYMBOL_H

#include "cJSON.h"

typedef struct LspServer LspServer;

cJSON *lsp_handle_document_symbol(LspServer *server, const cJSON *params);

#endif  // LSP_HANDLERS_DOCUMENT_SYMBOL_H
